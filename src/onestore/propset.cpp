// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Property set parsing (MS-ONESTORE 2.6).
#include <sstream>

#include "../util/log.hpp"
#include "../util/text.hpp"
#include "store.hpp"

namespace oneconv {

namespace {
constexpr int kMaxDepth = 32;

PropType prop_type_of(uint32_t raw) { return static_cast<PropType>((raw >> 26) & 0x1F); }

void parse_value(Reader& r, PropertyValue& v, RefCursor& cursor, int depth) {
    switch (v.type) {
        case PropType::NoData: break;
        case PropType::Bool: v.num = (v.raw >> 31) & 1; break;
        case PropType::OneByte: v.num = r.u8(); break;
        case PropType::TwoBytes: v.num = r.u16(); break;
        case PropType::FourBytes: v.num = r.u32(); break;
        case PropType::EightBytes: v.num = r.u64(); break;
        case PropType::Bytes: {
            uint32_t n = r.u32();
            v.data = r.bytes(n);
            break;
        }
        case PropType::ObjectId:
            v.count = 1;
            v.ref_index = cursor.oid;
            cursor.oid += 1;
            break;
        case PropType::ObjectIds:
            v.count = r.u32();
            v.ref_index = cursor.oid;
            cursor.oid += v.count;
            break;
        case PropType::ObjectSpaceId:
            v.count = 1;
            v.ref_index = cursor.osid;
            cursor.osid += 1;
            break;
        case PropType::ObjectSpaceIds:
            v.count = r.u32();
            v.ref_index = cursor.osid;
            cursor.osid += v.count;
            break;
        case PropType::ContextId:
            v.count = 1;
            v.ref_index = cursor.ctx;
            cursor.ctx += 1;
            break;
        case PropType::ContextIds:
            v.count = r.u32();
            v.ref_index = cursor.ctx;
            cursor.ctx += v.count;
            break;
        case PropType::PropertyValues: {
            // ArrayOfPropertyValues (2.6.9): count, element PropertyID, then the property sets
            uint32_t n = r.u32();
            v.element_prop = r.u32();
            if (n > r.remaining() / 2) throw ParseError("property value array too large");
            v.sets.reserve(n);
            for (uint32_t i = 0; i < n; ++i) v.sets.push_back(parse_property_set(r, cursor, depth + 1));
            break;
        }
        case PropType::PropertySet: v.sets.push_back(parse_property_set(r, cursor, depth + 1)); break;
        default: throw ParseError(cat("unknown property type 0x", std::hex, static_cast<int>(v.type)));
    }
}
}  // namespace

PropertySet parse_property_set(Reader& r, RefCursor& cursor, int depth) {
    if (depth > kMaxDepth) throw ParseError("property sets nested too deeply");
    PropertySet set;
    uint16_t count = r.u16();
    r.need(static_cast<size_t>(count) * 4);
    set.values.resize(count);
    for (auto& v : set.values) {
        v.raw = r.u32();
        v.type = prop_type_of(v.raw);
    }
    for (auto& v : set.values) parse_value(r, v, cursor, depth);
    return set;
}

RawPropSet RawPropSet::parse(Reader& r) {
    RawPropSet ps;
    StreamHeader oid_header = StreamHeader::parse(r);
    r.need(static_cast<size_t>(oid_header.count) * 4);
    for (uint32_t i = 0; i < oid_header.count; ++i) ps.oids.push_back(CompactId::parse(r));
    if (!oid_header.osid_stream_not_present) {
        StreamHeader osid_header = StreamHeader::parse(r);
        r.need(static_cast<size_t>(osid_header.count) * 4);
        for (uint32_t i = 0; i < osid_header.count; ++i) ps.osids.push_back(CompactId::parse(r));
        if (osid_header.extended_streams_present) {
            StreamHeader ctx_header = StreamHeader::parse(r);
            r.need(static_cast<size_t>(ctx_header.count) * 4);
            for (uint32_t i = 0; i < ctx_header.count; ++i) ps.ctxids.push_back(CompactId::parse(r));
        }
    }
    RefCursor cursor;
    ps.props = parse_property_set(r, cursor);
    return ps;
}

// ---------------------------------------------------------------------------
// Debug dump

namespace {
void dump_set(std::ostringstream& os, const PropertySet& set, const Object& obj, int indent) {
    std::string pad(static_cast<size_t>(indent) * 2, ' ');
    for (const auto& v : set.values) {
        os << pad << "0x" << std::hex << v.raw << std::dec << " ";
        switch (v.type) {
            case PropType::NoData: os << "(nodata)"; break;
            case PropType::Bool: os << (v.num ? "true" : "false"); break;
            case PropType::OneByte:
            case PropType::TwoBytes:
            case PropType::FourBytes:
            case PropType::EightBytes: os << v.num << " (0x" << std::hex << v.num << std::dec << ")"; break;
            case PropType::Bytes: {
                os << "bytes[" << v.data.size() << "]";
                if (v.data.size() >= 2 && v.data.size() % 2 == 0 && v.data.size() < 400) {
                    std::string s = utf16le_to_utf8(v.data);
                    bool printable = !s.empty();
                    for (unsigned char c : s)
                        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != 0x0b) printable = false;
                    if (printable) os << " \"" << s << "\"";
                }
                break;
            }
            case PropType::ObjectId:
            case PropType::ObjectIds:
                os << "oids[";
                for (uint32_t i = 0; i < std::min<uint32_t>(v.count, 64); ++i) {
                    size_t k = v.ref_index + i;
                    os << (i ? " " : "") << (k < obj.oids.size() ? obj.oids[k].str() : std::string("?"));
                }
                os << "]";
                break;
            case PropType::ObjectSpaceId:
            case PropType::ObjectSpaceIds:
                os << "osids[";
                for (uint32_t i = 0; i < std::min<uint32_t>(v.count, 64); ++i) {
                    size_t k = v.ref_index + i;
                    os << (i ? " " : "") << (k < obj.osids.size() ? obj.osids[k].str() : std::string("?"));
                }
                os << "]";
                break;
            case PropType::ContextId:
            case PropType::ContextIds: os << "ctx x" << v.count; break;
            case PropType::PropertyValues:
                os << "array of " << v.sets.size() << " (elem 0x" << std::hex << v.element_prop << std::dec << ")\n";
                for (const auto& s : v.sets) {
                    os << pad << "  {\n";
                    dump_set(os, s, obj, indent + 2);
                    os << pad << "  }\n";
                }
                continue;
            case PropType::PropertySet:
                os << "set\n";
                for (const auto& s : v.sets) dump_set(os, s, obj, indent + 1);
                continue;
        }
        os << "\n";
    }
}
}  // namespace

std::string dump_store(const Store& store) {
    std::ostringstream os;
    os << "store kind=" << (store.kind == StoreKind::Section ? "section" : "toc")
       << " format=" << (store.format == StoreFormat::Desktop ? "desktop" : "packaging")
       << " root=" << store.root_space.str() << "\n";
    for (const auto& kv : store.spaces) {
        const ObjectSpace& sp = kv.second;
        os << "space " << kv.first.str() << " (" << sp.objects.size() << " objects)\n";
        for (const auto& r : sp.roots) os << "  root role " << r.first << " -> " << r.second.str() << "\n";
        for (const auto& o : sp.objects) {
            os << "  object " << o.first.str() << " jcid=0x" << std::hex << o.second->jcid << std::dec;
            if (o.second->file_data) os << " file_data=" << o.second->file_data->size << "B";
            if (!o.second->file_ext.empty()) os << " ext=" << o.second->file_ext;
            os << "\n";
            dump_set(os, o.second->props, *o.second, 2);
        }
    }
    return os.str();
}

}  // namespace oneconv
