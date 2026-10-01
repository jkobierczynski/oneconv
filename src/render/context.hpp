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

struct Options {
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
    explicit AssetWriter(fs::path dir) : dir_(std::move(dir)) {}

    /// Write `data` under a unique name derived from `preferred`; returns the file path.
    fs::path write(const Blob& data, const std::string& preferred);
    /// Write text (e.g. an SVG) under a unique name.
    fs::path write_text(const std::string& text, const std::string& preferred);
    const fs::path& dir() const { return dir_; }
    size_t count() const { return written_.size(); }

private:
    fs::path dir_;
    std::set<std::string> used_;  // lower-cased file names
    std::map<std::tuple<const void*, size_t, size_t>, fs::path> written_;
    fs::path unique_path(const std::string& preferred);
};

/// Locations of exported pages, used to rewrite onenote: links.
struct LinkTable {
    struct Target {
        fs::path md;
        fs::path html;
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

    /// Resolve a hyperlink target, rewriting OneNote-internal links to relative files.
    std::string resolve_link(const std::string& href) const;
    /// Relative URL from the page directory to a file.
    std::string rel(const fs::path& file) const;
};

/// Render an ink drawing as a standalone SVG document. `max_width_px` of 0 keeps natural size.
std::string ink_to_svg(const model::Ink& ink, bool standalone, const std::string& title = "");
/// Natural size of an ink drawing in CSS pixels.
std::pair<double, double> ink_size_px(const model::Ink& ink);

/// True for empty or OneNote-generated names such as "Untitled picture.png".
bool generic_image_name(const std::string& filename);

/// Sort page items into reading order (top-to-bottom, then left-to-right).
std::vector<const model::PageItem*> reading_order(const model::Page& page);

/// Reading-order items for flowing layouts. Runs of adjacent ink objects are merged
/// into a single drawing so handwriting keeps its spatial arrangement.
std::vector<model::PageItem> flow_items(const model::Page& page);

}  // namespace oneconv::render
