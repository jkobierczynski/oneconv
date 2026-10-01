// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Output-oriented document model of a OneNote notebook.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../util/bytes.hpp"
#include "math.hpp"

namespace oneconv::model {

struct Color {
    uint8_t r = 0, g = 0, b = 0;
    std::string hex() const;  // "#rrggbb"
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; }
};

struct TextStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool superscript = false;
    bool subscript = false;
    bool hidden = false;
    bool hyperlink = false;
    bool math = false;
    bool embedded = false;  // run is an embedded object (ink)
    std::optional<uint32_t> embedded_type;
    std::string font;
    int font_size_half_pt = 0;
    std::optional<Color> color;
    std::optional<Color> highlight;
    std::string style_id;

    /// Same visible character formatting (ignores hidden/hyperlink flags).
    bool same_format(const TextStyle& o) const {
        return bold == o.bold && italic == o.italic && underline == o.underline && strike == o.strike &&
               superscript == o.superscript && subscript == o.subscript && color == o.color &&
               highlight == o.highlight && font == o.font && font_size_half_pt == o.font_size_half_pt;
    }
};

enum class Align { Left, Center, Right };

struct InkStroke {
    std::vector<std::pair<float, float>> points;  // absolute, HIMETRIC (0.01 mm)
    std::optional<uint32_t> color;                // COLORREF 0x00BBGGRR
    float width = 0;                              // HIMETRIC
    float height = 0;
    uint8_t transparency = 0;
    uint8_t pen_tip = 0;
};

struct Ink {
    std::vector<InkStroke> strokes;
    std::optional<float> offset_x;  // half-inches from parent
    std::optional<float> offset_y;
    std::string recognized_text;    // handwriting recognition, when present
    bool empty() const { return strokes.empty(); }
};

struct Inline {
    enum class Kind { Text, Break, Math, Ink };
    Kind kind = Kind::Text;
    std::string text;  // Text: content; Math: plain-text fallback
    TextStyle style;
    std::string href;  // hyperlink target
    std::shared_ptr<const MathSeq> math;
    bool math_display = false;
    std::shared_ptr<const Ink> ink;
};

/// Note tag shapes (MS-ONE 2.3.? NoteTagShape); only the ones we treat specially are named.
struct NoteTag {
    std::string label;
    int shape = 0;
    bool completed = false;
    std::optional<Color> highlight;
    std::optional<Color> text_color;
    std::string created;
    std::string completed_at;

    bool is_checkbox() const;
    /// Unicode symbol approximating the tag icon.
    std::string symbol() const;
};

struct Paragraph {
    std::vector<Inline> inlines;
    std::string style_id;  // "p", "h1".."h6", "PageTitle", "PageDateTime", "cite", "blockquote", "code"
    Align align = Align::Left;
    bool rtl = false;
    std::vector<NoteTag> tags;
    TextStyle base;  // paragraph-level formatting

    std::string plain_text() const;
    bool empty() const;
    int heading_level() const;  // 1..6 or 0
};

struct Image {
    Blob data;
    std::string ext;  // ".png" etc, may be empty
    std::string alt;
    std::string filename;
    std::string ocr_text;
    std::string link;
    float width = 0;  // half-inches; 0 if unknown
    float height = 0;
    bool background = false;
    std::vector<std::string> embeds;  // URLs of embedded web content (videos etc)
    std::vector<NoteTag> tags;
    bool missing = false;
    std::optional<float> x, y;
};

struct Attachment {
    Blob data;
    std::string name;
    std::string source_path;
    enum class Media { None, Audio, Video } media = Media::None;
    bool missing = false;
    std::vector<NoteTag> tags;
    std::optional<float> x, y;
};

struct OutlineElement;

struct TableCell {
    std::vector<OutlineElement> elements;
    std::optional<Color> background;
};

struct Table {
    std::vector<std::vector<TableCell>> rows;
    std::vector<float> col_widths;  // half-inches
    bool borders = true;
    std::vector<NoteTag> tags;
};

struct Content {
    enum class Kind { Paragraph, Table, Image, Attachment, Ink };
    Kind kind = Kind::Paragraph;
    std::shared_ptr<Paragraph> paragraph;
    std::shared_ptr<Table> table;
    std::shared_ptr<Image> image;
    std::shared_ptr<Attachment> attachment;
    std::shared_ptr<Ink> ink;
};

struct ListFormat {
    bool ordered = false;
    std::string bullet;   // UTF-8 bullet character for unordered lists
    int number_style = 0;  // 0 decimal, 1 upper roman, 2 lower roman, 3 upper alpha, 4 lower alpha
    std::optional<int> restart;
};

struct OutlineElement {
    std::vector<Content> contents;
    std::optional<ListFormat> list;
    std::vector<OutlineElement> children;
};

struct Outline {
    std::vector<OutlineElement> elements;
    std::optional<float> x, y;  // half-inches
    std::optional<float> width;
};

struct PageItem {
    enum class Kind { Outline, Image, Attachment, Ink };
    Kind kind = Kind::Outline;
    std::shared_ptr<Outline> outline;
    std::shared_ptr<Image> image;
    std::shared_ptr<Attachment> attachment;
    std::shared_ptr<Ink> ink;

    std::optional<float> x() const;
    std::optional<float> y() const;
};

struct Page {
    std::string title;
    std::string id;  // page entity GUID, "{XXXXXXXX-...}" upper case
    int level = 0;   // 0 = top-level page, 1 = subpage, ...
    std::string created;
    std::string modified;
    std::string author;
    std::vector<PageItem> items;
    std::string title_date;  // text of the date/time line under the title
};

struct Section {
    std::string name;
    std::string id;
    std::optional<Color> color;
    std::vector<Page> pages;
    std::string source;  // input file path
    bool encrypted = false;
    std::string error;   // set if the section could not be read
};

struct SectionGroup;

struct NotebookEntry {
    std::shared_ptr<Section> section;
    std::shared_ptr<SectionGroup> group;
};

struct SectionGroup {
    std::string name;
    std::vector<NotebookEntry> entries;
};

struct Notebook {
    std::string name;
    std::vector<NotebookEntry> entries;
};

}  // namespace oneconv::model
