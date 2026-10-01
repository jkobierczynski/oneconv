// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Unicode and string helpers.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace oneconv {

/// Decode little-endian UTF-16 bytes to UTF-8. Lone surrogates become U+FFFD.
/// A trailing NUL terminator (and anything after it) is dropped.
std::string utf16le_to_utf8(const uint8_t* data, size_t size);
inline std::string utf16le_to_utf8(const std::vector<uint8_t>& v) { return utf16le_to_utf8(v.data(), v.size()); }

/// UTF-16 code units -> UTF-8.
std::string utf16_to_utf8(const std::u16string& s);
/// UTF-8 -> UTF-16 code units (invalid sequences become U+FFFD).
std::u16string utf8_to_utf16(const std::string& s);
/// Decode Windows-1252 bytes (used by TextExtendedAscii) to UTF-8.
std::string cp1252_to_utf8(const uint8_t* data, size_t size);
/// Append a code point as UTF-8.
void append_utf8(std::string& out, uint32_t cp);
/// Decode UTF-8 into code points.
std::vector<uint32_t> utf8_codepoints(const std::string& s);

std::string html_escape(const std::string& s, bool quotes = true);
/// Escape characters that would otherwise be interpreted as Markdown syntax.
std::string md_escape(const std::string& s);
/// Percent-encode a relative path for use inside Markdown/HTML links.
std::string url_encode_path(const std::string& s);
std::string url_decode(const std::string& s);

/// Make a string safe to use as a file or directory name on Windows, macOS and Linux.
std::string sanitize_filename(const std::string& s, size_t max_len = 120);

std::string trim(const std::string& s);
std::string to_lower(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
bool iequals(const std::string& a, const std::string& b);
std::string replace_all(std::string s, const std::string& from, const std::string& to);

/// Portable conversion between UTF-8 strings and filesystem paths.
std::filesystem::path u8path(const std::string& s);
std::string path_utf8(const std::filesystem::path& p);

/// Relative link from directory `from_dir` to file `to`, using '/' separators.
std::string relative_link(const std::filesystem::path& from_dir, const std::filesystem::path& to);

/// FILETIME (100ns ticks since 1601) -> ISO 8601 UTC string. Empty if out of range.
std::string filetime_to_iso(uint64_t ft);
/// Seconds since 1980-01-01 UTC (OneNote Time32) -> ISO 8601 UTC string.
std::string time32_to_iso(uint32_t t);

}  // namespace oneconv
