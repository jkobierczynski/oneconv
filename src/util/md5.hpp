// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// MD5 (RFC 1321) and base64 (RFC 4648). ENEX identifies attachments by their MD5 digest.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

namespace oneconv {

/// Lower-case hexadecimal MD5 digest.
std::string md5_hex(const uint8_t* data, size_t size);

/// Write base64 text, wrapped at `line_width` characters (0 = no wrapping).
void write_base64(std::ostream& out, const uint8_t* data, size_t size, size_t line_width = 76);
std::string base64(const uint8_t* data, size_t size);

}  // namespace oneconv
