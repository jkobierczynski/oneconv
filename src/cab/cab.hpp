// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Microsoft Cabinet (.cab / .onepkg) reader with stored, MSZIP and LZX support.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../util/bytes.hpp"

namespace oneconv::cab {

struct File {
    std::string name;  // path inside the cabinet, '\' separators converted to '/'
    Blob data;
};

/// Extract every file of a cabinet held in memory. Throws ParseError.
std::vector<File> extract(const BufferPtr& cabinet);

/// Decompress an LZX stream (as stored in CAB folders) of `out_size` bytes.
/// `window_bits` is 15..21. Input is the concatenation of the folder's data blocks.
Buffer lzx_decompress(const std::vector<std::pair<const uint8_t*, size_t>>& blocks, int window_bits, size_t out_size);

/// Inflate one complete raw DEFLATE stream, appending to `out`. Back-references may
/// reach into data already in `out` (needed for MSZIP's shared history).
void inflate_append(const uint8_t* in, size_t size, Buffer& out, size_t max_out);

}  // namespace oneconv::cab
