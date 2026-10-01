// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Parser for the OneNote 2010+ desktop revision store file format
// ([MS-ONESTORE] sections 2.3 - 2.6).
#include <limits>
#include <set>

#include "../util/log.hpp"
#include "../util/text.hpp"
#include "store.hpp"

namespace oneconv {

namespace {

const Guid kGuidOneFile = Guid::from_string("{7B5C52E4-D88C-4DA7-AEB1-5378D02996D3}");
const Guid kGuidTocFile = Guid::from_string("{43FF2FA1-EFD9-4C76-9EE2-10EA5722765F}");
const Guid kGuidFileDataHeader = Guid::from_string("{BDE316E7-2665-4511-A4C4-8D4D0B7A9EAC}");

constexpr uint64_t kFragmentMagic = 0xA4567AB1F5F7F4C4ULL;
constexpr uint64_t kFragmentFooter = 0x8BC215C38233BA4BULL;

// File node IDs (2.4.3)
enum : uint32_t {
    ObjectSpaceManifestRootFND = 0x004,
    ObjectSpaceManifestListReferenceFND = 0x008,
    ObjectSpaceManifestListStartFND = 0x00C,
    RevisionManifestListReferenceFND = 0x010,
    RevisionManifestListStartFND = 0x014,
    RevisionManifestStart4FND = 0x01B,
    RevisionManifestEndFND = 0x01C,
    RevisionManifestStart6FND = 0x01E,
    RevisionManifestStart7FND = 0x01F,
    GlobalIdTableStartFNDX = 0x021,
    GlobalIdTableStart2FND = 0x022,
    GlobalIdTableEntryFNDX = 0x024,
    GlobalIdTableEntry2FNDX = 0x025,
    GlobalIdTableEntry3FNDX = 0x026,
    GlobalIdTableEndFNDX = 0x028,
    ObjectDeclarationWithRefCountFNDX = 0x02D,
    ObjectDeclarationWithRefCount2FNDX = 0x02E,
    ObjectRevisionWithRefCountFNDX = 0x041,
    ObjectRevisionWithRefCount2FNDX = 0x042,
    RootObjectReference2FNDX = 0x059,
    RootObjectReference3FND = 0x05A,
    RevisionRoleDeclarationFND = 0x05C,
    RevisionRoleAndContextDeclarationFND = 0x05D,
    ObjectDeclarationFileData3RefCountFND = 0x072,
    ObjectDeclarationFileData3LargeRefCountFND = 0x073,
    ObjectDataEncryptionKeyV2FNDX = 0x07C,
    ObjectInfoDependencyOverridesFND = 0x084,
    DataSignatureGroupDefinitionFND = 0x08C,
    FileDataStoreListReferenceFND = 0x090,
    FileDataStoreObjectReferenceFND = 0x094,
    ObjectDeclaration2RefCountFND = 0x0A4,
    ObjectDeclaration2LargeRefCountFND = 0x0A5,
    ObjectGroupListReferenceFND = 0x0B0,
    ObjectGroupStartFND = 0x0B4,
    ObjectGroupEndFND = 0x0B8,
    HashedChunkDescriptor2FND = 0x0C2,
    ReadOnlyObjectDeclaration2RefCountFND = 0x0C4,
    ReadOnlyObjectDeclaration2LargeRefCountFND = 0x0C5,
    ChunkTerminatorFND = 0x0FF,
};

struct ChunkRef {
    uint64_t stp = 0;
    uint64_t cb = 0;
    bool nil = false;
    bool valid() const { return !nil && !(stp == 0 && cb == 0); }
};

ChunkRef read_ref64x32(Reader& r) {
    ChunkRef c;
    c.stp = r.u64();
    c.cb = r.u32();
    c.nil = (c.stp == std::numeric_limits<uint64_t>::max() && c.cb == 0);
    return c;
}

/// FileNodeChunkReference (2.2.4.2) with variable-size encodings.
ChunkRef read_node_ref(Reader& r, uint32_t stp_format, uint32_t cb_format) {
    ChunkRef c;
    bool stp_all_ones = false;
    switch (stp_format) {
        case 0: c.stp = r.u64(); stp_all_ones = c.stp == 0xFFFFFFFFFFFFFFFFULL; break;
        case 1: c.stp = r.u32(); stp_all_ones = c.stp == 0xFFFFFFFFULL; break;
        case 2: c.stp = r.u16(); stp_all_ones = c.stp == 0xFFFF; c.stp *= 8; break;
        case 3: c.stp = r.u32(); stp_all_ones = c.stp == 0xFFFFFFFFULL; c.stp *= 8; break;
    }
    switch (cb_format) {
        case 0: c.cb = r.u32(); break;
        case 1: c.cb = r.u64(); break;
        case 2: c.cb = static_cast<uint64_t>(r.u8()) * 8; break;
        case 3: c.cb = static_cast<uint64_t>(r.u16()) * 8; break;
    }
    c.nil = stp_all_ones && c.cb == 0;
    return c;
}

struct FileNode {
    uint32_t id = 0;
    uint32_t base_type = 0;
    ChunkRef ref;
    Reader body;  // node data following the chunk reference
};

/// Maps CompactID guid indexes to GUIDs (a global identification table, 2.4.3).
struct IdMap {
    std::map<uint32_t, Guid> table;
    ExGuid resolve(const CompactId& c) const {
        auto it = table.find(c.guid_index);
        if (it == table.end()) {
            if (!c.is_zero()) Log::debug(cat("unresolved compact id index ", c.guid_index));
            return ExGuid();
        }
        return ExGuid(it->second, c.n);
    }
};

struct Revision {
    ExGuid rid;
    ExGuid dependent;
    ExGuid context;
    uint32_t role = 0;
    std::map<uint32_t, ExGuid> roots;
    std::map<ExGuid, std::shared_ptr<const Object>> objects;
    IdMap ids;  // union of all id tables seen in this revision and its dependencies
};

class DesktopParser {
public:
    DesktopParser(BufferPtr data, const StoreOptions& opts) : buf_(std::move(data)), file_(buf_), opts_(opts) {}

    Store parse() {
        Reader h = file_.slice(0, 1024);
        Guid file_type = Guid::parse(h);
        if (file_type == kGuidOneFile)
            store_.kind = StoreKind::Section;
        else if (file_type == kGuidTocFile)
            store_.kind = StoreKind::TableOfContents;
        else
            throw ParseError("not a OneNote file (unknown file type GUID)");
        store_.format = StoreFormat::Desktop;

        h.seek(96);
        uint32_t transactions_in_log = h.u32();
        h.seek(160);
        ChunkRef tx_log = read_ref64x32(h);
        ChunkRef root_list = read_ref64x32(h);

        read_transaction_log(tx_log, transactions_in_log);

        if (!root_list.valid()) throw ParseError("file has no root file node list");
        std::vector<FileNode> root = read_list(root_list);

        // File data store first: object declarations reference it.
        for (const auto& n : root)
            if (n.id == FileDataStoreListReferenceFND) read_file_data_store(n);

        bool have_root = false;
        for (const auto& n : root) {
            if (n.id == ObjectSpaceManifestRootFND) {
                Reader b = n.body;
                store_.root_space = CellId(ExGuid::parse_fixed(b));
                have_root = true;
            } else if (n.id == ObjectSpaceManifestListReferenceFND) {
                Reader b = n.body;
                ExGuid gosid = ExGuid::parse_fixed(b);
                try {
                    parse_object_space(gosid, n.ref);
                } catch (const ParseError& e) {
                    if (store_.encrypted) throw;
                    Log::warn(cat("skipping damaged object space ", gosid.str(), ": ", e.what()));
                }
            }
        }
        if (!have_root) throw ParseError("file has no root object space");
        return std::move(store_);
    }

private:
    BufferPtr buf_;
    Reader file_;
    StoreOptions opts_;
    Store store_;
    std::map<uint32_t, uint64_t> list_node_counts_;
    std::map<Guid, Blob> file_data_;

    // -- Low-level file structure ------------------------------------------------

    void read_transaction_log(ChunkRef ref, uint32_t transactions) {
        // Only the first `transactions` committed transactions are valid; each
        // transaction is terminated by a sentinel entry (srcID == 1).
        uint32_t seen = 0;
        std::set<uint64_t> visited;
        std::map<uint32_t, uint64_t> pending;
        while (ref.valid() && seen < transactions) {
            if (!visited.insert(ref.stp).second) break;
            if (ref.cb < 12) break;
            Reader frag = file_.slice(ref.stp, ref.cb);
            size_t entries = (ref.cb - 12) / 8;
            for (size_t i = 0; i < entries && seen < transactions; ++i) {
                uint32_t src = frag.u32();
                uint32_t sw = frag.u32();
                if (src == 1) {
                    for (const auto& kv : pending) list_node_counts_[kv.first] = kv.second;
                    pending.clear();
                    ++seen;
                } else if (src != 0) {
                    pending[src] = sw;
                }
            }
            frag.seek(ref.cb - 12);
            ref = read_ref64x32(frag);
        }
    }

    /// Read a complete file node list, following fragments (2.4.1).
    std::vector<FileNode> read_list(ChunkRef ref) {
        std::vector<FileNode> nodes;
        uint32_t expected_seq = 0;
        uint64_t remaining = std::numeric_limits<uint64_t>::max();
        bool first = true;
        std::set<uint64_t> visited;
        while (ref.valid()) {
            if (!visited.insert(ref.stp).second) throw ParseError("cycle in file node list fragments");
            if (ref.cb < 36) throw ParseError("file node list fragment too small");
            Reader frag = file_.slice(ref.stp, ref.cb);
            if (frag.u64() != kFragmentMagic) throw ParseError("bad file node list fragment magic");
            uint32_t list_id = frag.u32();
            uint32_t seq = frag.u32();
            if (seq != expected_seq) Log::debug(cat("unexpected fragment sequence ", seq, " in list ", list_id));
            expected_seq = seq + 1;
            if (first) {
                auto it = list_node_counts_.find(list_id);
                if (it != list_node_counts_.end()) remaining = it->second;
                first = false;
            }
            const size_t nodes_end = static_cast<size_t>(ref.cb) - 20;
            while (remaining > 0 && frag.position() + 4 <= nodes_end) {
                size_t start = frag.position();
                uint32_t header = frag.u32();
                if (header == 0) continue;  // padding
                FileNode node;
                node.id = header & 0x3FF;
                uint32_t size = (header >> 10) & 0x1FFF;
                uint32_t stp_format = (header >> 23) & 0x3;
                uint32_t cb_format = (header >> 25) & 0x3;
                node.base_type = (header >> 27) & 0xF;
                if (node.id == ChunkTerminatorFND) break;
                if (size < 4 || start + size > nodes_end) throw ParseError(cat("bad file node size ", size));
                Reader body = frag.slice(start + 4, size - 4);
                if (node.base_type == 1 || node.base_type == 2) node.ref = read_node_ref(body, stp_format, cb_format);
                node.body = body.take(body.remaining());
                nodes.push_back(std::move(node));
                --remaining;
                frag.seek(start + size);
            }
            if (remaining == 0) break;
            frag.seek(nodes_end);
            ref = read_ref64x32(frag);
            if (frag.u64() != kFragmentFooter) Log::debug("bad file node list fragment footer");
        }
        return nodes;
    }

    // -- File data store (2.5.21) -------------------------------------------------

    void read_file_data_store(const FileNode& n) {
        for (const auto& item : read_list(n.ref)) {
            if (item.id != FileDataStoreObjectReferenceFND) continue;
            Reader b = item.body;
            Guid id = Guid::parse(b);
            try {
                Reader obj = file_.slice(item.ref.stp, item.ref.cb);
                Guid header = Guid::parse(obj);
                if (header != kGuidFileDataHeader) throw ParseError("bad FileDataStoreObject header");
                uint64_t len = obj.u64();
                obj.skip(4 + 8);
                if (len > obj.remaining()) throw ParseError("FileDataStoreObject length out of range");
                file_data_[id] = obj.blob(static_cast<size_t>(len));
            } catch (const ParseError& e) {
                Log::warn(cat("unreadable embedded file data ", id.str(), ": ", e.what()));
            }
        }
    }

    Blob resolve_file_data(const std::string& ref, bool& missing) {
        missing = false;
        if (starts_with(ref, "<ifndf>")) {
            std::string g = ref.substr(7);
            try {
                auto it = file_data_.find(Guid::from_string(g));
                if (it != file_data_.end()) return it->second;
            } catch (const ParseError&) {
            }
            Log::warn("embedded file data " + g + " not found");
        } else if (starts_with(ref, "<file>")) {
            std::string name = ref.substr(6);
            if (opts_.load_external) {
                Blob b = opts_.load_external(name);
                if (!b.empty()) return b;
            }
            Log::warn("external file data '" + name + "' not found next to the section file");
        } else if (starts_with(ref, "<invfdo>")) {
            Log::warn("file data object marked invalid by OneNote");
        } else {
            Log::warn("unknown file data reference: " + ref);
        }
        missing = true;
        return Blob();
    }

    // -- Object spaces, revisions and objects -------------------------------------

    void parse_object_space(const ExGuid& gosid, const ChunkRef& ref) {
        std::vector<FileNode> list = read_list(ref);
        const FileNode* last_rev_list = nullptr;
        for (const auto& n : list)
            if (n.id == RevisionManifestListReferenceFND) last_rev_list = &n;
        if (!last_rev_list) throw ParseError("object space has no revision manifest list");

        std::vector<FileNode> rev_nodes = read_list(last_rev_list->ref);
        std::map<ExGuid, Revision> revisions;
        std::map<std::pair<ExGuid, uint32_t>, ExGuid> labels;  // (context, role) -> revision

        size_t i = 0;
        while (i < rev_nodes.size()) {
            const FileNode& n = rev_nodes[i];
            switch (n.id) {
                case RevisionManifestListStartFND: ++i; break;
                case RevisionManifestStart4FND:
                case RevisionManifestStart6FND:
                case RevisionManifestStart7FND: {
                    Revision rev = parse_revision(rev_nodes, i, revisions);
                    labels[{rev.context, rev.role}] = rev.rid;
                    ExGuid rid = rev.rid;
                    revisions[rid] = std::move(rev);
                    break;
                }
                case RevisionRoleDeclarationFND:
                case RevisionRoleAndContextDeclarationFND: {
                    Reader b = n.body;
                    ExGuid rid = ExGuid::parse_fixed(b);
                    uint32_t role = b.u32();
                    ExGuid ctx;
                    if (n.id == RevisionRoleAndContextDeclarationFND) ctx = ExGuid::parse_fixed(b);
                    if (revisions.count(rid)) labels[{ctx, role}] = rid;
                    ++i;
                    break;
                }
                default: ++i; break;
            }
        }

        auto active = labels.find({ExGuid(), RoleDefaultContent});
        if (active == labels.end()) throw ParseError("object space has no active revision");

        // Materialise the dependency chain, oldest first.
        std::vector<const Revision*> chain;
        ExGuid cur = active->second;
        std::set<ExGuid> seen;
        while (!cur.is_nil()) {
            if (!seen.insert(cur).second) throw ParseError("cyclic revision dependencies");
            auto it = revisions.find(cur);
            if (it == revisions.end()) {
                Log::warn("revision depends on a missing revision " + cur.str());
                break;
            }
            chain.push_back(&it->second);
            cur = it->second.dependent;
        }
        ObjectSpace space;
        space.id = CellId(gosid);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (const auto& r : (*it)->roots) space.roots[r.first] = r.second;
            for (const auto& o : (*it)->objects) space.objects[o.first] = o.second;
        }
        store_.spaces[space.id] = std::move(space);
    }

    Revision parse_revision(const std::vector<FileNode>& nodes, size_t& i, const std::map<ExGuid, Revision>& revisions) {
        Revision rev;
        {
            const FileNode& start = nodes[i];
            Reader b = start.body;
            rev.rid = ExGuid::parse_fixed(b);
            rev.dependent = ExGuid::parse_fixed(b);
            if (start.id == RevisionManifestStart4FND) b.skip(8);  // timeCreation
            rev.role = b.u32();
            b.skip(2);  // odcsDefault
            if (start.id == RevisionManifestStart7FND) rev.context = ExGuid::parse_fixed(b);
        }
        ++i;

        const Revision* parent = nullptr;
        if (!rev.dependent.is_nil()) {
            auto it = revisions.find(rev.dependent);
            if (it != revisions.end()) {
                parent = &it->second;
                rev.ids = parent->ids;
            } else {
                Log::debug("revision depends on undeclared revision " + rev.dependent.str());
            }
        }

        IdMap current;  // id table in effect for the following declarations
        bool have_table = false;

        while (i < nodes.size()) {
            const FileNode& n = nodes[i];
            if (n.id == RevisionManifestEndFND) {
                ++i;
                break;
            }
            switch (n.id) {
                case ObjectGroupListReferenceFND: parse_object_group(n.ref, rev); ++i; break;
                case GlobalIdTableStartFNDX:
                case GlobalIdTableStart2FND:
                    current = parse_id_table(nodes, i, parent ? &parent->ids : nullptr);
                    have_table = true;
                    for (const auto& kv : current.table) rev.ids.table[kv.first] = kv.second;
                    break;
                case RootObjectReference3FND: {
                    Reader b = n.body;
                    ExGuid oid = ExGuid::parse_fixed(b);
                    rev.roots[b.u32()] = oid;
                    ++i;
                    break;
                }
                case RootObjectReference2FNDX: {
                    Reader b = n.body;
                    CompactId cid = CompactId::parse(b);
                    uint32_t role = b.u32();
                    rev.roots[role] = (have_table ? current : rev.ids).resolve(cid);
                    ++i;
                    break;
                }
                case ObjectRevisionWithRefCountFNDX:
                case ObjectRevisionWithRefCount2FNDX:
                    apply_object_revision(n, have_table ? current : rev.ids, rev, revisions);
                    ++i;
                    break;
                case ObjectDataEncryptionKeyV2FNDX:
                    store_.encrypted = true;
                    ++i;
                    break;
                default:
                    if (is_declaration(n.id)) {
                        declare_object(n, have_table ? current : rev.ids, rev.objects);
                    }
                    ++i;  // DataSignatureGroupDefinitionFND, ObjectInfoDependencyOverridesFND, ...
                    break;
            }
        }
        return rev;
    }

    static bool is_declaration(uint32_t id) {
        switch (id) {
            case ObjectDeclarationWithRefCountFNDX:
            case ObjectDeclarationWithRefCount2FNDX:
            case ObjectDeclaration2RefCountFND:
            case ObjectDeclaration2LargeRefCountFND:
            case ReadOnlyObjectDeclaration2RefCountFND:
            case ReadOnlyObjectDeclaration2LargeRefCountFND:
            case ObjectDeclarationFileData3RefCountFND:
            case ObjectDeclarationFileData3LargeRefCountFND: return true;
            default: return false;
        }
    }

    IdMap parse_id_table(const std::vector<FileNode>& nodes, size_t& i, const IdMap* parent) {
        IdMap map;
        ++i;  // start node
        while (i < nodes.size()) {
            const FileNode& n = nodes[i];
            Reader b = n.body;
            if (n.id == GlobalIdTableEndFNDX) {
                ++i;
                break;
            }
            if (n.id == GlobalIdTableEntryFNDX) {
                uint32_t index = b.u32();
                map.table[index] = Guid::parse(b);
            } else if (n.id == GlobalIdTableEntry2FNDX) {
                uint32_t from = b.u32();
                uint32_t to = b.u32();
                if (parent) {
                    auto it = parent->table.find(from);
                    if (it != parent->table.end()) map.table[to] = it->second;
                }
            } else if (n.id == GlobalIdTableEntry3FNDX) {
                uint32_t from = b.u32();
                uint32_t count = b.u32();
                uint32_t to = b.u32();
                if (parent && count < 0x01000000) {
                    for (uint32_t k = 0; k < count; ++k) {
                        auto it = parent->table.find(from + k);
                        if (it != parent->table.end()) map.table[to + k] = it->second;
                    }
                }
            } else {
                break;  // unexpected node: table ended implicitly
            }
            ++i;
        }
        return map;
    }

    void parse_object_group(const ChunkRef& ref, Revision& rev) {
        std::vector<FileNode> nodes = read_list(ref);
        IdMap ids;
        size_t i = 0;
        while (i < nodes.size()) {
            const FileNode& n = nodes[i];
            if (n.id == ObjectGroupEndFND) break;
            if (n.id == GlobalIdTableStartFNDX || n.id == GlobalIdTableStart2FND) {
                ids = parse_id_table(nodes, i, nullptr);
                for (const auto& kv : ids.table) rev.ids.table[kv.first] = kv.second;
                continue;
            }
            if (is_declaration(n.id)) declare_object(n, ids, rev.objects);
            ++i;
        }
    }

    std::shared_ptr<Object> make_object(const RawPropSet& raw, const IdMap& ids) {
        auto obj = std::make_shared<Object>();
        obj->props = raw.props;
        obj->oids.reserve(raw.oids.size());
        for (const auto& c : raw.oids) obj->oids.push_back(ids.resolve(c));
        for (const auto& c : raw.osids) obj->osids.push_back(CellId(ids.resolve(c)));
        for (const auto& c : raw.ctxids) obj->ctxids.push_back(ids.resolve(c));
        return obj;
    }

    RawPropSet read_prop_set(const ChunkRef& ref) {
        if (!ref.valid()) return RawPropSet();
        Reader r = file_.slice(ref.stp, ref.cb);
        return RawPropSet::parse(r);
    }

    void declare_object(const FileNode& n, const IdMap& ids, std::map<ExGuid, std::shared_ptr<const Object>>& out) {
        Reader b = n.body;
        try {
            switch (n.id) {
                case ObjectDeclarationWithRefCountFNDX:
                case ObjectDeclarationWithRefCount2FNDX: {
                    CompactId oid = CompactId::parse(b);
                    uint32_t bits = b.u32();
                    uint32_t jci = bits & 0x3FF;
                    uint32_t odcs = (bits >> 10) & 0xF;
                    if (odcs != 0) {
                        store_.encrypted = true;
                        return;
                    }
                    auto obj = make_object(read_prop_set(n.ref), ids);
                    obj->jcid = 0x00020000 | jci;
                    out[ids.resolve(oid)] = obj;
                    break;
                }
                case ObjectDeclaration2RefCountFND:
                case ObjectDeclaration2LargeRefCountFND:
                case ReadOnlyObjectDeclaration2RefCountFND:
                case ReadOnlyObjectDeclaration2LargeRefCountFND: {
                    CompactId oid = CompactId::parse(b);
                    uint32_t jcid = b.u32();
                    auto obj = make_object(read_prop_set(n.ref), ids);
                    obj->jcid = jcid;
                    out[ids.resolve(oid)] = obj;
                    break;
                }
                case ObjectDeclarationFileData3RefCountFND:
                case ObjectDeclarationFileData3LargeRefCountFND: {
                    CompactId oid = CompactId::parse(b);
                    uint32_t jcid = b.u32();
                    if (n.id == ObjectDeclarationFileData3RefCountFND)
                        b.skip(1);
                    else
                        b.skip(4);
                    std::string ref = read_storage_string(b);
                    std::string ext = read_storage_string(b);
                    auto obj = std::make_shared<Object>();
                    obj->jcid = jcid;
                    obj->file_ext = ext;
                    bool missing = false;
                    Blob data = resolve_file_data(ref, missing);
                    if (!missing) obj->file_data = data;
                    obj->file_missing = missing;
                    out[ids.resolve(oid)] = obj;
                    break;
                }
                default: break;
            }
        } catch (const ParseError& e) {
            Log::warn(cat("skipping unreadable object (node 0x", std::hex, n.id, std::dec, "): ", e.what()));
        }
    }

    void apply_object_revision(const FileNode& n, const IdMap& ids, Revision& rev,
                               const std::map<ExGuid, Revision>& revisions) {
        Reader b = n.body;
        CompactId cid = CompactId::parse(b);
        ExGuid id = ids.resolve(cid);
        // The revised object keeps the type of its original declaration.
        uint32_t jcid = 0;
        auto own = rev.objects.find(id);
        if (own != rev.objects.end()) {
            jcid = own->second->jcid;
        } else {
            ExGuid cur = rev.dependent;
            std::set<ExGuid> seen;
            while (!cur.is_nil() && seen.insert(cur).second) {
                auto it = revisions.find(cur);
                if (it == revisions.end()) break;
                auto o = it->second.objects.find(id);
                if (o != it->second.objects.end()) {
                    jcid = o->second->jcid;
                    break;
                }
                cur = it->second.dependent;
            }
        }
        if (jcid == 0) {
            Log::debug("object revision without declaration: " + id.str());
            return;
        }
        try {
            auto obj = make_object(read_prop_set(n.ref), ids);
            obj->jcid = jcid;
            rev.objects[id] = obj;
        } catch (const ParseError& e) {
            Log::warn(std::string("skipping unreadable object revision: ") + e.what());
        }
    }

    static std::string read_storage_string(Reader& r) {
        uint32_t cch = r.u32();
        if (cch > r.remaining() / 2) throw ParseError("string length out of range");
        Buffer data = r.bytes(static_cast<size_t>(cch) * 2);
        return utf16le_to_utf8(data);
    }
};

}  // namespace

Store load_desktop_store(BufferPtr data, const StoreOptions& opts) {
    DesktopParser p(std::move(data), opts);
    return p.parse();
}

}  // namespace oneconv
