// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Builds the document model (model.hpp) from a parsed revision store.
#pragma once

#include <string>
#include <vector>

#include "../onestore/store.hpp"
#include "model.hpp"

namespace oneconv {

/// Build a section from a .one store. `fallback_name` is used when the file
/// does not record a display name (typically the file name without extension).
model::Section build_section(const Store& store, const std::string& fallback_name);

struct TocEntry {
    std::string filename;  // section file name or section group folder name
    uint32_t order = 0;
    std::optional<model::Color> color;
};

/// Read the ordered entries of a .onetoc2 table of contents.
std::vector<TocEntry> read_toc(const Store& store);

}  // namespace oneconv
