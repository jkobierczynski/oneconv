// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Renders ink strokes to a PNG image, for output formats that cannot show SVG.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "context.hpp"

namespace oneconv::render {

using namespace model;

namespace {

constexpr double kHimetricPerPx = 2540.0 / 96.0;

// ---------------------------------------------------------------- PNG writer

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/// DEFLATE with fixed Huffman codes and a tiny match finder. Drawings are mostly
/// uniform background, which "repeat the previous pixel / the row above" compresses well.
class Deflater {
public:
    explicit Deflater(Buffer& out) : out_(out) {}

    void compress(const uint8_t* data, size_t n, size_t stride, size_t bpp) {
        put_bits(1, 1);  // final block
        put_bits(1, 2);  // fixed Huffman
        const size_t candidates[3] = {1, bpp, stride};
        size_t i = 0;
        while (i < n) {
            size_t best_len = 0, best_dist = 0;
            for (size_t d : candidates) {
                if (d == 0 || d > i || d > 32768) continue;
                size_t max_len = std::min<size_t>(258, n - i);
                size_t len = 0;
                while (len < max_len && data[i + len] == data[i + len - d]) ++len;
                if (len > best_len) {
                    best_len = len;
                    best_dist = d;
                }
            }
            if (best_len >= 4) {
                put_length(best_len);
                put_distance(best_dist);
                i += best_len;
            } else {
                put_literal(data[i]);
                ++i;
            }
        }
        put_symbol(256);  // end of block
        if (bit_count_) out_.push_back(static_cast<uint8_t>(bit_buf_));
    }

private:
    Buffer& out_;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;

    void put_bits(uint32_t value, int count) {  // LSB first
        bit_buf_ |= value << bit_count_;
        bit_count_ += count;
        while (bit_count_ >= 8) {
            out_.push_back(static_cast<uint8_t>(bit_buf_));
            bit_buf_ >>= 8;
            bit_count_ -= 8;
        }
    }
    void put_code(uint32_t code, int len) {  // Huffman codes go MSB first
        uint32_t rev = 0;
        for (int i = 0; i < len; ++i) rev |= ((code >> i) & 1u) << (len - 1 - i);
        put_bits(rev, len);
    }
    void put_symbol(uint32_t sym) {
        if (sym < 144)
            put_code(0x30 + sym, 8);
        else if (sym < 256)
            put_code(0x190 + (sym - 144), 9);
        else if (sym < 280)
            put_code(sym - 256, 7);
        else
            put_code(0xC0 + (sym - 280), 8);
    }
    void put_literal(uint8_t b) { put_symbol(b); }
    void put_length(size_t len) {
        static const uint16_t base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        int idx = 28;
        while (base[idx] > len) --idx;
        put_symbol(257 + static_cast<uint32_t>(idx));
        if (extra[idx]) put_bits(static_cast<uint32_t>(len - base[idx]), extra[idx]);
    }
    void put_distance(size_t dist) {
        static const uint16_t base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        int idx = 29;
        while (base[idx] > dist) --idx;
        put_code(static_cast<uint32_t>(idx), 5);
        if (extra[idx]) put_bits(static_cast<uint32_t>(dist - base[idx]), extra[idx]);
    }
};

void put_u32(Buffer& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

void put_chunk(Buffer& png, const char type[4], const Buffer& data) {
    put_u32(png, static_cast<uint32_t>(data.size()));
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put_u32(png, crc32(png.data() + start, png.size() - start));
}

/// 8-bit RGB PNG. `dpi` is recorded so that viewers show a 2x image at its intended size.
Buffer encode_png(const std::vector<uint8_t>& rgb, int w, int h, int dpi) {
    const size_t stride = static_cast<size_t>(w) * 3 + 1;
    Buffer raw(stride * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        raw[static_cast<size_t>(y) * stride] = 0;  // filter: none
        std::memcpy(&raw[static_cast<size_t>(y) * stride + 1], &rgb[static_cast<size_t>(y) * static_cast<size_t>(w) * 3],
                    static_cast<size_t>(w) * 3);
    }
    Buffer z = {0x78, 0x01};  // zlib header
    Deflater(z).compress(raw.data(), raw.size(), stride, 3);
    uint32_t a = 1, b = 0;  // Adler-32
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put_u32(z, (b << 16) | a);

    Buffer png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Buffer ihdr;
    put_u32(ihdr, static_cast<uint32_t>(w));
    put_u32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bit, RGB
    put_chunk(png, "IHDR", ihdr);
    Buffer phys;
    uint32_t ppm = static_cast<uint32_t>(std::lround(dpi / 0.0254));
    put_u32(phys, ppm);
    put_u32(phys, ppm);
    phys.push_back(1);  // unit: metre
    put_chunk(png, "pHYs", phys);
    put_chunk(png, "IDAT", z);
    put_chunk(png, "IEND", Buffer());
    return png;
}

}  // namespace

PngImage ink_to_png(const Ink& ink) {
    PngImage result;
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = std::numeric_limits<double>::lowest(), y1 = x1;
    for (const auto& s : ink.strokes) {
        double pad = std::max(s.width, s.height) / 2.0;
        for (const auto& p : s.points) {
            x0 = std::min(x0, p.first - pad);
            y0 = std::min(y0, p.second - pad);
            x1 = std::max(x1, p.first + pad);
            y1 = std::max(y1, p.second + pad);
        }
    }
    if (x0 > x1 || y0 > y1) return result;

    const double margin = 2.0;  // CSS px around the drawing
    // Whole CSS pixels, so that the bitmap is exactly `scale` times the size it is displayed at.
    double css_w = std::max(1.0, std::ceil((x1 - x0) / kHimetricPerPx + 2 * margin));
    double css_h = std::max(1.0, std::ceil((y1 - y0) / kHimetricPerPx + 2 * margin));
    if (!std::isfinite(css_w) || !std::isfinite(css_h)) return result;
    double scale = 2.0;  // device pixels per CSS px
    while (css_w * scale * css_h * scale > 24e6 && scale > 0.05) scale /= 2;
    if (css_w * scale > 12000 || css_h * scale > 12000) scale = std::min(12000 / css_w, 12000 / css_h);
    const int w = std::max(1, static_cast<int>(std::lround(css_w * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(css_h * scale)));
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 255);
    const double k = scale / kHimetricPerPx;
    auto px = [&](double v, double origin) { return (v - origin) * k + margin * scale; };

    std::vector<uint8_t> mask;
    for (const auto& s : ink.strokes) {
        if (s.points.empty()) continue;
        const double r = std::max(0.6 * scale / 2, std::max(s.width, s.height) * k / 2.0);
        // Stroke bounding box in device pixels
        double sx0 = w, sy0 = h, sx1 = 0, sy1 = 0;
        for (const auto& p : s.points) {
            sx0 = std::min(sx0, px(p.first, x0));
            sy0 = std::min(sy0, px(p.second, y0));
            sx1 = std::max(sx1, px(p.first, x0));
            sy1 = std::max(sy1, px(p.second, y0));
        }
        const int bx0 = std::max(0, static_cast<int>(std::floor(sx0 - r - 1))), by0 = std::max(0, static_cast<int>(std::floor(sy0 - r - 1)));
        const int bx1 = std::min(w - 1, static_cast<int>(std::ceil(sx1 + r + 1))), by1 = std::min(h - 1, static_cast<int>(std::ceil(sy1 + r + 1)));
        if (bx1 < bx0 || by1 < by0) continue;
        const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
        // Coverage of the whole stroke first, so a translucent stroke does not darken
        // where its own segments overlap.
        mask.assign(static_cast<size_t>(bw) * static_cast<size_t>(bh), 0);
        auto segment = [&](double ax, double ay, double bx, double by) {
            const int mx0 = std::max(bx0, static_cast<int>(std::floor(std::min(ax, bx) - r - 1)));
            const int mx1 = std::min(bx1, static_cast<int>(std::ceil(std::max(ax, bx) + r + 1)));
            const int my0 = std::max(by0, static_cast<int>(std::floor(std::min(ay, by) - r - 1)));
            const int my1 = std::min(by1, static_cast<int>(std::ceil(std::max(ay, by) + r + 1)));
            const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
            for (int y = my0; y <= my1; ++y) {
                for (int x = mx0; x <= mx1; ++x) {
                    const double cx = x + 0.5, cy = y + 0.5;
                    double t = len2 > 0 ? ((cx - ax) * dx + (cy - ay) * dy) / len2 : 0;
                    t = std::max(0.0, std::min(1.0, t));
                    const double ex = cx - (ax + t * dx), ey = cy - (ay + t * dy);
                    const double cov = r + 0.5 - std::sqrt(ex * ex + ey * ey);  // 1 px anti-aliasing ramp
                    if (cov <= 0) continue;
                    const uint8_t c = static_cast<uint8_t>(std::min(1.0, cov) * 255.0 + 0.5);
                    uint8_t& m = mask[static_cast<size_t>(y - by0) * static_cast<size_t>(bw) + static_cast<size_t>(x - bx0)];
                    if (c > m) m = c;
                }
            }
        };
        if (s.points.size() == 1) {
            segment(px(s.points[0].first, x0), px(s.points[0].second, y0), px(s.points[0].first, x0), px(s.points[0].second, y0));
        } else {
            for (size_t i = 1; i < s.points.size(); ++i)
                segment(px(s.points[i - 1].first, x0), px(s.points[i - 1].second, y0), px(s.points[i].first, x0),
                        px(s.points[i].second, y0));
        }
        uint32_t color = s.color.value_or(0);
        const int cr = static_cast<int>(color & 0xFF), cg = static_cast<int>((color >> 8) & 0xFF), cb = static_cast<int>((color >> 16) & 0xFF);
        const int opacity = 255 - s.transparency;
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                const int m = mask[static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x)];
                if (!m) continue;
                const int alpha = m * opacity / 255;
                uint8_t* p = &rgb[(static_cast<size_t>(y + by0) * static_cast<size_t>(w) + static_cast<size_t>(x + bx0)) * 3];
                p[0] = static_cast<uint8_t>((cr * alpha + p[0] * (255 - alpha)) / 255);
                p[1] = static_cast<uint8_t>((cg * alpha + p[1] * (255 - alpha)) / 255);
                p[2] = static_cast<uint8_t>((cb * alpha + p[2] * (255 - alpha)) / 255);
            }
        }
    }
    result.data = encode_png(rgb, w, h, static_cast<int>(std::lround(96 * scale)));
    // Oversized drawings were scaled down above; show those at their bitmap size.
    result.width = scale >= 1.0 ? static_cast<int>(css_w) : w;
    result.height = scale >= 1.0 ? static_cast<int>(css_h) : h;
    return result;
}

}  // namespace oneconv::render
