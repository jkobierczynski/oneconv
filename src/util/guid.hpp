// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// GUID, extended GUID (ExGUID) and cell identifiers used by MS-ONESTORE / MS-FSSHTTPB.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "bytes.hpp"

namespace oneconv {

struct Guid {
    std::array<uint8_t, 16> b{};  // raw on-disk byte order

    static Guid parse(Reader& r) {
        Guid g;
        r.need(16);
        for (auto& x : g.b) x = r.u8();
        return g;
    }
    /// Parse "{01234567-89ab-cdef-0123-456789abcdef}" (braces optional).
    static Guid from_string(const std::string& s);

    bool is_nil() const {
        for (auto x : b)
            if (x) return false;
        return true;
    }
    /// Canonical lowercase form without braces.
    std::string str() const;
    /// Uppercase with braces, the way OneNote writes them in links.
    std::string braced_upper() const;

    bool operator==(const Guid& o) const { return b == o.b; }
    bool operator!=(const Guid& o) const { return b != o.b; }
    bool operator<(const Guid& o) const { return b < o.b; }
    Guid operator^(const Guid& o) const {
        Guid g;
        for (size_t i = 0; i < 16; ++i) g.b[i] = b[i] ^ o.b[i];
        return g;
    }
};

/// Extended GUID: GUID + 32-bit value (MS-ONESTORE 2.2.1 / MS-FSSHTTPB 2.2.1.7).
struct ExGuid {
    Guid guid;
    uint32_t n = 0;

    ExGuid() = default;
    ExGuid(const Guid& g, uint32_t v) : guid(g), n(v) {}

    bool is_nil() const { return n == 0 && guid.is_nil(); }
    std::string str() const { return "{" + guid.str() + "," + std::to_string(n) + "}"; }

    /// Fixed-size desktop encoding: 16-byte GUID + u32.
    static ExGuid parse_fixed(Reader& r) {
        ExGuid e;
        e.guid = Guid::parse(r);
        e.n = r.u32();
        return e;
    }
    /// Variable-length FSSHTTPB encoding.
    static ExGuid parse_compact(Reader& r);

    bool operator==(const ExGuid& o) const { return n == o.n && guid == o.guid; }
    bool operator!=(const ExGuid& o) const { return !(*this == o); }
    bool operator<(const ExGuid& o) const { return guid < o.guid || (guid == o.guid && n < o.n); }
    ExGuid operator^(const ExGuid& o) const { return ExGuid(guid ^ o.guid, n ^ o.n); }
};

/// Cell identifier (two ExGUIDs). Object spaces are keyed by these.
/// For desktop files the second ExGUID is always nil.
struct CellId {
    ExGuid a;
    ExGuid b;
    CellId() = default;
    CellId(const ExGuid& x, const ExGuid& y) : a(x), b(y) {}
    explicit CellId(const ExGuid& x) : a(x) {}
    bool is_nil() const { return a.is_nil() && b.is_nil(); }
    std::string str() const { return "(" + a.str() + "," + b.str() + ")"; }
    bool operator==(const CellId& o) const { return a == o.a && b == o.b; }
    bool operator!=(const CellId& o) const { return !(*this == o); }
    bool operator<(const CellId& o) const { return a < o.a || (a == o.a && b < o.b); }
};

}  // namespace oneconv
