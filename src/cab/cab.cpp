// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "cab.hpp"

#include <algorithm>
#include <map>

#include "../util/log.hpp"
#include "../util/text.hpp"

namespace oneconv::cab {

namespace {
constexpr uint16_t kFlagPrevCabinet = 0x0001;
constexpr uint16_t kFlagNextCabinet = 0x0002;
constexpr uint16_t kFlagReservePresent = 0x0004;
constexpr uint16_t kAttrNameIsUtf8 = 0x0080;

std::string read_cstring(Reader& r, size_t max = 1024) {
    std::string s;
    while (true) {
        char c = static_cast<char>(r.u8());
        if (c == 0) break;
        s.push_back(c);
        if (s.size() > max) throw ParseError("CAB string too long");
    }
    return s;
}

struct Folder {
    uint32_t data_offset = 0;
    uint16_t data_count = 0;
    uint16_t compression = 0;
};

struct Entry {
    std::string name;
    uint32_t size = 0;
    uint32_t offset = 0;
    uint16_t folder = 0;
};
}  // namespace

std::vector<File> extract(const BufferPtr& cabinet) {
    Reader r(cabinet);
    if (r.size() < 36) throw ParseError("file too small to be a cabinet");
    if (r.u32() != 0x4643534D) throw ParseError("not a cabinet file (missing MSCF signature)");  // "MSCF"
    r.skip(4);
    uint32_t cab_size = r.u32();
    (void)cab_size;
    r.skip(4);
    uint32_t files_offset = r.u32();
    r.skip(4);
    r.skip(2);  // version
    uint16_t folder_count = r.u16();
    uint16_t file_count = r.u16();
    uint16_t flags = r.u16();
    r.skip(4);  // setID, iCabinet
    uint8_t folder_reserve = 0, data_reserve = 0;
    if (flags & kFlagReservePresent) {
        uint16_t header_reserve = r.u16();
        folder_reserve = r.u8();
        data_reserve = r.u8();
        r.skip(header_reserve);
    }
    if (flags & kFlagPrevCabinet) {
        read_cstring(r);
        read_cstring(r);
    }
    if (flags & kFlagNextCabinet) {
        read_cstring(r);
        read_cstring(r);
    }
    if (flags & (kFlagPrevCabinet | kFlagNextCabinet))
        Log::warn("multi-part cabinet: only the files contained in this part can be extracted");

    std::vector<Folder> folders(folder_count);
    for (auto& f : folders) {
        f.data_offset = r.u32();
        f.data_count = r.u16();
        f.compression = r.u16();
        r.skip(folder_reserve);
    }

    r.seek(files_offset);
    std::vector<Entry> entries;
    for (uint16_t i = 0; i < file_count; ++i) {
        Entry e;
        e.size = r.u32();
        e.offset = r.u32();
        e.folder = r.u16();
        r.skip(4);  // date, time
        uint16_t attribs = r.u16();
        std::string raw = read_cstring(r);
        bool utf8 = (attribs & kAttrNameIsUtf8) != 0;
        if (!utf8) {
            // Many writers store UTF-8 without setting the flag; accept it when it decodes cleanly.
            bool high = false, valid = true;
            for (unsigned char c : raw) high |= c >= 0x80;
            for (uint32_t cp : utf8_codepoints(raw)) valid &= cp != 0xFFFD;
            utf8 = high && valid;
        }
        e.name = utf8 ? raw : cp1252_to_utf8(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        if (e.folder >= 0xFFFD) {
            Log::warn("cabinet file '" + e.name + "' continues in another cabinet; skipped");
            continue;
        }
        entries.push_back(e);
    }

    std::vector<File> out;
    std::map<uint16_t, BufferPtr> decoded;
    for (const auto& e : entries) {
        if (e.folder >= folders.size()) throw ParseError("cabinet file refers to a missing folder");
        auto it = decoded.find(e.folder);
        if (it == decoded.end()) {
            const Folder& f = folders[e.folder];
            // Gather data blocks
            Reader d(cabinet);
            d.seek(f.data_offset);
            std::vector<std::pair<const uint8_t*, size_t>> blocks;
            std::vector<size_t> uncompressed_sizes;
            size_t total = 0;
            for (uint16_t b = 0; b < f.data_count; ++b) {
                d.skip(4);  // checksum
                uint16_t cb = d.u16();
                uint16_t ucb = d.u16();
                d.skip(data_reserve);
                d.need(cb);
                blocks.emplace_back(d.ptr(), cb);
                d.skip(cb);
                uncompressed_sizes.push_back(ucb);
                total += ucb;
            }
            Buffer data;
            int method = f.compression & 0x000F;
            if (method == 0) {
                data.reserve(total);
                for (const auto& b : blocks) data.insert(data.end(), b.first, b.first + b.second);
            } else if (method == 1) {
                data.reserve(total);
                for (size_t b = 0; b < blocks.size(); ++b) {
                    const uint8_t* p = blocks[b].first;
                    size_t n = blocks[b].second;
                    if (n < 2 || p[0] != 'C' || p[1] != 'K') throw ParseError("bad MSZIP block signature");
                    size_t before = data.size();
                    inflate_append(p + 2, n - 2, data, before + uncompressed_sizes[b]);
                    if (data.size() != before + uncompressed_sizes[b]) throw ParseError("MSZIP block size mismatch");
                }
            } else if (method == 3) {
                int window = (f.compression >> 8) & 0x1F;
                data = lzx_decompress(blocks, window, total);
            } else {
                throw ParseError(method == 2 ? "Quantum-compressed cabinets are not supported"
                                             : "unknown cabinet compression method");
            }
            it = decoded.emplace(e.folder, std::make_shared<const Buffer>(std::move(data))).first;
        }
        const BufferPtr& buf = it->second;
        if (static_cast<size_t>(e.offset) + e.size > buf->size()) throw ParseError("cabinet file extends past folder data");
        out.push_back(File{e.name, Blob(buf, e.offset, e.size)});
    }
    return out;
}

}  // namespace oneconv::cab
