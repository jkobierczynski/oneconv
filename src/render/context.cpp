// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "context.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

#include "../util/log.hpp"
#include "../util/text.hpp"

namespace oneconv::render {

using namespace model;

fs::path AssetWriter::unique_path(const std::string& preferred) {
    std::string name = sanitize_filename(preferred, 100);
    std::string stem = name, ext;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    std::string candidate = name;
    for (int i = 2; used_.count(to_lower(candidate)); ++i) candidate = stem + "-" + std::to_string(i) + ext;
    used_.insert(to_lower(candidate));
    return dir_ / u8path(candidate);
}

fs::path AssetWriter::write(const Blob& data, const std::string& preferred) {
    auto key = std::make_tuple(static_cast<const void*>(data.buffer.get()), data.offset, data.size);
    auto it = written_.find(key);
    if (it != written_.end()) return it->second;
    fs::path p = unique_path(preferred);
    std::error_code ec;
    fs::create_directories(dir_, ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path_utf8(p));
    if (data.size) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size));
    written_[key] = p;
    return p;
}

fs::path AssetWriter::write_text(const std::string& text, const std::string& preferred) {
    fs::path p = unique_path(preferred);
    std::error_code ec;
    fs::create_directories(dir_, ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path_utf8(p));
    f << text;
    return p;
}

std::string PageContext::rel(const fs::path& file) const { return relative_link(page_dir, file); }

std::string PageContext::resolve_link(const std::string& href) const {
    if (!starts_with(to_lower(href), "onenote:") || !links) return href;
    auto find_param = [&](const std::string& key) -> std::string {
        std::string lower = to_lower(href);
        size_t p = lower.find(key + "=");
        if (p == std::string::npos) return "";
        size_t s = p + key.size() + 1;
        size_t e = href.find('&', s);
        std::string v = href.substr(s, e == std::string::npos ? std::string::npos : e - s);
        v = url_decode(v);
        std::string up;
        for (char c : v) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (!up.empty() && up.front() != '{') up = "{" + up + "}";
        return up;
    };
    auto pick = [&](const LinkTable::Target& t) { return rel(for_html ? t.html : t.md); };
    std::string page = find_param("page-id");
    if (!page.empty()) {
        auto it = links->pages.find(page);
        if (it != links->pages.end()) return pick(it->second);
    }
    std::string section = find_param("section-id");
    if (!section.empty()) {
        auto it = links->sections.find(section);
        if (it != links->sections.end()) return pick(it->second);
    }
    return href;  // link into another notebook: keep it
}

// ---------------------------------------------------------------- ink -> SVG

namespace {
constexpr double kHimetricPerPx = 2540.0 / 96.0;

struct Bounds {
    double x0 = std::numeric_limits<double>::max(), y0 = std::numeric_limits<double>::max();
    double x1 = std::numeric_limits<double>::lowest(), y1 = std::numeric_limits<double>::lowest();
    bool valid() const { return x0 <= x1 && y0 <= y1; }
};

Bounds ink_bounds(const Ink& ink) {
    Bounds b;
    for (const auto& s : ink.strokes) {
        double pad = std::max(s.width, s.height) / 2.0;
        for (const auto& p : s.points) {
            b.x0 = std::min(b.x0, p.first - pad);
            b.y0 = std::min(b.y0, p.second - pad);
            b.x1 = std::max(b.x1, p.first + pad);
            b.y1 = std::max(b.y1, p.second + pad);
        }
    }
    return b;
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}
}  // namespace

std::pair<double, double> ink_size_px(const Ink& ink) {
    Bounds b = ink_bounds(ink);
    if (!b.valid()) return {0, 0};
    return {std::max(1.0, (b.x1 - b.x0) / kHimetricPerPx), std::max(1.0, (b.y1 - b.y0) / kHimetricPerPx)};
}

std::string ink_to_svg(const Ink& ink, bool standalone, const std::string& title) {
    Bounds b = ink_bounds(ink);
    if (!b.valid()) return "";
    double w = std::max(1.0, (b.x1 - b.x0) / kHimetricPerPx);
    double h = std::max(1.0, (b.y1 - b.y0) / kHimetricPerPx);
    std::string s;
    if (standalone) s += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + fmt(std::ceil(w)) + "\" height=\"" + fmt(std::ceil(h)) +
         "\" viewBox=\"0 0 " + fmt(w) + " " + fmt(h) + "\">";
    if (!title.empty()) s += "<title>" + html_escape(title) + "</title>";
    for (const auto& st : ink.strokes) {
        if (st.points.empty()) continue;
        std::string color = "#000000";
        if (st.color) {
            Color c;
            c.r = static_cast<uint8_t>(*st.color & 0xFF);
            c.g = static_cast<uint8_t>((*st.color >> 8) & 0xFF);
            c.b = static_cast<uint8_t>((*st.color >> 16) & 0xFF);
            color = c.hex();
        }
        double width = std::max(0.5, std::max(st.width, st.height) / kHimetricPerPx);
        double opacity = (255.0 - st.transparency) / 255.0;
        bool round = st.pen_tip == 0;
        std::string d = "M" + fmt((st.points[0].first - b.x0) / kHimetricPerPx) + " " +
                        fmt((st.points[0].second - b.y0) / kHimetricPerPx);
        if (st.points.size() == 1) d += "l0 0";
        for (size_t i = 1; i < st.points.size(); ++i)
            d += "L" + fmt((st.points[i].first - b.x0) / kHimetricPerPx) + " " +
                 fmt((st.points[i].second - b.y0) / kHimetricPerPx);
        s += "<path d=\"" + d + "\" fill=\"none\" stroke=\"" + color + "\" stroke-width=\"" + fmt(width) + "\"";
        if (opacity < 0.999) s += " stroke-opacity=\"" + fmt(opacity) + "\"";
        s += round ? " stroke-linecap=\"round\" stroke-linejoin=\"round\"" : " stroke-linecap=\"square\" stroke-linejoin=\"bevel\"";
        s += "/>";
    }
    s += "</svg>";
    if (standalone) s += "\n";
    return s;
}

bool generic_image_name(const std::string& filename) {
    std::string l = to_lower(trim(filename));
    return l.empty() || starts_with(l, "untitled picture") || starts_with(l, "image.") || l == "image";
}

std::vector<const PageItem*> reading_order(const Page& page) {
    std::vector<const PageItem*> items;
    for (const auto& it : page.items) items.push_back(&it);
    bool all_positioned = std::all_of(items.begin(), items.end(), [](const PageItem* i) { return i->y().has_value(); });
    if (!all_positioned) return items;
    // Items whose tops fall in the same 0.1" band are treated as one row (left to right).
    auto row = [](const PageItem* i) { return static_cast<long>(std::floor(*i->y() * 5.0f)); };
    std::stable_sort(items.begin(), items.end(), [&](const PageItem* a, const PageItem* b) {
        long ra = row(a), rb = row(b);
        if (ra != rb) return ra < rb;
        return a->x().value_or(0) < b->x().value_or(0);
    });
    return items;
}

}  // namespace oneconv::render

namespace oneconv::render {

std::vector<PageItem> flow_items(const Page& page) {
    std::vector<PageItem> out;
    std::shared_ptr<Ink> merged;
    auto fold = [](const Ink& src, Ink& dst) {
        float dx = src.offset_x.value_or(0) * 1270.0f;  // half-inch -> HIMETRIC
        float dy = src.offset_y.value_or(0) * 1270.0f;
        for (auto s : src.strokes) {
            for (auto& p : s.points) {
                p.first += dx;
                p.second += dy;
            }
            dst.strokes.push_back(std::move(s));
        }
        if (!src.recognized_text.empty())
            dst.recognized_text += (dst.recognized_text.empty() ? "" : " ") + src.recognized_text;
    };
    for (const PageItem* it : reading_order(page)) {
        if (it->kind == PageItem::Kind::Ink && it->ink) {
            if (!merged) {
                merged = std::make_shared<Ink>();
                merged->offset_x = it->ink->offset_x;
                merged->offset_y = it->ink->offset_y;
                PageItem pi;
                pi.kind = PageItem::Kind::Ink;
                pi.ink = merged;
                out.push_back(pi);
            }
            fold(*it->ink, *merged);
            continue;
        }
        merged.reset();
        out.push_back(*it);
    }
    return out;
}

}  // namespace oneconv::render
