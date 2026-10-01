// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Format-independent model of a OneNote revision store: object spaces holding
// objects, each with a property set (MS-ONESTORE 2.1, 2.6).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../util/bytes.hpp"
#include "../util/guid.hpp"

namespace oneconv {

/// Property data types (MS-ONESTORE 2.6.6, PropertyID.type).
enum class PropType : uint8_t {
    NoData = 0x1,
    Bool = 0x2,
    OneByte = 0x3,
    TwoBytes = 0x4,
    FourBytes = 0x5,
    EightBytes = 0x6,
    Bytes = 0x7,  // FourBytesOfLengthFollowedByData
    ObjectId = 0x8,
    ObjectIds = 0x9,
    ObjectSpaceId = 0xA,
    ObjectSpaceIds = 0xB,
    ContextId = 0xC,
    ContextIds = 0xD,
    PropertyValues = 0x10,  // ArrayOfPropertyValues
    PropertySet = 0x11,
};

struct PropertySet;

struct PropertyValue {
    uint32_t raw = 0;  // full PropertyID including type and bool bits
    PropType type = PropType::NoData;
    uint64_t num = 0;    // Bool/OneByte/TwoBytes/FourBytes/EightBytes
    Buffer data;         // Bytes
    uint32_t count = 0;  // number of references for *Id(s) types
    uint32_t ref_index = 0;  // index of first reference in the owning object's id stream
    uint32_t element_prop = 0;  // PropertyValues: PropertyID of the elements
    std::vector<PropertySet> sets;  // PropertyValues (n) / PropertySet (1)

    uint32_t id() const { return raw & 0x03FFFFFF; }
};

struct PropertySet {
    std::vector<PropertyValue> values;

    /// Look up by property id. `prop` may include the type bits (they are masked off).
    const PropertyValue* get(uint32_t prop) const {
        uint32_t id = prop & 0x03FFFFFF;
        for (const auto& v : values)
            if (v.id() == id) return &v;
        return nullptr;
    }
};

/// Reference counters used while parsing a property set. Object, object-space
/// and context references are stored out-of-band in streams; properties carry
/// only a count, so each value remembers where its references start.
struct RefCursor {
    uint32_t oid = 0;
    uint32_t osid = 0;
    uint32_t ctx = 0;
};

/// Parse a PropertySet structure (MS-ONESTORE 2.6.7).
PropertySet parse_property_set(Reader& r, RefCursor& cursor, int depth = 0);

/// Header of an ObjectSpaceObjectPropSet stream (2.6.5).
struct StreamHeader {
    uint32_t count = 0;
    bool extended_streams_present = false;
    bool osid_stream_not_present = false;
    static StreamHeader parse(Reader& r) {
        uint32_t v = r.u32();
        StreamHeader h;
        h.count = v & 0x00FFFFFF;
        h.extended_streams_present = (v >> 30) & 1;
        h.osid_stream_not_present = (v >> 31) & 1;
        return h;
    }
};

/// Compact object identifier (2.2.2): 8-bit value + 24-bit index into the global id table.
struct CompactId {
    uint8_t n = 0;
    uint32_t guid_index = 0;
    static CompactId parse(Reader& r) {
        uint32_t v = r.u32();
        CompactId c;
        c.n = static_cast<uint8_t>(v & 0xFF);
        c.guid_index = v >> 8;
        return c;
    }
    bool is_zero() const { return n == 0 && guid_index == 0; }
};

/// ObjectSpaceObjectPropSet (2.6.1), with the id streams still in compact form.
struct RawPropSet {
    std::vector<CompactId> oids;
    std::vector<CompactId> osids;
    std::vector<CompactId> ctxids;
    PropertySet props;
    static RawPropSet parse(Reader& r);
};

/// An object, with all of its references resolved to global identifiers.
struct Object {
    uint32_t jcid = 0;
    PropertySet props;
    std::vector<ExGuid> oids;     // index-aligned with the property set's ObjectID stream
    std::vector<CellId> osids;    // index-aligned with the ObjectSpaceID stream
    std::vector<ExGuid> ctxids;   // index-aligned with the ContextID stream
    std::optional<Blob> file_data;
    std::string file_ext;         // extension recorded with the file data (desktop format)
    bool file_missing = false;    // file data was referenced but could not be loaded
};

/// Root roles (2.1.8).
enum RootRole : uint32_t { RoleDefaultContent = 1, RoleMetadata = 2, RoleEncryptionKey = 3, RoleVersionMetadata = 4 };

struct ObjectSpace {
    CellId id;
    std::map<ExGuid, std::shared_ptr<const Object>> objects;
    std::map<uint32_t, ExGuid> roots;

    const Object* get(const ExGuid& oid) const {
        auto it = objects.find(oid);
        return it == objects.end() ? nullptr : it->second.get();
    }
    std::optional<ExGuid> root(uint32_t role) const {
        auto it = roots.find(role);
        if (it == roots.end()) return std::nullopt;
        return it->second;
    }
};

enum class StoreKind { Section, TableOfContents };
enum class StoreFormat { Desktop, Packaging };

struct Store {
    StoreKind kind = StoreKind::Section;
    StoreFormat format = StoreFormat::Desktop;
    CellId root_space;
    std::map<CellId, ObjectSpace> spaces;
    bool encrypted = false;

    const ObjectSpace* space(const CellId& id) const {
        auto it = spaces.find(id);
        if (it != spaces.end()) return &it->second;
        // Desktop stores identify spaces by the first ExGUID only.
        for (const auto& kv : spaces)
            if (kv.first.a == id.a && (kv.first.b.is_nil() || id.b.is_nil())) return &kv.second;
        return nullptr;
    }
    const ObjectSpace* root() const { return space(root_space); }
};

/// Hooks so a store can resolve "<file>" references to sibling files (desktop format).
struct StoreOptions {
    /// Load an external file referenced from the store by name; returns empty Blob if missing.
    std::function<Blob(const std::string& name)> load_external;
};

/// Parse a .one or .onetoc2 file (either on-disk format). Throws ParseError.
Store load_store(BufferPtr data, const StoreOptions& opts = {});

// Format-specific entry points (used by load_store).
Store load_desktop_store(BufferPtr data, const StoreOptions& opts);
Store load_packaging_store(BufferPtr data, size_t offset);

/// Human-readable dump of every object (for --dump debugging).
std::string dump_store(const Store& store);

}  // namespace oneconv
