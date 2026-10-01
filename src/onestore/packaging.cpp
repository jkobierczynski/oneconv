// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Parser for the alternative packaging format used by OneDrive / SharePoint
// downloads and OneNote for Windows 10 ([MS-ONESTORE] 2.8, [MS-FSSHTTPB]).
#include <set>

#include "../util/log.hpp"
#include "store.hpp"

namespace oneconv {

namespace {

const Guid kGuidPackageFormat = Guid::from_string("{638DE92F-A6D4-4BC1-9A36-B3FC2511A5B7}");
const Guid kGuidOneFile = Guid::from_string("{7B5C52E4-D88C-4DA7-AEB1-5378D02996D3}");
const Guid kGuidRevisionRole = Guid::from_string("{4A3717F8-1C14-49E7-9526-81D942DE1741}");
const ExGuid kHeaderCellRoot(Guid::from_string("{1A5A319C-C26B-41AA-B9C5-9BD8C44E07D4}"), 1);
const ExGuid kDataRoot(Guid::from_string("{84DEFAB9-AAA3-4A0D-A3A8-520C77AC7073}"), 2);

// Stream object types (MS-FSSHTTPB 2.2.1.5)
enum : uint32_t {
    TypeDataElement = 0x01,
    TypeObjectDataBlob = 0x02,
    TypeObjectGroupDataExcluded = 0x03,
    TypeObjectGroupDataBlob = 0x05,
    TypeStorageManifestRoot = 0x07,
    TypeRevisionManifestRoot = 0x0A,
    TypeCellManifest = 0x0B,
    TypeStorageManifest = 0x0C,
    TypeStorageIndexRevisionMapping = 0x0D,
    TypeStorageIndexCellMapping = 0x0E,
    TypeStorageIndexManifestMapping = 0x11,
    TypeDataElementPackage = 0x15,
    TypeObjectGroupDataObject = 0x16,
    TypeObjectGroupObject = 0x18,
    TypeRevisionManifestGroupReference = 0x19,
    TypeRevisionManifest = 0x1A,
    TypeObjectGroupBlobReference = 0x1C,
    TypeObjectGroupDeclaration = 0x1D,
    TypeObjectGroupData = 0x1E,
    TypeDataElementFragment = 0x6A,
    TypeObjectGroupMetadata = 0x78,
    TypeObjectGroupMetadataBlock = 0x79,
    TypeOneNotePackaging = 0x7A,
};

uint64_t compact_u64(Reader& r) {
    uint8_t first = r.peek_u8();
    if (first == 0) {
        r.skip(1);
        return 0;
    }
    if (first & 0x01) return r.u8() >> 1;
    if (first & 0x02) return r.u16() >> 2;
    if (first & 0x04) return r.u24() >> 3;
    if (first & 0x08) return r.u32() >> 4;
    auto read_n = [&](int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(r.u8()) << (8 * i);
        return v;
    };
    if (first & 0x10) return read_n(5) >> 5;
    if (first & 0x20) return read_n(6) >> 6;
    if (first & 0x40) return read_n(7) >> 7;
    if (first == 0x80) {
        r.skip(1);
        return r.u64();
    }
    throw ParseError("invalid compact uint64");
}

struct SerialNumber {
    Guid guid;
    uint64_t serial = 0;
    static SerialNumber parse(Reader& r) {
        SerialNumber s;
        if (r.u8() == 0) return s;
        s.guid = Guid::parse(r);
        s.serial = r.u64();
        return s;
    }
    bool operator==(const SerialNumber& o) const { return guid == o.guid && serial == o.serial; }
};

CellId parse_cell(Reader& r) {
    ExGuid a = ExGuid::parse_compact(r);
    ExGuid b = ExGuid::parse_compact(r);
    return CellId(a, b);
}

std::vector<ExGuid> parse_exguid_array(Reader& r) {
    uint64_t n = compact_u64(r);
    if (n > r.remaining()) throw ParseError("ExGUID array too long");
    std::vector<ExGuid> v;
    v.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) v.push_back(ExGuid::parse_compact(r));
    return v;
}

std::vector<CellId> parse_cell_array(Reader& r) {
    uint64_t n = compact_u64(r);
    if (n > r.remaining()) throw ParseError("cell id array too long");
    std::vector<CellId> v;
    v.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) v.push_back(parse_cell(r));
    return v;
}

struct ObjectHeader {
    uint32_t type = 0;
    uint64_t length = 0;
    bool compound = false;
};

ObjectHeader parse_header(Reader& r) {
    uint8_t first = r.peek_u8();
    ObjectHeader h;
    if ((first & 0x3) == 0) {
        uint16_t v = r.u16();
        h.compound = v & 0x4;
        h.type = (v >> 3) & 0x3F;
        h.length = v >> 9;
    } else if ((first & 0x3) == 2) {
        uint32_t v = r.u32();
        h.compound = v & 0x4;
        h.type = (v >> 3) & 0x3FFF;
        h.length = v >> 17;
        if (h.length == 0x7FFF) h.length = compact_u64(r);
    } else {
        throw ParseError("expected stream object start header");
    }
    return h;
}

void expect_header(Reader& r, uint32_t type) {
    ObjectHeader h = parse_header(r);
    if (h.type != type) throw ParseError(cat("unexpected stream object 0x", std::hex, h.type, " (wanted 0x", type, ")"));
}

bool at_end8(Reader& r, uint32_t type) {
    if (r.eof()) throw ParseError("unexpected end of packaging data");
    uint8_t b = r.peek_u8();
    return (b & 0x3) == 1 && (b >> 2) == type;
}

void expect_end8(Reader& r, uint32_t type) {
    uint8_t b = r.u8();
    if ((b & 0x3) != 1 || (b >> 2) != type) throw ParseError(cat("expected 8-bit end of stream object 0x", std::hex, type));
}

struct StorageIndex {
    struct CellMapping {
        CellId cell;
        ExGuid id;
        SerialNumber serial;
    };
    struct RevisionMapping {
        ExGuid revision_mapping;
        SerialNumber serial;
    };
    std::vector<CellMapping> cells;
    std::map<ExGuid, RevisionMapping> revisions;
};

struct StorageManifest {
    Guid schema;
    std::map<ExGuid, CellId> roots;
};

struct RevisionManifest {
    ExGuid rev_id;
    ExGuid base_rev_id;
    std::vector<std::pair<ExGuid, ExGuid>> roots;  // (root id, object id)
    std::vector<ExGuid> groups;
};

struct GroupDecl {
    ExGuid object_id;
    uint64_t partition = 0;
};

struct GroupData {
    enum Kind { Object, Excluded, BlobRef } kind = Object;
    std::vector<ExGuid> refs;
    std::vector<CellId> cells;
    Blob data;
    ExGuid blob;
};

struct ObjectGroup {
    std::vector<GroupDecl> decls;
    std::vector<GroupData> data;
};

struct Package {
    std::map<ExGuid, StorageIndex> indexes;
    std::vector<StorageManifest> manifests;
    std::map<ExGuid, ExGuid> cell_manifests;
    std::map<ExGuid, RevisionManifest> revision_manifests;
    std::map<ExGuid, ObjectGroup> groups;
    std::map<ExGuid, Blob> blobs;
};

class PackagingParser {
public:
    PackagingParser(BufferPtr data, size_t offset) : buf_(std::move(data)), offset_(offset) {}

    Store parse() {
        Reader r(buf_);
        r.seek(offset_);
        Guid file_type = Guid::parse(r);
        Guid::parse(r);  // guidFile
        Guid::parse(r);  // guidLegacyFileVersion
        Guid format = Guid::parse(r);
        if (format != kGuidPackageFormat) throw ParseError("not a OneNote packaging file");
        if (r.u32() != 0) Log::debug("non-zero packaging padding");
        expect_header(r, TypeOneNotePackaging);
        ExGuid storage_index_id = ExGuid::parse_compact(r);
        Guid::parse(r);  // cell schema
        parse_data_element_package(r);

        Store store;
        store.format = StoreFormat::Packaging;
        store.kind = file_type == kGuidOneFile ? StoreKind::Section : StoreKind::TableOfContents;

        const StorageIndex* index = nullptr;
        auto it = pkg_.indexes.find(storage_index_id);
        if (it != pkg_.indexes.end())
            index = &it->second;
        else if (!pkg_.indexes.empty())
            index = &pkg_.indexes.begin()->second;
        if (!index) throw ParseError("storage index missing");
        if (pkg_.manifests.empty()) throw ParseError("storage manifest missing");
        const StorageManifest& manifest = pkg_.manifests.front();

        auto data_root = manifest.roots.find(kDataRoot);
        if (data_root == manifest.roots.end()) throw ParseError("no data root in storage manifest");
        store.root_space = data_root->second;
        CellId header_cell;
        auto hr = manifest.roots.find(kHeaderCellRoot);
        if (hr != manifest.roots.end()) header_cell = hr->second;

        for (const auto& mapping : index->cells) {
            if (mapping.id.is_nil()) continue;
            if (mapping.cell == header_cell && !header_cell.is_nil()) continue;
            try {
                ObjectSpace space = parse_object_space(mapping, *index);
                store.spaces[mapping.cell] = std::move(space);
            } catch (const ParseError& e) {
                Log::warn(cat("skipping damaged object space ", mapping.cell.str(), ": ", e.what()));
            }
        }
        if (!store.spaces.count(store.root_space)) throw ParseError("data root object space could not be read");
        return store;
    }

private:
    BufferPtr buf_;
    size_t offset_;
    Package pkg_;

    void parse_data_element_package(Reader& r) {
        expect_header(r, TypeDataElementPackage);
        if (r.u8() != 0) Log::debug("non-zero data element package reserved byte");
        while (!at_end8(r, TypeDataElementPackage)) parse_data_element(r);
        expect_end8(r, TypeDataElementPackage);
    }

    void parse_data_element(Reader& r) {
        expect_header(r, TypeDataElement);
        ExGuid id = ExGuid::parse_compact(r);
        SerialNumber::parse(r);
        uint64_t type = compact_u64(r);
        switch (type) {
            case 0x01: pkg_.indexes[id] = parse_storage_index(r); break;
            case 0x02: pkg_.manifests.push_back(parse_storage_manifest(r)); break;
            case 0x03:
                expect_header(r, TypeCellManifest);
                pkg_.cell_manifests[id] = ExGuid::parse_compact(r);
                expect_end8(r, TypeDataElement);
                break;
            case 0x04: pkg_.revision_manifests[id] = parse_revision_manifest(r); break;
            case 0x05: pkg_.groups[id] = parse_object_group(r); break;
            case 0x06: parse_fragment(r); break;
            case 0x0A: {
                expect_header(r, TypeObjectDataBlob);
                uint64_t size = compact_u64(r);
                if (size > r.remaining()) throw ParseError("object data blob out of range");
                pkg_.blobs[id] = r.blob(static_cast<size_t>(size));
                expect_end8(r, TypeDataElement);
                break;
            }
            default: throw ParseError(cat("unknown data element type ", type));
        }
    }

    StorageIndex parse_storage_index(Reader& r) {
        StorageIndex idx;
        while (!at_end8(r, TypeDataElement)) {
            ObjectHeader h = parse_header(r);
            switch (h.type) {
                case TypeStorageIndexManifestMapping:
                    ExGuid::parse_compact(r);
                    SerialNumber::parse(r);
                    break;
                case TypeStorageIndexCellMapping: {
                    StorageIndex::CellMapping m;
                    m.cell = parse_cell(r);
                    m.id = ExGuid::parse_compact(r);
                    m.serial = SerialNumber::parse(r);
                    idx.cells.push_back(m);
                    break;
                }
                case TypeStorageIndexRevisionMapping: {
                    ExGuid rev = ExGuid::parse_compact(r);
                    StorageIndex::RevisionMapping m;
                    m.revision_mapping = ExGuid::parse_compact(r);
                    m.serial = SerialNumber::parse(r);
                    idx.revisions[rev] = m;
                    break;
                }
                default: throw ParseError(cat("unexpected object in storage index: 0x", std::hex, h.type));
            }
        }
        expect_end8(r, TypeDataElement);
        return idx;
    }

    StorageManifest parse_storage_manifest(Reader& r) {
        StorageManifest m;
        expect_header(r, TypeStorageManifest);
        m.schema = Guid::parse(r);
        while (!at_end8(r, TypeDataElement)) {
            expect_header(r, TypeStorageManifestRoot);
            ExGuid root = ExGuid::parse_compact(r);
            m.roots[root] = parse_cell(r);
        }
        expect_end8(r, TypeDataElement);
        return m;
    }

    RevisionManifest parse_revision_manifest(Reader& r) {
        RevisionManifest m;
        expect_header(r, TypeRevisionManifest);
        m.rev_id = ExGuid::parse_compact(r);
        m.base_rev_id = ExGuid::parse_compact(r);
        while (!at_end8(r, TypeDataElement)) {
            ObjectHeader h = parse_header(r);
            if (h.type == TypeRevisionManifestRoot) {
                ExGuid root = ExGuid::parse_compact(r);
                ExGuid obj = ExGuid::parse_compact(r);
                m.roots.emplace_back(root, obj);
            } else if (h.type == TypeRevisionManifestGroupReference) {
                m.groups.push_back(ExGuid::parse_compact(r));
            } else {
                throw ParseError(cat("unexpected object in revision manifest: 0x", std::hex, h.type));
            }
        }
        expect_end8(r, TypeDataElement);
        return m;
    }

    ObjectGroup parse_object_group(Reader& r) {
        ObjectGroup g;
        expect_header(r, TypeObjectGroupDeclaration);
        while (!at_end8(r, TypeObjectGroupDeclaration)) {
            ObjectHeader h = parse_header(r);
            GroupDecl d;
            if (h.type == TypeObjectGroupObject) {
                d.object_id = ExGuid::parse_compact(r);
                d.partition = compact_u64(r);
                compact_u64(r);  // data size
                compact_u64(r);  // object reference count
                compact_u64(r);  // cell reference count
            } else if (h.type == TypeObjectGroupDataBlob) {
                d.object_id = ExGuid::parse_compact(r);
                ExGuid::parse_compact(r);  // blob id
                d.partition = compact_u64(r);
                compact_u64(r);
                compact_u64(r);
            } else {
                throw ParseError(cat("unexpected object group declaration 0x", std::hex, h.type));
            }
            g.decls.push_back(d);
        }
        expect_end8(r, TypeObjectGroupDeclaration);

        ObjectHeader h = parse_header(r);
        if (h.type == TypeObjectGroupMetadataBlock) {
            while (!at_end8(r, TypeObjectGroupMetadataBlock)) {
                expect_header(r, TypeObjectGroupMetadata);
                compact_u64(r);  // change frequency
            }
            expect_end8(r, TypeObjectGroupMetadataBlock);
            h = parse_header(r);
        }
        if (h.type != TypeObjectGroupData) throw ParseError("object group data missing");
        while (!at_end8(r, TypeObjectGroupData)) {
            ObjectHeader dh = parse_header(r);
            GroupData d;
            if (dh.type == TypeObjectGroupDataExcluded) {
                d.kind = GroupData::Excluded;
                d.refs = parse_exguid_array(r);
                d.cells = parse_cell_array(r);
                compact_u64(r);
            } else if (dh.type == TypeObjectGroupDataObject) {
                d.kind = GroupData::Object;
                d.refs = parse_exguid_array(r);
                d.cells = parse_cell_array(r);
                uint64_t size = compact_u64(r);
                if (size > r.remaining()) throw ParseError("object data out of range");
                d.data = r.blob(static_cast<size_t>(size));
            } else if (dh.type == TypeObjectGroupBlobReference) {
                d.kind = GroupData::BlobRef;
                d.refs = parse_exguid_array(r);
                d.cells = parse_cell_array(r);
                d.blob = ExGuid::parse_compact(r);
            } else {
                throw ParseError(cat("unexpected object group data 0x", std::hex, dh.type));
            }
            g.data.push_back(std::move(d));
        }
        expect_end8(r, TypeObjectGroupData);
        expect_end8(r, TypeDataElement);
        return g;
    }

    void parse_fragment(Reader& r) {
        expect_header(r, TypeDataElementFragment);
        ExGuid::parse_compact(r);
        uint64_t size = compact_u64(r);
        compact_u64(r);  // offset
        compact_u64(r);  // length
        if (size > r.remaining()) throw ParseError("data element fragment out of range");
        r.skip(static_cast<size_t>(size));
        if (!r.eof() && at_end8(r, TypeDataElement)) expect_end8(r, TypeDataElement);
        Log::debug("data element fragments are not supported; skipped one");
    }

    // -- Object spaces ----------------------------------------------------------

    std::optional<ExGuid> resolve_revision(const StorageIndex& idx, const ExGuid& id) const {
        auto it = idx.revisions.find(id);
        if (it != idx.revisions.end()) return it->second.revision_mapping;
        if (pkg_.revision_manifests.count(id)) return id;
        return std::nullopt;
    }

    ObjectSpace parse_object_space(const StorageIndex::CellMapping& mapping, const StorageIndex& idx) {
        ObjectSpace space;
        space.id = mapping.cell;
        const ExGuid& space_id = mapping.cell.b;

        std::optional<ExGuid> manifest_id;
        bool nil_revision = false;
        auto cm = pkg_.cell_manifests.find(mapping.id);
        if (cm != pkg_.cell_manifests.end()) {
            nil_revision = cm->second.is_nil();
            manifest_id = resolve_revision(idx, cm->second);
        }
        if (!manifest_id) manifest_id = resolve_revision(idx, mapping.id);
        if (!manifest_id) {
            for (const auto& kv : idx.revisions)
                if (kv.second.serial == mapping.serial) {
                    manifest_id = kv.second.revision_mapping;
                    break;
                }
        }
        if (!manifest_id) {
            if (nil_revision) return space;
            throw ParseError("no revision manifest for cell");
        }

        // Walk from the newest revision back through its bases; newer data wins.
        std::set<ExGuid> visited;
        std::optional<ExGuid> cur = manifest_id;
        while (cur) {
            if (!visited.insert(*cur).second) break;
            auto rm = pkg_.revision_manifests.find(*cur);
            if (rm == pkg_.revision_manifests.end()) throw ParseError("revision manifest not found");
            const RevisionManifest& man = rm->second;
            for (const auto& root : man.roots) {
                if (root.first.guid != kGuidRevisionRole) continue;
                space.roots.emplace(root.first.n, root.second);  // emplace keeps newer entry
            }
            for (const auto& gid : man.groups) {
                auto g = pkg_.groups.find(gid);
                if (g == pkg_.groups.end()) throw ParseError("object group not found");
                add_group_objects(g->second, space_id, space);
            }
            cur.reset();
            if (!man.base_rev_id.is_nil()) cur = resolve_revision(idx, man.base_rev_id);
        }
        return space;
    }

    void add_group_objects(const ObjectGroup& g, const ExGuid& space_id, ObjectSpace& space) {
        if (g.decls.size() != g.data.size()) throw ParseError("object group declaration/data count mismatch");
        std::map<std::pair<ExGuid, uint64_t>, const GroupData*> parts;
        std::vector<ExGuid> order;
        for (size_t i = 0; i < g.decls.size(); ++i) {
            parts[{g.decls[i].object_id, g.decls[i].partition}] = &g.data[i];
            if (order.empty() || order.back() != g.decls[i].object_id) order.push_back(g.decls[i].object_id);
        }
        for (const ExGuid& oid : order) {
            if (space.objects.count(oid)) continue;  // newer revision already provided it
            auto meta = parts.find({oid, 4});
            auto data = parts.find({oid, 1});
            if (meta == parts.end() || data == parts.end()) continue;
            if (meta->second->kind != GroupData::Object || data->second->kind != GroupData::Object) continue;
            try {
                auto obj = std::make_shared<Object>();
                Reader mr(meta->second->data.buffer, meta->second->data.offset,
                          meta->second->data.offset + meta->second->data.size);
                obj->jcid = mr.u32();
                Reader dr(data->second->data.buffer, data->second->data.offset,
                          data->second->data.offset + data->second->data.size);
                RawPropSet raw = RawPropSet::parse(dr);
                obj->props = std::move(raw.props);
                obj->oids = data->second->refs;
                for (const auto& c : data->second->cells) {
                    if (c.b == space_id)
                        obj->ctxids.push_back(c.a);
                    else
                        obj->osids.push_back(c);
                }
                auto file = parts.find({oid, 2});
                if (file != parts.end() && file->second->kind == GroupData::BlobRef) {
                    auto b = pkg_.blobs.find(file->second->blob);
                    if (b != pkg_.blobs.end())
                        obj->file_data = b->second;
                    else
                        obj->file_missing = true;
                }
                space.objects[oid] = obj;
            } catch (const ParseError& e) {
                Log::warn(cat("skipping unreadable object ", oid.str(), ": ", e.what()));
            }
        }
    }
};

}  // namespace

Store load_packaging_store(BufferPtr data, size_t offset) {
    PackagingParser p(std::move(data), offset);
    return p.parse();
}

Store load_store(BufferPtr data, const StoreOptions& opts) {
    if (!data || data->size() < 64) throw ParseError("file too small to be a OneNote file");
    Reader r(data);
    r.seek(32);
    Guid legacy_version = Guid::parse(r);
    Guid format = Guid::parse(r);
    if (format == kGuidPackageFormat) return load_packaging_store(data, 0);

    const Guid desktop_format = Guid::from_string("{109ADD3F-911B-49F5-A5D0-1791EDC8AED8}");
    if (format != desktop_format)
        throw ParseError("unsupported OneNote file format (OneNote 2007 and older are not supported)");
    // Some downloads wrap a packaging store after a nearly empty desktop header.
    if (legacy_version.is_nil() && data->size() >= 1024) {
        Reader h(data);
        h.seek(160);
        uint64_t stp = h.u64();
        uint64_t cb = h.u32();
        uint64_t end = stp + cb;
        if (end + 64 <= data->size()) {
            Reader p(data);
            p.seek(static_cast<size_t>(end) + 48);
            if (Guid::parse(p) == kGuidPackageFormat) return load_packaging_store(data, static_cast<size_t>(end));
        }
    }
    return load_desktop_store(data, opts);
}

}  // namespace oneconv
