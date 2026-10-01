// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "guid.hpp"

#include <cctype>

namespace oneconv {

namespace {
int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
const char* kHexLower = "0123456789abcdef";
const char* kHexUpper = "0123456789ABCDEF";

std::string format(const Guid& g, const char* hex) {
    // Data1 (LE u32), Data2 (LE u16), Data3 (LE u16), Data4 (8 bytes as-is)
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        uint8_t v = g.b[order[i]];
        s.push_back(hex[v >> 4]);
        s.push_back(hex[v & 15]);
    }
    return s;
}
}  // namespace

Guid Guid::from_string(const std::string& in) {
    std::string hex;
    for (char c : in)
        if (hexval(c) >= 0) hex.push_back(c);
    if (hex.size() != 32) throw ParseError("invalid GUID string: " + in);
    uint8_t v[16];
    for (int i = 0; i < 16; ++i) v[i] = static_cast<uint8_t>(hexval(hex[2 * i]) * 16 + hexval(hex[2 * i + 1]));
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    Guid g;
    for (int i = 0; i < 16; ++i) g.b[order[i]] = v[i];
    return g;
}

std::string Guid::str() const { return format(*this, kHexLower); }
std::string Guid::braced_upper() const { return "{" + format(*this, kHexUpper) + "}"; }

ExGuid ExGuid::parse_compact(Reader& r) {
    // MS-FSSHTTPB 2.2.1.7 Extended GUID
    uint8_t first = r.u8();
    if (first == 0) return ExGuid();
    if ((first & 0x07) == 0x04) {  // 5-bit value
        uint32_t v = first >> 3;
        return ExGuid(Guid::parse(r), v);
    }
    if ((first & 0x3F) == 0x20) {  // 10-bit value
        uint32_t v = (static_cast<uint32_t>(r.u8()) << 2) | (first >> 6);
        return ExGuid(Guid::parse(r), v);
    }
    if ((first & 0x7F) == 0x40) {  // 17-bit value
        uint32_t v = (static_cast<uint32_t>(r.u16()) << 1) | (first >> 7);
        return ExGuid(Guid::parse(r), v);
    }
    if (first == 0x80) {  // 32-bit value
        uint32_t v = r.u32();
        return ExGuid(Guid::parse(r), v);
    }
    throw ParseError("invalid compact ExGUID header byte");
}

}  // namespace oneconv
