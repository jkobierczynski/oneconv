// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Page renderers and the notebook exporter.
#pragma once

#include <string>

#include "context.hpp"

namespace oneconv::render {

/// Navigation information for HTML pages.
struct NavInfo {
    std::string notebook_name;
    std::string section_path;  // e.g. "Group › Section"
    fs::path index;            // notebook index.html
    fs::path prev, next;       // neighbouring pages (empty if none)
    std::string prev_title, next_title;
};

std::string render_markdown(const model::Page& page, PageContext& ctx);
std::string render_html(const model::Page& page, PageContext& ctx, const NavInfo& nav);

struct ExportStats {
    int sections = 0;
    int pages = 0;
    int assets = 0;
    int failed_sections = 0;
    int encrypted_sections = 0;
};

/// Write a whole notebook (or a single section wrapped in a notebook) to `out_dir`.
ExportStats export_notebook(const model::Notebook& notebook, const fs::path& out_dir, const Options& opts);

/// Shared stylesheet used by HTML pages and the index.
const char* html_stylesheet();

}  // namespace oneconv::render
