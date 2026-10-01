// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Turns command-line inputs (.one, .onetoc2, notebook folders, .onepkg) into notebooks.
#pragma once

#include <filesystem>
#include <string>

#include "../one/model.hpp"

namespace oneconv {

struct LoadOptions {
    bool include_recycle_bin = false;
};

/// Load any supported input. Throws std::runtime_error for unreadable inputs;
/// individual broken sections are reported inside the notebook instead.
model::Notebook load_input(const std::filesystem::path& input, const LoadOptions& opts);

}  // namespace oneconv
