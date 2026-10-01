// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace oneconv {

void append_utf8(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string utf16_to_utf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        append_utf8(out, c);
    }
    return out;
}

std::string utf16le_to_utf8(const uint8_t* data, size_t size) {
    std::u16string s;
    s.reserve(size / 2);
    for (size_t i = 0; i + 1 < size; i += 2) {
        char16_t c = static_cast<char16_t>(data[i] | (data[i + 1] << 8));
        if (c == 0) break;
        s.push_back(c);
    }
    return utf16_to_utf8(s);
}

std::vector<uint32_t> utf8_codepoints(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int extra;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size()) {
                ok = false;
                break;
            }
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::u16string utf8_to_utf16(const std::string& s) {
    std::u16string out;
    for (uint32_t cp : utf8_codepoints(s)) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string cp1252_to_utf8(const uint8_t* data, size_t size) {
    static const uint16_t high[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                      0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        uint8_t c = data[i];
        if (c == 0) break;
        if (c >= 0x80 && c < 0xA0)
            append_utf8(out, high[c - 0x80]);
        else
            append_utf8(out, c);
    }
    return out;
}

std::string html_escape(const std::string& s, bool quotes) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"':
                if (quotes)
                    out += "&quot;";
                else
                    out.push_back(c);
                break;
            case '\'':
                if (quotes)
                    out += "&#39;";
                else
                    out.push_back(c);
                break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string md_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case '\\':
            case '`':
            case '*':
            case '_':
            case '[':
            case ']':
            case '<':
            case '>':
            case '|':
            case '~':
                out.push_back('\\');
                out.push_back(c);
                break;
            case '$':
                // Avoid accidental inline math in renderers that support $...$
                out += "\\$";
                break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string url_encode_path(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c >= 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string sanitize_filename(const std::string& in, size_t max_len) {
    std::string out;
    for (uint32_t cp : utf8_codepoints(in)) {
        if (cp < 0x20 || cp == 0x7F) continue;
        if (cp >= 0xFDD0 && cp <= 0xFDEF) continue;  // OneNote internal markers
        switch (cp) {
            case '<':
            case '>':
            case ':':
            case '"':
            case '/':
            case '\\':
            case '|':
            case '?':
            case '*':
                out.push_back('_');
                break;
            default: append_utf8(out, cp);
        }
    }
    // Collapse whitespace
    std::string collapsed;
    bool space = false;
    for (char c : out) {
        if (c == ' ' || c == '\t') {
            if (!space) collapsed.push_back(' ');
            space = true;
        } else {
            collapsed.push_back(c);
            space = false;
        }
    }
    collapsed = trim(collapsed);
    // Trailing dots/spaces are not allowed on Windows
    while (!collapsed.empty() && (collapsed.back() == '.' || collapsed.back() == ' ')) collapsed.pop_back();
    while (!collapsed.empty() && collapsed.front() == '.') collapsed.erase(collapsed.begin());
    // Truncate on a code point boundary
    if (collapsed.size() > max_len) {
        size_t cut = max_len;
        while (cut > 0 && (static_cast<unsigned char>(collapsed[cut]) & 0xC0) == 0x80) --cut;
        collapsed.resize(cut);
        collapsed = trim(collapsed);
    }
    // Reserved device names on Windows
    static const char* reserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                     "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                     "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string base = collapsed.substr(0, collapsed.find('.'));
    for (const char* r : reserved) {
        if (iequals(base, r)) {
            collapsed = "_" + collapsed;
            break;
        }
    }
    if (collapsed.empty()) collapsed = "untitled";
    return collapsed;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool iequals(const std::string& a, const std::string& b) { return to_lower(a) == to_lower(b); }

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::filesystem::path u8path(const std::string& s) {
#if defined(__cpp_char8_t)
    std::u8string u(s.begin(), s.end());
    return std::filesystem::path(u);
#else
    return std::filesystem::u8path(s);
#endif
}

std::string path_utf8(const std::filesystem::path& p) {
#if defined(__cpp_char8_t)
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

std::string relative_link(const std::filesystem::path& from_dir, const std::filesystem::path& to) {
    auto rel = to.lexically_normal().lexically_relative(from_dir.lexically_normal());
    if (rel.empty()) rel = to;
    std::string s = path_utf8(rel);
    std::replace(s.begin(), s.end(), '\\', '/');
    return url_encode_path(s);
}

namespace {
std::string format_utc(int64_t unix_seconds) {
    // Civil-from-days (Howard Hinnant), avoids platform-specific gmtime variants
    int64_t days = unix_seconds / 86400;
    int64_t rem = unix_seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02dZ", static_cast<long long>(y), m, d,
                  static_cast<int>(rem / 3600), static_cast<int>((rem % 3600) / 60), static_cast<int>(rem % 60));
    return buf;
}
}  // namespace

std::string filetime_to_iso(uint64_t ft) {
    if (ft == 0) return "";
    const uint64_t epoch_diff = 116444736000000000ULL;  // 1601 -> 1970 in 100ns
    if (ft < epoch_diff) return "";
    int64_t secs = static_cast<int64_t>((ft - epoch_diff) / 10000000ULL);
    if (secs > 253402300799LL) return "";  // > year 9999
    return format_utc(secs);
}

std::string time32_to_iso(uint32_t t) {
    if (t == 0) return "";
    return format_utc(315532800LL + t);  // 1980-01-01
}

}  // namespace oneconv
