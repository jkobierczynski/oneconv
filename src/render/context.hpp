// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Shared rendering infrastructure: options, asset files and link resolution.
#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include "../one/model.hpp"

namespace oneconv::render {

namespace fs = std::filesystem;

/// Markdown dialect.
enum class MdFlavor {
    Standard,  // CommonMark + GitHub extensions, relative links
    Obsidian,  // an Obsidian vault: wikilinks, embeds, #tags, properties
};

struct Options {
    MdFlavor flavor = MdFlavor::Standard;
    bool enex = false;           // write Evernote export files (.enex) instead of pages
    bool markdown = true;
    bool html = false;
    bool front_matter = true;    // YAML front matter in Markdown pages
    int heading_offset = 1;      // OneNote "Heading 1" -> "##" by default (page title is "#")
    bool html_canvas = false;    // absolute positioning like OneNote instead of flowing layout
    bool ink = true;             // export ink drawings as SVG
    bool md_html_tags = true;    // allow <u>, <sup>, <sub>, <mark> in Markdown output
    bool include_date = true;    // show the page date line
};

/// Writes binary assets next to pages, de-duplicating identical blobs.
class AssetWriter {
public:
    /// `shared_names` (optional) makes file names unique across several writers, so that
    /// a file can be referenced by its bare name from anywhere (Obsidian embeds).
    /// `wiki_safe` keeps characters out of file names that break Obsidian links.
    explicit AssetWriter(fs::path dir, std::set<std::string>* shared_names = nullptr, bool wiki_safe = false)
        : dir_(std::move(dir)), shared_(shared_names), wiki_safe_(wiki_safe) {}

    /// Write `data` under a unique name derived from `preferred`; returns the file path.
    fs::path write(const Blob& data, const std::string& preferred);
    /// Write text (e.g. an SVG) under a unique name.
    fs::path write_text(const std::string& text, const std::string& preferred);
    const fs::path& dir() const { return dir_; }
    size_t count() const { return written_.size(); }

private:
    fs::path dir_;
    std::set<std::string>* shared_ = nullptr;
    bool wiki_safe_ = false;
    std::set<std::string> used_;  // lower-cased file names
    std::map<std::tuple<const void*, size_t, size_t>, fs::path> written_;
    fs::path unique_path(const std::string& preferred);
};

/// Locations of exported pages, used to rewrite onenote: links.
struct LinkTable {
    struct Target {
        fs::path md;
        fs::path html;
        std::string wiki;  // Obsidian wikilink target (note name, or path when the name is ambiguous)
    };
    std::map<std::string, Target> pages;     // page id (upper-case braced GUID) -> files
    std::map<std::string, Target> sections;  // section id -> first page
};

/// Per-page rendering context.
struct PageContext {
    const Options* opts = nullptr;
    const LinkTable* links = nullptr;
    AssetWriter* assets = nullptr;
    fs::path page_dir;     // directory the page file is written to
    std::string slug;      // file-name stem of the page, used for asset names
    int image_counter = 0;
    int ink_counter = 0;
    bool for_html = false;
    std::string stem;         // file name of the page without extension
    std::string parent_wiki;  // Obsidian: wikilink target of the parent page (subpages)

    /// The exported page a OneNote-internal (onenote:) link points to, if it is part of this export.
    const LinkTable::Target* find_internal(const std::string& href) const;
    /// Resolve a hyperlink target, rewriting OneNote-internal links to relative files.
    std::string resolve_link(const std::string& href) const;
    /// Relative URL from the page directory to a file.
    std::string rel(const fs::path& file) const;
};

/// Render an ink drawing as a standalone SVG document. `max_width_px` of 0 keeps natural size.
std::string ink_to_svg(const model::Ink& ink, bool standalone, const std::string& title = "");
/// Natural size of an ink drawing in CSS pixels.
std::pair<double, double> ink_size_px(const model::Ink& ink);

/// A raster image and the size (in CSS pixels) it is meant to be shown at.
struct PngImage {
    Buffer data;
    int width = 0;
    int height = 0;
};
/// Render an ink drawing to a PNG on a white background (2x resolution, anti-aliased).
/// For formats such as ENEX whose readers cannot display SVG. Empty if there is no ink.
PngImage ink_to_png(const model::Ink& ink);

/// Replace the characters that cannot appear in an Obsidian link target (# ^ [ ] |).
std::string wiki_safe_name(const std::string& name);

/// True for empty or OneNote-generated names such as "Untitled picture.png".
bool generic_image_name(const std::string& filename);

/// Sort page items into reading order (top-to-bottom, then left-to-right).
std::vector<const model::PageItem*> reading_order(const model::Page& page);

/// Reading-order items for flowing layouts. Runs of adjacent ink objects are merged
/// into a single drawing so handwriting keeps its spatial arrangement.
std::vector<model::PageItem> flow_items(const model::Page& page);

}  // namespace oneconv::render
