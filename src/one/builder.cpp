// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "builder.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

#include "../util/log.hpp"
#include "../util/text.hpp"
#include "ids.hpp"

namespace oneconv {

using namespace model;

namespace {

constexpr int kMaxDepth = 64;
constexpr uint32_t kInkSpaceBlob = 0x00020026;
constexpr uint32_t kInkEndOfLineBlob = 0x00020027;
const char* kHyperlinkMarker = "\xEF\xB7\x9F" "HYPERLINK \"";  // U+FDDF HYPERLINK "

/// Typed accessors over an object's (possibly nested) property set.
class Props {
public:
    Props(const Object* obj, const PropertySet* set) : obj_(obj), set_(set) {}
    explicit Props(const Object* obj) : obj_(obj), set_(obj ? &obj->props : nullptr) {}

    const PropertyValue* get(uint32_t p) const { return set_ ? set_->get(p) : nullptr; }
    bool has(uint32_t p) const { return get(p) != nullptr; }

    std::optional<uint64_t> num(uint32_t p) const {
        const PropertyValue* v = get(p);
        if (!v) return std::nullopt;
        switch (v->type) {
            case PropType::Bool:
            case PropType::OneByte:
            case PropType::TwoBytes:
            case PropType::FourBytes:
            case PropType::EightBytes: return v->num;
            default: return std::nullopt;
        }
    }
    std::optional<uint32_t> u32(uint32_t p) const {
        auto n = num(p);
        if (!n) return std::nullopt;
        return static_cast<uint32_t>(*n);
    }
    bool flag(uint32_t p, bool def = false) const {
        const PropertyValue* v = get(p);
        if (!v) return def;
        return v->num != 0;
    }
    std::optional<float> f32(uint32_t p) const {
        auto n = u32(p);
        if (!n) return std::nullopt;
        float f;
        uint32_t bits = *n;
        std::memcpy(&f, &bits, 4);
        if (f != f) return std::nullopt;  // NaN
        return f;
    }
    const Buffer* bytes(uint32_t p) const {
        const PropertyValue* v = get(p);
        if (!v || v->type != PropType::Bytes) return nullptr;
        return &v->data;
    }
    std::string str(uint32_t p) const {
        const Buffer* b = bytes(p);
        return b ? utf16le_to_utf8(*b) : std::string();
    }
    std::optional<Color> color(uint32_t p) const {
        auto n = u32(p);
        if (!n) return std::nullopt;
        if ((*n >> 24) != 0) return std::nullopt;  // 0xFF...... = automatic
        Color c;
        c.r = static_cast<uint8_t>(*n & 0xFF);
        c.g = static_cast<uint8_t>((*n >> 8) & 0xFF);
        c.b = static_cast<uint8_t>((*n >> 16) & 0xFF);
        return c;
    }
    std::vector<ExGuid> refs(uint32_t p) const {
        std::vector<ExGuid> out;
        const PropertyValue* v = get(p);
        if (!v || !obj_) return out;
        if (v->type != PropType::ObjectId && v->type != PropType::ObjectIds) return out;
        size_t end = std::min(obj_->oids.size(), static_cast<size_t>(v->ref_index) + v->count);
        for (size_t k = v->ref_index; k < end; ++k)
            if (!obj_->oids[k].is_nil()) out.push_back(obj_->oids[k]);
        return out;
    }
    std::optional<ExGuid> ref(uint32_t p) const {
        auto v = refs(p);
        if (v.empty()) return std::nullopt;
        return v.front();
    }
    std::vector<CellId> space_refs(uint32_t p) const {
        std::vector<CellId> out;
        const PropertyValue* v = get(p);
        if (!v || !obj_) return out;
        if (v->type != PropType::ObjectSpaceId && v->type != PropType::ObjectSpaceIds) return out;
        size_t end = std::min(obj_->osids.size(), static_cast<size_t>(v->ref_index) + v->count);
        for (size_t k = v->ref_index; k < end; ++k) out.push_back(obj_->osids[k]);
        return out;
    }
    /// Element property sets of an ArrayOfPropertyValues property.
    std::vector<Props> array(uint32_t p) const {
        std::vector<Props> out;
        const PropertyValue* v = get(p);
        if (!v || v->type != PropType::PropertyValues) return out;
        for (const auto& s : v->sets) out.emplace_back(obj_, &s);
        return out;
    }
    std::string time32(uint32_t p) const {
        auto n = u32(p);
        return n ? time32_to_iso(*n) : std::string();
    }
    std::string filetime(uint32_t p) const {
        auto n = num(p);
        return n ? filetime_to_iso(*n) : std::string();
    }
    std::optional<Guid> guid(uint32_t p) const {
        const Buffer* b = bytes(p);
        if (!b || b->size() < 16) return std::nullopt;
        Reader r = Reader::from_bytes(*b);
        return Guid::parse(r);
    }

private:
    const Object* obj_;
    const PropertySet* set_;
};

std::string detect_image_ext(const Blob& b) {
    if (b.size < 4) return "";
    const uint8_t* p = b.data();
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return ".png";
    if (p[0] == 0xFF && p[1] == 0xD8) return ".jpg";
    if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F') return ".gif";
    if (p[0] == 'B' && p[1] == 'M') return ".bmp";
    if ((p[0] == 'I' && p[1] == 'I' && p[2] == 42) || (p[0] == 'M' && p[1] == 'M' && p[3] == 42)) return ".tif";
    if (b.size >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) return ".webp";
    if (p[0] == 0xD7 && p[1] == 0xCD && p[2] == 0xC6 && p[3] == 0x9A) return ".wmf";
    if (b.size >= 44 && p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 0 && std::memcmp(p + 40, " EMF", 4) == 0)
        return ".emf";
    if (std::memcmp(p, "%PDF", 4) == 0) return ".pdf";
    if (b.size >= 5 && (std::memcmp(p, "<?xml", 5) == 0 || std::memcmp(p, "<svg", 4) == 0)) return ".svg";
    return "";
}

/// Decode the variable-length integer encoding used by ink paths.
bool decode_multibyte(const Buffer& in, std::vector<int64_t>& out) {
    size_t pos = 0;
    auto read_uint = [&](uint64_t& v) -> bool {
        v = 0;
        for (int count = 0; count < 10; ++count) {
            if (pos >= in.size()) return false;
            uint8_t b = in[pos++];
            v |= static_cast<uint64_t>(b & 0x7F) << (7 * count);
            if (!(b & 0x80)) return true;
        }
        return false;
    };
    uint64_t len;
    if (!read_uint(len)) return false;
    len >>= 1;
    if (len > in.size()) return false;
    out.reserve(static_cast<size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
        uint64_t v;
        if (!read_uint(v)) return false;
        int64_t s = static_cast<int64_t>(v >> 1);
        out.push_back((v & 1) ? -s : s);
    }
    return true;
}

class SectionBuilder {
public:
    explicit SectionBuilder(const Store& store) : store_(store) {}

    Section build(const std::string& fallback_name) {
        Section sec;
        const ObjectSpace* root = store_.root();
        if (!root) throw ParseError("section has no root object space");

        if (auto meta_id = root->root(RoleMetadata)) {
            if (const Object* meta = root->get(*meta_id); meta && meta->jcid == jcid::SectionMetadata) {
                Props p(meta);
                sec.name = p.str(prop::SectionDisplayName);
                sec.color = p.color(prop::SectionColor);
            }
        }
        if (sec.name.empty()) sec.name = fallback_name;
        if (ends_with(to_lower(sec.name), ".one")) sec.name.resize(sec.name.size() - 4);

        auto content_id = root->root(RoleDefaultContent);
        const Object* content = content_id ? root->get(*content_id) : nullptr;
        if (!content || content->jcid != jcid::SectionNode) {
            if (store_.encrypted) {
                sec.encrypted = true;
                return sec;
            }
            throw ParseError("section content node is missing");
        }
        Props sp(content);
        if (auto g = sp.guid(prop::NotebookManagementEntityGuid)) sec.id = g->braced_upper();

        for (const ExGuid& series_id : sp.refs(prop::ElementChildNodes)) {
            const Object* series = root->get(series_id);
            if (!series || series->jcid != jcid::PageSeriesNode) continue;
            Props ps(series);
            for (const CellId& page_space_id : ps.space_refs(prop::ChildGraphSpaceElementNodes)) {
                const ObjectSpace* page_space = store_.space(page_space_id);
                if (!page_space) {
                    Log::warn("page object space missing in section '" + sec.name + "'");
                    continue;
                }
                try {
                    if (auto page = build_page(*page_space)) sec.pages.push_back(std::move(*page));
                } catch (const std::exception& e) {
                    Log::warn("skipping unreadable page in section '" + sec.name + "': " + e.what());
                }
            }
        }
        if (store_.encrypted && sec.pages.empty()) sec.encrypted = true;
        return sec;
    }

private:
    const Store& store_;
    const ObjectSpace* space_ = nullptr;  // current page space
    size_t budget_ = 0;                   // node visits left on this page (guards against cycles)

    bool spend() {
        if (budget_ == 0) return false;
        if (--budget_ == 0) Log::warn("page structure too large or cyclic; output truncated");
        return true;
    }

    const Object* obj(const ExGuid& id) const { return space_ ? space_->get(id) : nullptr; }

    // ---------------------------------------------------------------- pages

    std::optional<Page> build_page(const ObjectSpace& space) {
        space_ = &space;
        budget_ = 1000000;
        Page page;

        if (auto meta_id = space.root(RoleMetadata)) {
            if (const Object* meta = space.get(*meta_id); meta && meta->jcid == jcid::PageMetadata) {
                Props m(meta);
                if (m.flag(prop::IsDeletedGraphSpaceContent)) return std::nullopt;
                page.title = m.str(prop::CachedTitleString);
                page.level = static_cast<int>(m.u32(prop::PageLevel).value_or(0));
                if (page.level > 0) page.level -= 1;  // PageLevel is 1-based
                page.created = m.filetime(prop::TopologyCreationTimeStamp);
                if (auto g = m.guid(prop::NotebookManagementEntityGuid)) page.id = g->braced_upper();
            }
        }

        auto manifest_id = space.root(RoleDefaultContent);
        const Object* manifest = manifest_id ? space.get(*manifest_id) : nullptr;
        if (!manifest) throw ParseError("page has no content root");
        const Object* node = manifest;
        if (manifest->jcid == jcid::PageManifestNode) {
            auto pid = Props(manifest).ref(prop::ContentChildNodes);
            node = pid ? space.get(*pid) : nullptr;
        }
        if (!node || node->jcid != jcid::PageNode) throw ParseError("page node missing");
        Props pn(node);
        page.modified = pn.time32(prop::LastModifiedTime);
        page.author = pn.str(prop::Author);

        // Title block
        if (auto title_id = pn.ref(prop::StructureElementChildNodes)) {
            if (const Object* title = obj(*title_id); title && title->jcid == jcid::TitleNode) {
                std::string title_text, date_text;
                for (const ExGuid& oid : Props(title).refs(prop::ElementChildNodes)) {
                    const Object* o = obj(oid);
                    if (!o || o->jcid != jcid::OutlineNode) continue;
                    Outline outline = build_outline(*o, 0);
                    collect_title_text(outline.elements, title_text, date_text);
                }
                if (!trim(title_text).empty()) page.title = trim(title_text);
                page.title_date = trim(date_text);
            }
        }
        if (page.title.empty()) page.title = trim(pn.str(prop::CachedTitleStringFromPage));

        for (const ExGuid& cid : pn.refs(prop::ElementChildNodes)) {
            const Object* o = obj(cid);
            if (!o) continue;
            PageItem item;
            switch (o->jcid) {
                case jcid::OutlineNode:
                    item.kind = PageItem::Kind::Outline;
                    item.outline = std::make_shared<Outline>(build_outline(*o, 0));
                    break;
                case jcid::ImageNode:
                    item.kind = PageItem::Kind::Image;
                    item.image = std::make_shared<Image>(build_image(*o));
                    break;
                case jcid::EmbeddedFileNode:
                    item.kind = PageItem::Kind::Attachment;
                    item.attachment = std::make_shared<Attachment>(build_attachment(*o));
                    break;
                case jcid::InkContainer:
                    item.kind = PageItem::Kind::Ink;
                    item.ink = std::make_shared<Ink>(build_ink_container(*o, 0));
                    if (item.ink->empty()) continue;
                    break;
                default: continue;
            }
            page.items.push_back(std::move(item));
        }

        if (page.title.empty()) {
            // Fall back to the first line of text on the page
            for (const auto& it : page.items) {
                if (it.kind != PageItem::Kind::Outline) continue;
                std::string a, b;
                collect_title_text(it.outline->elements, a, b, true);
                a = trim(a.substr(0, a.find('\n')));
                if (!a.empty()) {
                    page.title = a.size() > 80 ? a.substr(0, 80) : a;
                    break;
                }
            }
        }
        space_ = nullptr;
        return page;
    }

    static void collect_title_text(const std::vector<OutlineElement>& els, std::string& title, std::string& date,
                                   bool any = false) {
        for (const auto& e : els) {
            for (const auto& c : e.contents) {
                if (c.kind != Content::Kind::Paragraph) continue;
                std::string t = c.paragraph->plain_text();
                if (c.paragraph->style_id == "PageDateTime") {
                    if (!date.empty()) date += " ";
                    date += t;
                } else if (title.empty() || !any) {
                    if (!title.empty()) title += " ";
                    title += t;
                }
                if (any && !trim(title).empty()) return;
            }
            collect_title_text(e.children, title, date, any);
        }
    }

    // -------------------------------------------------------------- outlines

    Outline build_outline(const Object& o, int depth) {
        Outline out;
        Props p(&o);
        out.x = p.f32(prop::OffsetFromParentHoriz);
        out.y = p.f32(prop::OffsetFromParentVert);
        out.width = p.f32(prop::LayoutMaxWidth);
        for (const ExGuid& id : p.refs(prop::ElementChildNodes)) append_outline_item(id, out.elements, depth + 1);
        return out;
    }

    void append_outline_item(const ExGuid& id, std::vector<OutlineElement>& out, int depth) {
        if (depth > kMaxDepth || !spend()) return;
        const Object* o = obj(id);
        if (!o) return;
        if (o->jcid == jcid::OutlineGroup) {
            for (const ExGuid& c : Props(o).refs(prop::ElementChildNodes)) append_outline_item(c, out, depth + 1);
        } else if (o->jcid == jcid::OutlineElementNode) {
            out.push_back(build_element(*o, depth));
        }
    }

    OutlineElement build_element(const Object& o, int depth) {
        OutlineElement el;
        Props p(&o);
        for (const ExGuid& cid : p.refs(prop::ContentChildNodes)) {
            const Object* c = obj(cid);
            if (!c) continue;
            Content content;
            switch (c->jcid) {
                case jcid::RichTextNode:
                    content.kind = Content::Kind::Paragraph;
                    content.paragraph = std::make_shared<Paragraph>(build_paragraph(*c));
                    break;
                case jcid::TableNode:
                    content.kind = Content::Kind::Table;
                    content.table = std::make_shared<Table>(build_table(*c, depth));
                    break;
                case jcid::ImageNode:
                    content.kind = Content::Kind::Image;
                    content.image = std::make_shared<Image>(build_image(*c));
                    break;
                case jcid::EmbeddedFileNode:
                    content.kind = Content::Kind::Attachment;
                    content.attachment = std::make_shared<Attachment>(build_attachment(*c));
                    break;
                case jcid::InkContainer:
                    content.kind = Content::Kind::Ink;
                    content.ink = std::make_shared<Ink>(build_ink_container(*c, 0));
                    if (content.ink->empty()) continue;
                    break;
                default: continue;
            }
            el.contents.push_back(std::move(content));
        }
        for (const ExGuid& lid : p.refs(prop::ListNodes)) {
            const Object* l = obj(lid);
            if (l && l->jcid == jcid::NumberListNode) {
                el.list = build_list(*l);
                break;
            }
        }
        for (const ExGuid& cid : p.refs(prop::ElementChildNodes)) append_outline_item(cid, el.children, depth + 1);
        return el;
    }

    static ListFormat build_list(const Object& o) {
        ListFormat f;
        Props p(&o);
        std::u16string fmt;
        if (const Buffer* b = p.bytes(prop::NumberListFormat)) {
            for (size_t i = 0; i + 1 < b->size(); i += 2) fmt.push_back(static_cast<char16_t>((*b)[i] | ((*b)[i + 1] << 8)));
            if (!fmt.empty()) fmt.erase(fmt.begin());  // leading length
        }
        if (auto r = p.u32(prop::ListRestart)) f.restart = static_cast<int>(*r);
        if (!fmt.empty() && fmt[0] == 0xFFFD) {
            f.ordered = true;
            f.number_style = fmt.size() > 1 ? fmt[1] : 0;
            if (f.number_style > 4) f.number_style = 0;
        } else {
            std::string font = p.str(prop::ListFont);
            char16_t c = fmt.empty() ? 0x2022 : fmt[0];
            // Map symbol-font bullets to Unicode
            if (font == "Wingdings") {
                if (c == 0xA7) c = 0x25AA;
                else if (c == 0xA8) c = 0x25FB;
                else if (c == 0x77) c = 0x2B25;
                else if (c == 0xD8) c = 0x27A2;
                else if (c == 0xFC) c = 0x2714;
                else if (c == 0x76) c = 0x2756;
                else c = 0x25AA;
            } else if (font == "Wingdings 2" || font == "Wingdings 3" || font == "Symbol") {
                c = (font == "Symbol" && c == 0xB7) ? 0x2022 : 0x25C6;
            } else if (c == 0xF0A7 || c == 0xF0B7) {
                c = 0x2022;
            }
            if (c >= 0xF000 && c <= 0xF0FF) c = 0x2022;  // private-use symbol font glyphs
            f.bullet = utf16_to_utf8(std::u16string(1, c));
        }
        return f;
    }

    // ---------------------------------------------------------------- tables

    Table build_table(const Object& o, int depth) {
        Table t;
        Props p(&o);
        t.borders = p.flag(prop::TableBordersVisible, true);
        if (const Buffer* w = p.bytes(prop::TableColumnWidths)) {
            for (size_t i = 1; i + 4 <= w->size(); i += 4) {
                float f;
                std::memcpy(&f, w->data() + i, 4);
                t.col_widths.push_back(f);
            }
        }
        t.tags = build_note_tags(p);
        for (const ExGuid& rid : p.refs(prop::ElementChildNodes)) {
            const Object* row = obj(rid);
            if (!row || row->jcid != jcid::TableRowNode) continue;
            std::vector<TableCell> cells;
            for (const ExGuid& cid : Props(row).refs(prop::ElementChildNodes)) {
                const Object* cell = obj(cid);
                if (!cell || cell->jcid != jcid::TableCellNode) continue;
                TableCell tc;
                Props cp(cell);
                tc.background = cp.color(prop::CellBackgroundColor);
                for (const ExGuid& eid : cp.refs(prop::ElementChildNodes)) append_outline_item(eid, tc.elements, depth + 1);
                cells.push_back(std::move(tc));
            }
            t.rows.push_back(std::move(cells));
        }
        return t;
    }

    // ------------------------------------------------------------- note tags

    std::vector<NoteTag> build_note_tags(const Props& p) {
        std::vector<NoteTag> tags;
        for (const Props& s : p.array(prop::NoteTagStates)) {
            NoteTag tag;
            tag.completed = (s.u32(prop::ActionItemStatus).value_or(0) & 1) != 0;
            tag.created = s.time32(prop::NoteTagCreated);
            tag.completed_at = s.time32(prop::NoteTagCompleted);
            if (auto def_id = s.ref(prop::NoteTagDefinitionOid)) {
                if (const Object* def = obj(*def_id); def && def->jcid == jcid::NoteTagSharedDefinitionContainer) {
                    Props d(def);
                    tag.label = d.str(prop::NoteTagLabel);
                    tag.shape = static_cast<int>(d.u32(prop::NoteTagShape).value_or(0));
                    tag.highlight = d.color(prop::NoteTagHighlightColor);
                    tag.text_color = d.color(prop::NoteTagTextColor);
                }
            }
            tags.push_back(std::move(tag));
        }
        return tags;
    }

    // -------------------------------------------------------------- rich text

    TextStyle build_style(const Object* o) {
        TextStyle s;
        if (!o) return s;
        Props p(o);
        s.bold = p.flag(prop::Bold);
        s.italic = p.flag(prop::Italic);
        s.underline = p.flag(prop::Underline);
        s.strike = p.flag(prop::Strikethrough);
        s.superscript = p.flag(prop::Superscript);
        s.subscript = p.flag(prop::Subscript);
        s.hidden = p.flag(prop::Hidden);
        s.hyperlink = p.flag(prop::Hyperlink);
        s.math = p.flag(prop::MathFormatting);
        s.embedded = p.flag(prop::TextRunIsEmbeddedObject);
        s.embedded_type = p.u32(prop::EmbeddedObjectType);
        s.font = p.str(prop::Font);
        s.font_size_half_pt = static_cast<int>(p.u32(prop::FontSize).value_or(0));
        s.color = p.color(prop::FontColor);
        s.highlight = p.color(prop::Highlight);
        s.style_id = p.str(prop::ParagraphStyleId);
        return s;
    }

    static MathObject build_math_object(const Props& p) {
        MathObject m;
        m.type = static_cast<MathType>(p.u32(prop::MathInlineObjectType).value_or(0));
        m.argc = p.u32(prop::MathInlineObjectCount).value_or(0);
        if (auto v = p.u32(prop::MathInlineObjectCol)) m.column = static_cast<uint8_t>(*v);
        if (auto v = p.u32(prop::MathInlineObjectAlign)) m.align = static_cast<uint8_t>(*v);
        if (auto v = p.u32(prop::MathInlineObjectChar)) m.ch = static_cast<char32_t>(*v);
        if (auto v = p.u32(prop::MathInlineObjectChar1)) m.ch1 = static_cast<char32_t>(*v);
        if (auto v = p.u32(prop::MathInlineObjectChar2)) m.ch2 = static_cast<char32_t>(*v);
        return m;
    }

    struct Run {
        size_t start = 0, end = 0;  // UTF-16 code unit range
        TextStyle style;
        std::optional<Props> data;  // TextRunData entry
    };

    Paragraph build_paragraph(const Object& o) {
        Paragraph para;
        Props p(&o);
        const Object* pstyle_obj = nullptr;
        if (auto ps = p.ref(prop::ParagraphStyle)) pstyle_obj = obj(*ps);
        para.base = build_style(pstyle_obj);
        para.style_id = para.base.style_id;
        switch (p.u32(prop::ParagraphAlignment).value_or(0)) {
            case 1: para.align = Align::Center; break;
            case 2: para.align = Align::Right; break;
            default: para.align = Align::Left; break;
        }
        para.rtl = p.flag(prop::ReadingOrderRtl);
        para.tags = build_note_tags(p);

        // Text
        std::u16string text;
        if (const Buffer* b = p.bytes(prop::RichEditTextUnicode)) {
            for (size_t i = 0; i + 1 < b->size(); i += 2) text.push_back(static_cast<char16_t>((*b)[i] | ((*b)[i + 1] << 8)));
        } else if (const Buffer* a = p.bytes(prop::TextExtendedAscii)) {
            text = utf8_to_utf16(cp1252_to_utf8(a->data(), a->size()));
        }
        while (!text.empty() && text.back() == 0) text.pop_back();

        // Runs
        std::vector<uint32_t> indices;
        if (const Buffer* b = p.bytes(prop::TextRunIndex))
            for (size_t i = 0; i + 4 <= b->size(); i += 4) indices.push_back(rd32(b->data() + i));
        std::vector<TextStyle> styles;
        for (const ExGuid& sid : p.refs(prop::TextRunFormatting)) styles.push_back(build_style(obj(sid)));
        std::vector<Props> run_data = p.array(prop::TextRunData);

        // A leading vertical tab can shift all run indices by one (seen in the wild).
        if (!text.empty() && text[0] == 0x0B && !indices.empty() && indices[0] == 1 &&
            (styles.size() == indices.size() || styles.size() == indices.size() + 1)) {
            bool drop_last = styles.size() == indices.size() + 1;
            text.erase(text.begin());
            indices.erase(indices.begin());
            for (auto& i : indices) i -= 1;
            if (drop_last && !styles.empty()) styles.pop_back();
        }

        std::vector<Run> runs;
        if (styles.empty()) {
            Run r;
            r.end = text.size();
            r.style = para.base;
            r.style.style_id.clear();
            runs.push_back(r);
        } else {
            size_t start = 0;
            for (size_t i = 0; i < styles.size(); ++i) {
                size_t end = i < indices.size() ? std::min<size_t>(indices[i], text.size()) : text.size();
                if (end < start) end = start;
                Run r;
                r.start = start;
                r.end = end;
                r.style = styles[i];
                if (i < run_data.size()) r.data = run_data[i];
                runs.push_back(r);
                start = end;
            }
        }

        std::vector<ExGuid> embedded_objects = p.refs(prop::TextRunDataObject);
        size_t embedded_next = 0;

        std::string pending_link;
        std::string hidden_buf;
        bool in_link = false;
        std::vector<std::pair<std::string, MathObject>> math_segments;

        auto flush_math = [&]() {
            if (math_segments.empty()) return;
            Inline in;
            in.kind = Inline::Kind::Math;
            std::string raw;
            for (const auto& s : math_segments) raw += s.first;
            try {
                auto seq = std::make_shared<MathSeq>(parse_math(math_segments));
                in.text = math_to_text(*seq);
                in.math = seq;
            } catch (const std::exception& e) {
                Log::verbose(std::string("equation kept as text: ") + e.what());
                in.kind = Inline::Kind::Text;
                in.text = strip_math_markers(raw);
            }
            para.inlines.push_back(std::move(in));
            math_segments.clear();
        };

        for (const Run& r : runs) {
            std::u16string seg = text.substr(r.start, r.end - r.start);
            std::string seg8 = utf16_to_utf8(seg);

            if (r.style.math) {
                MathObject mo;
                if (r.data) mo = build_math_object(*r.data);
                math_segments.emplace_back(seg8, mo);
                continue;
            }
            flush_math();

            if (r.style.embedded) {
                uint32_t type = r.style.embedded_type.value_or(0);
                if (type == kInkEndOfLineBlob) {
                    Inline br;
                    br.kind = Inline::Kind::Break;
                    para.inlines.push_back(br);
                } else if (type == kInkSpaceBlob) {
                    Inline sp;
                    sp.text = " ";
                    para.inlines.push_back(sp);
                } else if (embedded_next < embedded_objects.size()) {
                    const Object* ink_data = obj(embedded_objects[embedded_next++]);
                    if (ink_data) {
                        auto ink = std::make_shared<Ink>(build_ink_data(*ink_data, std::nullopt, std::nullopt));
                        if (!ink->empty()) {
                            Inline in;
                            in.kind = Inline::Kind::Ink;
                            in.ink = ink;
                            para.inlines.push_back(in);
                        }
                    }
                }
                continue;
            }

            if (r.style.hidden) {
                // Hidden runs carry hyperlink field codes: U+FDDF HYPERLINK "target".
                // A field code may be split over several runs (e.g. at font changes).
                hidden_buf += seg8;
                continue;
            }
            if (!hidden_buf.empty()) {
                size_t m = hidden_buf.rfind(kHyperlinkMarker);
                if (m != std::string::npos) {
                    size_t st = m + std::strlen(kHyperlinkMarker);
                    size_t e = hidden_buf.find('"', st);
                    pending_link = hidden_buf.substr(st, e == std::string::npos ? std::string::npos : e - st);
                    in_link = true;
                }
                hidden_buf.clear();
            }
            // Some files keep the field code in a visible run
            while (true) {
                size_t m = seg8.find(kHyperlinkMarker);
                if (m == std::string::npos) break;
                size_t st = m + std::strlen(kHyperlinkMarker);
                size_t e = seg8.find('"', st);
                if (e == std::string::npos) break;
                if (m > 0) emit_text(para, seg8.substr(0, m), r.style, in_link ? pending_link : std::string());
                pending_link = seg8.substr(st, e - st);
                in_link = true;
                seg8 = seg8.substr(e + 1);
            }
            std::string href;
            if (r.style.hyperlink && in_link)
                href = pending_link;
            else if (!r.style.hyperlink)
                in_link = false;
            emit_text(para, seg8, r.style, href);
        }
        flush_math();

        // Paragraph consisting only of math is a display equation
        bool all_math = !para.inlines.empty();
        for (const auto& in : para.inlines)
            if (in.kind != Inline::Kind::Math && !(in.kind == Inline::Kind::Text && trim(in.text).empty())) all_math = false;
        if (all_math)
            for (auto& in : para.inlines)
                if (in.kind == Inline::Kind::Math) in.math_display = true;
        return para;
    }

    static void emit_text(Paragraph& para, const std::string& s, const TextStyle& style, const std::string& href) {
        std::string cur;
        auto flush = [&]() {
            if (cur.empty()) return;
            Inline in;
            in.text = cur;
            in.style = style;
            in.href = href;
            // Merge with the previous inline when formatting and link are identical
            if (!para.inlines.empty()) {
                Inline& prev = para.inlines.back();
                if (prev.kind == Inline::Kind::Text && prev.href == href && prev.style.same_format(style)) {
                    prev.text += cur;
                    cur.clear();
                    return;
                }
            }
            para.inlines.push_back(std::move(in));
            cur.clear();
        };
        for (uint32_t cp : utf8_codepoints(s)) {
            if (cp == 0x0B || cp == 0x0A || cp == 0x0D || cp == 0x2028) {
                flush();
                Inline br;
                br.kind = Inline::Kind::Break;
                para.inlines.push_back(br);
            } else if (cp >= 0xFDD0 && cp <= 0xFDEF) {
                // internal markers
            } else if (cp < 0x20 && cp != 0x09) {
                // control characters
            } else {
                append_utf8(cur, cp);
            }
        }
        flush();
    }

    // ------------------------------------------------------- images and files

    Image build_image(const Object& o) {
        Image img;
        Props p(&o);
        img.alt = p.str(prop::ImageAltText);
        img.filename = p.str(prop::ImageFilename);
        img.ocr_text = p.str(prop::RichEditTextUnicode);
        img.link = p.str(prop::WzHyperlinkUrl);
        img.background = p.flag(prop::IsBackground);
        img.x = p.f32(prop::OffsetFromParentHoriz);
        img.y = p.f32(prop::OffsetFromParentVert);
        img.width = p.f32(prop::PictureWidth).value_or(p.f32(prop::LayoutMaxWidth).value_or(0));
        img.height = p.f32(prop::PictureHeight).value_or(p.f32(prop::LayoutMaxHeight).value_or(0));
        img.tags = build_note_tags(p);
        if (auto cid = p.ref(prop::PictureContainer)) {
            if (const Object* c = obj(*cid)) {
                if (c->file_data) img.data = *c->file_data;
                img.missing = c->file_missing || !c->file_data;
                img.ext = c->file_ext;
            }
        } else {
            img.missing = true;
        }
        if (!img.data.empty()) {
            std::string detected = detect_image_ext(img.data);
            if (!detected.empty()) img.ext = detected;
        }
        if (img.ext.empty() && !img.filename.empty()) {
            size_t dot = img.filename.rfind('.');
            if (dot != std::string::npos) img.ext = to_lower(img.filename.substr(dot));
        }
        if (!img.ext.empty() && img.ext[0] != '.') img.ext = "." + img.ext;
        for (const ExGuid& cid : p.refs(prop::ContentChildNodes)) {
            const Object* c = obj(cid);
            if (c && c->jcid == jcid::IFrameNode) {
                std::string url = Props(c).str(prop::ImageEmbeddedUrl);
                if (!url.empty()) img.embeds.push_back(url);
            }
        }
        return img;
    }

    Attachment build_attachment(const Object& o) {
        Attachment a;
        Props p(&o);
        a.name = p.str(prop::EmbeddedFileName);
        a.source_path = p.str(prop::SourceFilepath);
        a.x = p.f32(prop::OffsetFromParentHoriz);
        a.y = p.f32(prop::OffsetFromParentVert);
        a.tags = build_note_tags(p);
        switch (p.u32(prop::IRecordMedia).value_or(0)) {
            case 1: a.media = Attachment::Media::Audio; break;
            case 2: a.media = Attachment::Media::Video; break;
            default: break;
        }
        if (auto cid = p.ref(prop::EmbeddedFileContainer)) {
            if (const Object* c = obj(*cid); c && c->file_data) {
                a.data = *c->file_data;
            } else {
                a.missing = true;
            }
        } else {
            a.missing = true;
        }
        if (a.name.empty() && !a.source_path.empty()) {
            size_t slash = a.source_path.find_last_of("/\\");
            a.name = slash == std::string::npos ? a.source_path : a.source_path.substr(slash + 1);
        }
        if (a.name.empty()) a.name = "attachment";
        return a;
    }

    // -------------------------------------------------------------------- ink

    Ink build_ink_container(const Object& o, int depth) {
        Ink ink;
        if (depth > 16 || !spend()) return ink;
        Props p(&o);
        ink.offset_x = p.f32(prop::OffsetFromParentHoriz);
        ink.offset_y = p.f32(prop::OffsetFromParentVert);
        auto sx = p.f32(prop::InkScalingX);
        auto sy = p.f32(prop::InkScalingY);
        if (auto data_id = p.ref(prop::InkData)) {
            if (const Object* d = obj(*data_id)) {
                Ink inner = build_ink_data(*d, sx, sy);
                ink.strokes = std::move(inner.strokes);
            }
        }
        for (const ExGuid& cid : p.refs(prop::ContentChildNodes)) {
            const Object* c = obj(cid);
            if (!c || c->jcid != jcid::InkContainer) continue;
            Ink child = build_ink_container(*c, depth + 1);
            // Child offsets are relative to this container; fold them into the points.
            float dx = child.offset_x.value_or(0) * 1270.0f;  // half-inch -> HIMETRIC
            float dy = child.offset_y.value_or(0) * 1270.0f;
            for (auto& s : child.strokes) {
                for (auto& pt : s.points) {
                    pt.first += dx;
                    pt.second += dy;
                }
                ink.strokes.push_back(std::move(s));
            }
        }
        return ink;
    }

    Ink build_ink_data(const Object& o, std::optional<float> sx, std::optional<float> sy) {
        Ink ink;
        if (o.jcid != jcid::InkDataNode) return ink;
        Props p(&o);
        float scale_x = sx.value_or(1.0f), scale_y = sy.value_or(1.0f);
        static const Guid kDimX = Guid::from_string("{598A6A8F-52C0-4BA0-93AF-AF357411A561}");
        static const Guid kDimY = Guid::from_string("{B53F9F75-04E0-4498-A7EE-C30DBB5A9011}");
        for (const ExGuid& sid : p.refs(prop::InkStrokes)) {
            const Object* s = obj(sid);
            if (!s || s->jcid != jcid::InkStrokeNode) continue;
            Props sp(s);
            const Buffer* path = sp.bytes(prop::InkPath);
            auto props_id = sp.ref(prop::InkStrokeProperties);
            const Object* props = props_id ? obj(*props_id) : nullptr;
            if (!path || !props) continue;
            Props pp(props);
            // Dimension descriptors: 32 bytes each (GUID, min, max, ...)
            int ix = -1, iy = -1;
            size_t ndim = 0;
            if (const Buffer* dims = pp.bytes(prop::InkDimensions)) {
                ndim = dims->size() / 32;
                for (size_t d = 0; d < ndim; ++d) {
                    Reader r = Reader::from_bytes(dims->data() + d * 32, 16);
                    Guid g = Guid::parse(r);
                    if (g == kDimX) ix = static_cast<int>(d);
                    if (g == kDimY) iy = static_cast<int>(d);
                }
            }
            if (ndim == 0) {
                ndim = 2;
                ix = 0;
                iy = 1;
            }
            if (ix < 0 || iy < 0) continue;
            std::vector<int64_t> values;
            if (!decode_multibyte(*path, values) || values.empty()) continue;
            size_t per_dim = values.size() / ndim;
            if (per_dim == 0) continue;
            InkStroke stroke;
            stroke.width = pp.f32(prop::InkWidth).value_or(0);
            stroke.height = pp.f32(prop::InkHeight).value_or(0);
            if (auto c = pp.u32(prop::InkColor)) stroke.color = *c;
            stroke.transparency = static_cast<uint8_t>(pp.u32(prop::InkTransparency).value_or(0));
            stroke.pen_tip = static_cast<uint8_t>(pp.u32(prop::InkPenTip).value_or(0));
            // Stored as a start point followed by deltas
            double x = 0, y = 0;
            for (size_t i = 0; i < per_dim; ++i) {
                x += static_cast<double>(values[static_cast<size_t>(ix) * per_dim + i]);
                y += static_cast<double>(values[static_cast<size_t>(iy) * per_dim + i]);
                stroke.points.emplace_back(static_cast<float>(x * scale_x), static_cast<float>(y * scale_y));
            }
            ink.strokes.push_back(std::move(stroke));
        }
        return ink;
    }
};

}  // namespace

model::Section build_section(const Store& store, const std::string& fallback_name) {
    SectionBuilder b(store);
    return b.build(fallback_name);
}

std::vector<TocEntry> read_toc(const Store& store) {
    std::vector<TocEntry> out;
    const ObjectSpace* root = store.root();
    if (!root) return out;
    auto content_id = root->root(RoleDefaultContent);
    if (!content_id) return out;
    std::set<ExGuid> visited;
    std::function<void(const ExGuid&, int)> walk = [&](const ExGuid& id, int depth) {
        if (depth > 32 || !visited.insert(id).second) return;
        const Object* o = root->get(id);
        if (!o || o->jcid != jcid::TocContainer) return;
        Props p(o);
        std::string name = p.str(prop::FolderChildFilename);
        if (!name.empty()) {
            TocEntry e;
            e.filename = replace_all(replace_all(name, "^M", "+"), "^J", ",");
            e.order = p.u32(prop::NotebookElementOrderingId).value_or(0);
            e.color = p.color(prop::SectionColor);
            out.push_back(e);
            return;
        }
        for (const ExGuid& c : p.refs(prop::TocChildren)) walk(c, depth + 1);
    };
    walk(*content_id, 0);
    // Keep the last occurrence of each name, then order by ordering id.
    std::vector<TocEntry> dedup;
    std::set<std::string> seen;
    for (auto it = out.rbegin(); it != out.rend(); ++it)
        if (seen.insert(to_lower(it->filename)).second) dedup.push_back(*it);
    std::reverse(dedup.begin(), dedup.end());
    std::stable_sort(dedup.begin(), dedup.end(), [](const TocEntry& a, const TocEntry& b) { return a.order < b.order; });
    return dedup;
}

}  // namespace oneconv
