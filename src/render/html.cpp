// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Standalone HTML page renderer.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../util/text.hpp"
#include "render.hpp"

namespace oneconv::render {

using namespace model;

const char* html_stylesheet() {
    return R"CSS(
:root{--fg:#1f2328;--muted:#656d76;--bg:#ffffff;--rule:#d0d7de;--accent:#7719aa;--code:#f6f8fa;--mark:#fff3a3}
@media (prefers-color-scheme:dark){:root{--fg:#e6edf3;--muted:#8d96a0;--bg:#0d1117;--rule:#30363d;--accent:#c58af9;--code:#161b22;--mark:#6b5d00}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 -apple-system,"Segoe UI",Calibri,Roboto,"Helvetica Neue",Arial,sans-serif}
main{max-width:52rem;margin:0 auto;padding:1.5rem 1.25rem 4rem}
main.canvas-page{max-width:none}
nav.crumbs{font-size:.85rem;color:var(--muted);display:flex;flex-wrap:wrap;gap:.5rem 1rem;justify-content:space-between;border-bottom:1px solid var(--rule);padding-bottom:.6rem;margin-bottom:1.2rem}
nav.crumbs a{color:var(--accent);text-decoration:none}
nav.crumbs a:hover{text-decoration:underline}
header.page-head h1{font-size:1.9rem;line-height:1.25;margin:.2rem 0 .3rem}
header.page-head .meta{color:var(--muted);font-size:.85rem;margin:0 0 1.4rem}
a{color:var(--accent)}
p{margin:.35rem 0}
h1,h2,h3,h4,h5,h6{line-height:1.3;margin:1.1rem 0 .4rem}
.outline{margin:0 0 1.2rem}
.indent{margin-left:1.6rem}
ul,ol{margin:.25rem 0;padding-left:1.6rem}
li>p:first-child{margin-top:0}
ul.tasks{list-style:none;padding-left:.2rem}
ul.tasks>li>input{margin:0 .45rem 0 0;vertical-align:-.1em}
ul.bullets-custom{list-style:none;padding-left:.2rem}
ul.bullets-custom>li>.bullet{display:inline-block;width:1.2rem}
blockquote{margin:.5rem 0;padding:.1rem 1rem;border-left:3px solid var(--rule);color:var(--muted)}
.cite{color:var(--muted);font-style:italic}
pre.code{background:var(--code);padding:.75rem 1rem;border-radius:6px;overflow:auto;font:13.5px/1.45 ui-monospace,Consolas,"SF Mono",Menlo,monospace}
mark{background:var(--mark);color:inherit}
table.grid{border-collapse:collapse;margin:.6rem 0;max-width:100%}
table.grid td{border:1px solid var(--rule);padding:.3rem .55rem;vertical-align:top}
table.grid.noborder td{border-color:transparent}
figure{margin:.6rem 0}
figure img{max-width:100%;height:auto}
figure figcaption{color:var(--muted);font-size:.85rem}
.tag{display:inline-block;margin-right:.3rem}
a.attachment{display:inline-flex;gap:.4rem;align-items:center;padding:.25rem .6rem;border:1px solid var(--rule);border-radius:6px;text-decoration:none}
.ink svg{max-width:100%;height:auto}
.ink-inline svg{vertical-align:middle}
.canvas{position:relative}
.canvas>.item{position:absolute}
math[display=block]{margin:.5rem 0}
.empty{height:.8rem}
.unavailable{color:var(--muted);font-style:italic}
.toc ul{list-style:none;padding-left:1.1rem}
.toc>ul{padding-left:0}
.toc li{margin:.15rem 0}
.toc .section{margin:1.4rem 0 .4rem;font-size:1.15rem;font-weight:600;border-left:4px solid var(--accent);padding-left:.55rem}
.toc .group{font-weight:600;margin:1rem 0 .2rem;color:var(--muted)}
.toc .count{color:var(--muted);font-weight:400;font-size:.85rem}
)CSS";
}

namespace {

constexpr double kPxPerHalfInch = 48.0;

std::string px(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0fpx", v);
    return buf;
}

class HtmlRenderer {
public:
    HtmlRenderer(const Page& page, PageContext& ctx, const NavInfo& nav)
        : page_(page), ctx_(ctx), opts_(*ctx.opts), nav_(nav) {}

    std::string render() {
        std::string title = page_.title.empty() ? "Untitled Page" : page_.title;
        std::string s = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
                        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
        s += "<title>" + html_escape(title) + "</title>\n";
        if (!page_.created.empty()) s += "<meta name=\"created\" content=\"" + page_.created + "\">\n";
        if (!page_.modified.empty()) s += "<meta name=\"modified\" content=\"" + page_.modified + "\">\n";
        if (!page_.author.empty()) s += "<meta name=\"author\" content=\"" + html_escape(page_.author) + "\">\n";
        s += "<style>" + std::string(html_stylesheet()) + "</style>\n</head>\n<body>\n";
        s += std::string("<main") + (opts_.html_canvas ? " class=\"canvas-page\"" : "") + ">\n";
        s += nav_html();
        s += "<article>\n<header class=\"page-head\"><h1>" + html_escape(title) + "</h1>";
        std::string meta;
        std::string date = page_.title_date.empty() ? page_.created.substr(0, std::min<size_t>(10, page_.created.size()))
                                                    : page_.title_date;
        if (opts_.include_date && !date.empty()) meta += html_escape(date);
        if (!page_.author.empty()) meta += (meta.empty() ? "" : " · ") + html_escape(page_.author);
        if (!meta.empty()) s += "<p class=\"meta\">" + meta + "</p>";
        s += "</header>\n";
        s += opts_.html_canvas ? canvas_body() : flow_body();
        s += "</article>\n</main>\n</body>\n</html>\n";
        return s;
    }

private:
    const Page& page_;
    PageContext& ctx_;
    const Options& opts_;
    const NavInfo& nav_;
    bool in_list_ = false;

    std::string nav_html() {
        if (nav_.index.empty()) return "";
        std::string s = "<nav class=\"crumbs\"><span><a href=\"" + html_escape(ctx_.rel(nav_.index)) + "\">" +
                        html_escape(nav_.notebook_name) + "</a> › " + html_escape(nav_.section_path) + "</span><span>";
        if (!nav_.prev.empty())
            s += "<a href=\"" + html_escape(ctx_.rel(nav_.prev)) + "\">← " + html_escape(nav_.prev_title) + "</a>";
        if (!nav_.prev.empty() && !nav_.next.empty()) s += " &nbsp; ";
        if (!nav_.next.empty())
            s += "<a href=\"" + html_escape(ctx_.rel(nav_.next)) + "\">" + html_escape(nav_.next_title) + " →</a>";
        return s + "</span></nav>\n";
    }

    std::string item_html(const PageItem& item) {
        switch (item.kind) {
            case PageItem::Kind::Outline: return "<div class=\"outline\">" + elements(item.outline->elements) + "</div>\n";
            case PageItem::Kind::Image: return image_html(*item.image) + "\n";
            case PageItem::Kind::Attachment: return "<p>" + attachment_html(*item.attachment) + "</p>\n";
            case PageItem::Kind::Ink: return "<div class=\"ink\">" + ink_html(*item.ink) + "</div>\n";
        }
        return "";
    }

    std::string flow_body() {
        std::string s;
        for (const PageItem& item : flow_items(page_)) s += item_html(item);
        return s;
    }

    std::string canvas_body() {
        std::string s;
        double max_bottom = 0;
        for (const auto& item : page_.items) {
            double x = item.x().value_or(0) * kPxPerHalfInch;
            double y = item.y().value_or(0) * kPxPerHalfInch;
            std::string style = "left:" + px(x) + ";top:" + px(y);
            double est_h = 200;
            if (item.kind == PageItem::Kind::Outline && item.outline->width)
                style += ";width:" + px(*item.outline->width * kPxPerHalfInch + 20);
            if (item.kind == PageItem::Kind::Image && item.image->height > 0) est_h = item.image->height * kPxPerHalfInch;
            if (item.kind == PageItem::Kind::Ink) est_h = ink_size_px(*item.ink).second;
            max_bottom = std::max(max_bottom, y + est_h);
            s += "<div class=\"item\" style=\"" + style + "\">" + item_html(item) + "</div>\n";
        }
        return "<div class=\"canvas\" style=\"min-height:" + px(max_bottom + 200) + "\">\n" + s + "</div>\n";
    }

    // ------------------------------------------------------------------ inline

    std::string style_attr(const TextStyle& st) {
        std::string css;
        if (st.color) css += "color:" + st.color->hex() + ";";
        if (!st.font.empty() && st.font != "Calibri" && st.font != "Calibri Light")
            css += "font-family:'" + html_escape(st.font) + "';";
        if (st.font_size_half_pt > 0 && st.font_size_half_pt != 22) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "font-size:%.1fpt;", st.font_size_half_pt / 2.0);
            css += buf;
        }
        return css;
    }

    std::string text_html(const Inline& in) {
        std::string s = html_escape(in.text, false);
        const TextStyle& st = in.style;
        if (st.superscript) s = "<sup>" + s + "</sup>";
        if (st.subscript) s = "<sub>" + s + "</sub>";
        if (st.strike) s = "<s>" + s + "</s>";
        if (st.underline) s = "<u>" + s + "</u>";
        if (st.italic) s = "<em>" + s + "</em>";
        if (st.bold) s = "<strong>" + s + "</strong>";
        // A white highlight is invisible on light themes and wrong on dark ones; drop it.
        if (st.highlight && !(*st.highlight == Color{255, 255, 255}))
            s = "<mark style=\"background:" + st.highlight->hex() + "\">" + s + "</mark>";
        std::string css = style_attr(st);
        if (!css.empty()) s = "<span style=\"" + css + "\">" + s + "</span>";
        return s;
    }

    std::string inlines(const std::vector<Inline>& ins) {
        std::string s;
        size_t i = 0;
        while (i < ins.size()) {
            const Inline& in = ins[i];
            if (in.kind == Inline::Kind::Text && !in.href.empty()) {
                std::string label;
                size_t j = i;
                while (j < ins.size() && ins[j].kind == Inline::Kind::Text && ins[j].href == in.href) label += text_html(ins[j++]);
                s += "<a href=\"" + html_escape(ctx_.resolve_link(in.href)) + "\">" + label + "</a>";
                i = j;
                continue;
            }
            switch (in.kind) {
                case Inline::Kind::Text: s += text_html(in); break;
                case Inline::Kind::Break: s += "<br>"; break;
                case Inline::Kind::Math:
                    s += in.math ? math_to_mathml(*in.math, in.math_display && !in_list_) : html_escape(in.text);
                    break;
                case Inline::Kind::Ink:
                    if (in.ink) s += "<span class=\"ink-inline\">" + ink_html(*in.ink) + "</span>";
                    break;
            }
            ++i;
        }
        return s;
    }

    std::string tags_html(const std::vector<NoteTag>& tags, bool skip_checkbox) {
        std::string s;
        for (const auto& t : tags) {
            if (skip_checkbox && t.is_checkbox()) continue;
            if (t.is_checkbox()) {
                s += std::string("<input type=\"checkbox\" disabled") + (t.completed ? " checked" : "") + " title=\"" +
                     html_escape(t.label) + "\"> ";
                continue;
            }
            std::string sym = t.symbol();
            if (sym.empty() && t.label.empty()) continue;
            std::string style;
            if (t.highlight) style = " style=\"background:" + t.highlight->hex() + "\"";
            s += "<span class=\"tag\" title=\"" + html_escape(t.label) + "\"" + style + ">" +
                 html_escape(sym.empty() ? "[" + t.label + "]" : sym) + "</span>";
        }
        return s;
    }

    // ------------------------------------------------------------------ blocks

    std::string paragraph_html(const Paragraph& p, bool skip_checkbox) {
        std::string body = tags_html(p.tags, skip_checkbox) + inlines(p.inlines);
        std::string attrs;
        if (p.align == Align::Center) attrs += " style=\"text-align:center\"";
        if (p.align == Align::Right) attrs += " style=\"text-align:right\"";
        if (p.rtl) attrs += " dir=\"rtl\"";
        if (p.empty() && p.tags.empty()) return "<div class=\"empty\"></div>";
        int level = p.heading_level();
        if (p.style_id == "PageTitle") level = 1;
        if (level > 0) {
            int n = std::min(6, level + opts_.heading_offset);
            std::string tag = "h" + std::to_string(n);
            return "<" + tag + attrs + ">" + body + "</" + tag + ">";
        }
        if (p.style_id == "blockquote") return "<blockquote" + attrs + "><p>" + body + "</p></blockquote>";
        if (p.style_id == "cite") return "<p class=\"cite\"" + attrs + ">" + body + "</p>";
        if (!in_list_ && p.inlines.size() == 1 && p.inlines[0].kind == Inline::Kind::Math && p.inlines[0].math_display)
            return "<div" + attrs + ">" + body + "</div>";
        return "<p" + attrs + ">" + body + "</p>";
    }

    std::string content_html(const Content& c, bool skip_checkbox) {
        switch (c.kind) {
            case Content::Kind::Paragraph: return paragraph_html(*c.paragraph, skip_checkbox);
            case Content::Kind::Table: return table_html(*c.table);
            case Content::Kind::Image: return image_html(*c.image);
            case Content::Kind::Attachment: return "<p>" + attachment_html(*c.attachment) + "</p>";
            case Content::Kind::Ink: return "<div class=\"ink\">" + ink_html(*c.ink) + "</div>";
        }
        return "";
    }

    static std::optional<bool> checkbox_state(const OutlineElement& el) {
        for (const auto& c : el.contents) {
            if (c.kind != Content::Kind::Paragraph) continue;
            for (const auto& t : c.paragraph->tags)
                if (t.is_checkbox()) return t.completed;
            break;
        }
        return std::nullopt;
    }

    static bool is_code(const OutlineElement& el) {
        return !el.list && el.contents.size() == 1 && el.contents[0].kind == Content::Kind::Paragraph &&
               el.contents[0].paragraph->style_id == "code";
    }

    enum class ListKind { None, Bullet, Ordered, Task };

    static ListKind kind_of(const OutlineElement& el) {
        if (checkbox_state(el)) return ListKind::Task;
        if (!el.list) return ListKind::None;
        return el.list->ordered ? ListKind::Ordered : ListKind::Bullet;
    }

    std::string list_open(ListKind k, const OutlineElement& first) {
        if (k == ListKind::Task) return "<ul class=\"tasks\">";
        if (k == ListKind::Ordered) {
            static const char* types[] = {"1", "I", "i", "A", "a"};
            std::string s = "<ol";
            int ns = first.list->number_style;
            if (ns > 0 && ns <= 4) s += std::string(" type=\"") + types[ns] + "\"";
            if (first.list->restart && *first.list->restart != 1) s += " start=\"" + std::to_string(*first.list->restart) + "\"";
            return s + ">";
        }
        const std::string& b = first.list->bullet;
        if (b.empty() || b == "•") return "<ul>";
        if (b == "◦" || b == "o") return "<ul style=\"list-style-type:circle\">";
        if (b == "▪" || b == "■") return "<ul style=\"list-style-type:square\">";
        return "<ul class=\"bullets-custom\">";
    }

    std::string list_item(const OutlineElement& el, ListKind k) {
        std::string s = "<li>";
        if (k == ListKind::Task) {
            s += std::string("<input type=\"checkbox\" disabled") + (*checkbox_state(el) ? " checked" : "") + ">";
        } else if (k == ListKind::Bullet && el.list && !el.list->bullet.empty() && el.list->bullet != "•" &&
                   el.list->bullet != "◦" && el.list->bullet != "o" && el.list->bullet != "▪" &&
                   el.list->bullet != "■") {
            s += "<span class=\"bullet\">" + html_escape(el.list->bullet) + "</span>";
        }
        bool first = true;
        for (const auto& c : el.contents) {
            in_list_ = true;
            std::string body = content_html(c, true);
            in_list_ = false;
            if (first && starts_with(body, "<div>") && ends_with(body, "</div>")) body = body.substr(5, body.size() - 11);
            // First paragraph inline with the marker
            if (first && c.kind == Content::Kind::Paragraph && starts_with(body, "<p>") && ends_with(body, "</p>"))
                body = body.substr(3, body.size() - 7);
            s += body;
            first = false;
        }
        if (!el.children.empty()) s += elements(el.children);
        return s + "</li>";
    }

    std::string elements(const std::vector<OutlineElement>& els) {
        std::string s;
        size_t i = 0;
        while (i < els.size()) {
            const OutlineElement& el = els[i];
            if (is_code(el)) {
                std::string code;
                while (i < els.size() && is_code(els[i])) {
                    code += html_escape(els[i].contents[0].paragraph->plain_text(), false) + "\n";
                    ++i;
                }
                s += "<pre class=\"code\"><code>" + code + "</code></pre>";
                continue;
            }
            ListKind k = kind_of(el);
            if (k == ListKind::None) {
                for (const auto& c : el.contents) s += content_html(c, false);
                if (!el.children.empty()) s += "<div class=\"indent\">" + elements(el.children) + "</div>";
                ++i;
                continue;
            }
            std::string open = list_open(k, el);
            std::string close = (k == ListKind::Ordered) ? "</ol>" : "</ul>";
            s += open;
            while (i < els.size() && kind_of(els[i]) == k && !is_code(els[i])) {
                if (k == ListKind::Bullet && els[i].list->bullet != el.list->bullet) break;
                s += list_item(els[i], k);
                ++i;
            }
            s += close;
        }
        return s;
    }

    // ---------------------------------------------------------------- tables

    std::string table_html(const Table& t) {
        std::string s = tags_html(t.tags, false);
        s += std::string("<table class=\"grid") + (t.borders ? "" : " noborder") + "\">";
        if (!t.col_widths.empty()) {
            s += "<colgroup>";
            for (float w : t.col_widths) s += "<col style=\"width:" + px(w * kPxPerHalfInch) + "\">";
            s += "</colgroup>";
        }
        for (const auto& row : t.rows) {
            s += "<tr>";
            for (const auto& cell : row) {
                s += "<td";
                if (cell.background) s += " style=\"background:" + cell.background->hex() + "\"";
                s += ">" + elements(cell.elements) + "</td>";
            }
            s += "</tr>";
        }
        return s + "</table>";
    }

    // ------------------------------------------------------- images and files

    std::string image_html(const Image& img) {
        std::string alt = !img.alt.empty() ? img.alt : (!generic_image_name(img.filename) ? img.filename : "");
        if (alt.empty() && !img.ocr_text.empty()) alt = img.ocr_text.substr(0, 300);
        std::string s = "<figure>" + tags_html(img.tags, false);
        if (img.missing || img.data.empty()) {
            s += "<span class=\"unavailable\">[image unavailable" + (alt.empty() ? "" : ": " + html_escape(alt)) + "]</span>";
        } else {
            std::string name = !generic_image_name(img.filename) ? img.filename
                                                     : ctx_.slug + "-image-" + std::to_string(++ctx_.image_counter) +
                                                           (img.ext.empty() ? ".bin" : img.ext);
            if (!generic_image_name(img.filename) && !img.ext.empty() && !ends_with(to_lower(name), to_lower(img.ext)))
                name += img.ext;
            fs::path p = ctx_.assets->write(img.data, name);
            std::string tag = "<img src=\"" + html_escape(ctx_.rel(p)) + "\" alt=\"" + html_escape(alt) + "\"";
            if (img.width > 0) tag += " width=\"" + std::to_string(static_cast<int>(std::lround(img.width * kPxPerHalfInch))) + "\"";
            if (img.height > 0) tag += " height=\"" + std::to_string(static_cast<int>(std::lround(img.height * kPxPerHalfInch))) + "\"";
            tag += " loading=\"lazy\">";
            if (!img.link.empty()) tag = "<a href=\"" + html_escape(ctx_.resolve_link(img.link)) + "\">" + tag + "</a>";
            s += tag;
        }
        for (const auto& e : img.embeds)
            s += "<figcaption><a href=\"" + html_escape(e) + "\">▶ " + html_escape(e) + "</a></figcaption>";
        return s + "</figure>";
    }

    std::string attachment_html(const Attachment& a) {
        std::string icon = a.media == Attachment::Media::Audio   ? "\U0001F3B5"
                           : a.media == Attachment::Media::Video ? "\U0001F3AC"
                                                                 : "\U0001F4CE";
        std::string tags = tags_html(a.tags, false);
        if (a.missing || a.data.empty())
            return tags + "<span class=\"unavailable\">" + icon + " " + html_escape(a.name) + " (file not available)</span>";
        fs::path p = ctx_.assets->write(a.data, a.name);
        std::string href = html_escape(ctx_.rel(p));
        std::string s = tags;
        if (a.media == Attachment::Media::Audio) s += "<audio controls src=\"" + href + "\"></audio><br>";
        if (a.media == Attachment::Media::Video) s += "<video controls src=\"" + href + "\" style=\"max-width:100%\"></video><br>";
        return s + "<a class=\"attachment\" href=\"" + href + "\" download>" + icon + " " + html_escape(a.name) + "</a>";
    }

    std::string ink_html(const Ink& ink) {
        if (!opts_.ink || ink.empty()) return html_escape(ink.recognized_text);
        return ink_to_svg(ink, false, ink.recognized_text);
    }
};

}  // namespace

std::string render_html(const Page& page, PageContext& ctx, const NavInfo& nav) {
    ctx.for_html = true;
    HtmlRenderer r(page, ctx, nav);
    return r.render();
}

}  // namespace oneconv::render
