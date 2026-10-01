// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "loader.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

#include "../cab/cab.hpp"
#include "../one/builder.hpp"
#include "../onestore/store.hpp"
#include "../util/log.hpp"
#include "../util/text.hpp"

namespace oneconv {

namespace fs = std::filesystem;
using namespace model;

namespace {

/// Minimal virtual file system so notebooks can come from disk or from a .onepkg.
class Vfs {
public:
    struct Entry {
        std::string name;
        bool dir = false;
    };
    virtual ~Vfs() = default;
    virtual std::vector<Entry> list(const std::string& dir) const = 0;
    virtual BufferPtr read(const std::string& path) const = 0;  // nullptr if missing
    virtual std::string describe(const std::string& path) const = 0;
};

std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }

BufferPtr read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return nullptr;
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size < 0) return nullptr;
    f.seekg(0);
    auto buf = std::make_shared<Buffer>(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(buf->data()), size);
    return buf;
}

class NativeFs : public Vfs {
public:
    explicit NativeFs(fs::path root) : root_(std::move(root)) {}
    std::vector<Entry> list(const std::string& dir) const override {
        std::vector<Entry> out;
        std::error_code ec;
        for (const auto& de : fs::directory_iterator(resolve(dir), ec)) {
            Entry e;
            e.name = path_utf8(de.path().filename());
            e.dir = de.is_directory(ec);
            out.push_back(e);
        }
        return out;
    }
    BufferPtr read(const std::string& path) const override { return read_file(resolve(path)); }
    std::string describe(const std::string& path) const override { return path_utf8(resolve(path)); }

private:
    fs::path root_;
    fs::path resolve(const std::string& rel) const { return rel.empty() ? root_ : root_ / u8path(rel); }
};

class MemFs : public Vfs {
public:
    MemFs(std::vector<cab::File> files, std::string label) : label_(std::move(label)) {
        for (auto& f : files) {
            std::string name = f.name;
            while (!name.empty() && name[0] == '/') name.erase(0, 1);
            files_[name] = f.data;
        }
    }
    std::vector<Entry> list(const std::string& dir) const override {
        std::vector<Entry> out;
        std::set<std::string> seen;
        std::string prefix = dir.empty() ? "" : dir + "/";
        for (const auto& kv : files_) {
            if (!starts_with(kv.first, prefix)) continue;
            std::string rest = kv.first.substr(prefix.size());
            size_t slash = rest.find('/');
            Entry e;
            e.name = rest.substr(0, slash);
            e.dir = slash != std::string::npos;
            if (seen.insert(e.name).second) out.push_back(e);
        }
        return out;
    }
    BufferPtr read(const std::string& path) const override {
        auto it = files_.find(path);
        if (it == files_.end()) return nullptr;
        const Blob& b = it->second;
        return std::make_shared<const Buffer>(b.data(), b.data() + b.size);
    }
    std::string describe(const std::string& path) const override { return label_ + ":" + path; }

private:
    std::map<std::string, Blob> files_;
    std::string label_;
};

std::string stem_of(const std::string& name) {
    size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::shared_ptr<Section> load_section(const Vfs& vfs, const std::string& dir, const std::string& file) {
    auto sec = std::make_shared<Section>();
    std::string path = join(dir, file);
    sec->source = vfs.describe(path);
    sec->name = stem_of(file);
    Log::verbose("reading section " + sec->source);
    try {
        BufferPtr data = vfs.read(path);
        if (!data) throw std::runtime_error("cannot read file");
        StoreOptions so;
        so.load_external = [&vfs, dir](const std::string& name) -> Blob {
            for (const std::string& candidate : {join(dir, name), join(dir, name + ".onebin")}) {
                if (BufferPtr b = vfs.read(candidate)) return Blob(b, 0, b->size());
            }
            return Blob();
        };
        Store store = load_store(data, so);
        if (store.kind != StoreKind::Section) throw std::runtime_error("not a section file");
        Section built = build_section(store, sec->name);
        built.source = sec->source;
        *sec = std::move(built);
        if (sec->encrypted) Log::warn("section '" + sec->name + "' is password protected and was skipped");
    } catch (const std::exception& e) {
        sec->error = e.what();
        Log::warn("cannot read section " + sec->source + ": " + e.what());
    }
    return sec;
}

bool is_recycle_bin(const std::string& name) {
    return iequals(name, "OneNote_RecycleBin") || iequals(name, "OneNote_DeletedPages.one");
}

std::vector<NotebookEntry> load_folder(const Vfs& vfs, const std::string& dir, const LoadOptions& opts, int depth) {
    std::vector<NotebookEntry> entries;
    if (depth > 16) return entries;
    std::vector<Vfs::Entry> listing = vfs.list(dir);
    std::sort(listing.begin(), listing.end(), [](const Vfs::Entry& a, const Vfs::Entry& b) {
        return to_lower(a.name) < to_lower(b.name);
    });

    // Table of contents order (any .onetoc2 in this folder)
    std::vector<TocEntry> toc;
    for (const auto& e : listing) {
        if (e.dir || !ends_with(to_lower(e.name), ".onetoc2")) continue;
        try {
            BufferPtr data = vfs.read(join(dir, e.name));
            if (!data) continue;
            Store s = load_store(data);
            auto t = read_toc(s);
            if (t.size() > toc.size()) toc = t;
        } catch (const std::exception& ex) {
            Log::verbose("ignoring unreadable table of contents " + e.name + ": " + ex.what());
        }
    }

    std::vector<const Vfs::Entry*> candidates;
    for (const auto& e : listing) {
        if (!opts.include_recycle_bin && is_recycle_bin(e.name)) continue;
        if (!e.dir && !ends_with(to_lower(e.name), ".one")) continue;
        if (e.dir && (e.name.empty() || e.name[0] == '.')) continue;
        candidates.push_back(&e);
    }
    // Order: entries listed in the table of contents first, in TOC order
    std::vector<const Vfs::Entry*> ordered;
    std::set<const Vfs::Entry*> used;
    std::map<std::string, const TocEntry*> toc_by_name;
    for (const auto& t : toc) {
        toc_by_name[to_lower(t.filename)] = &t;
        for (const auto* c : candidates) {
            if (!used.count(c) && iequals(c->name, t.filename)) {
                ordered.push_back(c);
                used.insert(c);
            }
        }
    }
    for (const auto* c : candidates)
        if (!used.count(c)) ordered.push_back(c);

    for (const auto* c : ordered) {
        NotebookEntry ne;
        if (c->dir) {
            auto group = std::make_shared<SectionGroup>();
            group->name = c->name;
            group->entries = load_folder(vfs, join(dir, c->name), opts, depth + 1);
            if (group->entries.empty()) continue;
            if (is_recycle_bin(c->name)) group->name = "Recycle Bin";
            ne.group = group;
        } else {
            ne.section = load_section(vfs, dir, c->name);
            auto t = toc_by_name.find(to_lower(c->name));
            if (!ne.section->color && t != toc_by_name.end()) ne.section->color = t->second->color;
        }
        entries.push_back(ne);
    }
    return entries;
}

/// The package may hold the notebook inside a single top-level folder.
std::string package_root(const Vfs& vfs) {
    std::string dir;
    for (int i = 0; i < 4; ++i) {
        auto listing = vfs.list(dir);
        bool has_onenote = false;
        const Vfs::Entry* only_dir = nullptr;
        int dirs = 0;
        for (const auto& e : listing) {
            std::string l = to_lower(e.name);
            if (!e.dir && (ends_with(l, ".one") || ends_with(l, ".onetoc2"))) has_onenote = true;
            if (e.dir) {
                ++dirs;
                only_dir = &e;
            }
        }
        if (has_onenote || dirs != 1) break;
        dir = join(dir, only_dir->name);
    }
    return dir;
}

}  // namespace

Notebook load_input(const fs::path& input, const LoadOptions& opts) {
    std::error_code ec;
    if (!fs::exists(input, ec)) throw std::runtime_error("input not found: " + path_utf8(input));
    Notebook nb;
    if (fs::is_directory(input, ec)) {
        fs::path abs = fs::absolute(input, ec).lexically_normal();
        if (abs.filename().empty()) abs = abs.parent_path();
        nb.name = path_utf8(abs.filename());
        NativeFs vfs(input);
        nb.entries = load_folder(vfs, "", opts, 0);
        if (nb.entries.empty()) throw std::runtime_error("no OneNote sections found in " + path_utf8(input));
        return nb;
    }
    std::string name = path_utf8(input.filename());
    std::string lower = to_lower(name);
    if (ends_with(lower, ".onetoc2")) {
        fs::path dir = input.parent_path();
        if (dir.empty()) dir = ".";
        fs::path abs = fs::absolute(dir, ec).lexically_normal();
        nb.name = path_utf8(abs.filename());
        if (nb.name.empty()) nb.name = stem_of(name);
        NativeFs vfs(dir);
        nb.entries = load_folder(vfs, "", opts, 0);
        if (nb.entries.empty()) throw std::runtime_error("no OneNote sections found next to " + name);
        return nb;
    }
    if (ends_with(lower, ".onepkg") || ends_with(lower, ".cab")) {
        BufferPtr data = read_file(input);
        if (!data) throw std::runtime_error("cannot read " + path_utf8(input));
        Log::verbose("extracting package " + name);
        MemFs vfs(cab::extract(data), name);
        nb.name = stem_of(name);
        nb.entries = load_folder(vfs, package_root(vfs), opts, 0);
        if (nb.entries.empty()) throw std::runtime_error("no OneNote sections found in package " + name);
        return nb;
    }
    // Single section
    fs::path dir = input.parent_path();
    NativeFs vfs(dir.empty() ? fs::path(".") : dir);
    auto sec = load_section(vfs, "", name);
    if (!sec->error.empty()) throw std::runtime_error(sec->error);
    nb.name = sec->name;
    NotebookEntry e;
    e.section = sec;
    nb.entries.push_back(e);
    return nb;
}

}  // namespace oneconv
