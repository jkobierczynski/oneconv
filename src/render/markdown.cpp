// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Markdown page renderer: CommonMark + GitHub extensions, or the Obsidian dialect
// (wikilinks, embeds, #tags, ==highlights==, properties).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "../util/log.hpp"
#include "../util/text.hpp"
#include "render.hpp"

namespace oneconv::render {

using namespace model;

namespace {

std::string yaml_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    return out + "\"";
}

std::string md_url(const std::string& url) {
    if (url.find_first_of(" ()<>") != std::string::npos) {
        std::string u = replace_all(replace_all(url, "<", "%3C"), ">", "%3E");
        return "<" + u + ">";
    }
    return url;
}

/// Obsidian gives meaning to a few sequences that plain Markdown leaves alone.
std::string obsidian_escape(const std::string& text) {
    std::string e = md_escape(text);
    std::string out;
    out.reserve(e.size() + 4);
    for (size_t i = 0; i < e.size(); ++i) {
        char c = e[i];
        unsigned char next = i + 1 < e.size() ? static_cast<unsigned char>(e[i + 1]) : 0;
        if (c == '#' && (std::isalnum(next) || next == '_' || next == '-' || next == '/' || next == '\\' || next >= 0x80)) {
            out += "\\#";  // would become a tag
        } else if ((c == '=' || c == '%') && next == static_cast<unsigned char>(c)) {
            out.push_back('\\');  // == highlight, %% comment
            out.push_back(c);
            out.push_back('\\');
            out.push_back(c);
            ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// Keep a paragraph line from being read as a heading, list item or rule.
std::string escape_line_start(const std::string& l) {
    if (l.empty()) return l;
    if (l[0] == '#') {
        size_t k = 0;
        while (k < l.size() && l[k] == '#') ++k;
        if (k <= 6 && (k == l.size() || l[k] == ' ')) return "\\" + l;
    }
    if ((l[0] == '-' || l[0] == '+') && (l.size() == 1 || l[1] == ' ')) return "\\" + l;
    if ((l[0] == '-' || l[0] == '=') && l.find_first_not_of(l[0]) == std::string::npos) return "\\" + l;
    size_t k = 0;
    while (k < l.size() && k < 9 && std::isdigit(static_cast<unsigned char>(l[k]))) ++k;
    if (k > 0 && k < l.size() && (l[k] == '.' || l[k] == ')') && (k + 1 == l.size() || l[k + 1] == ' '))
        return l.substr(0, k) + "\\" + l.substr(k);
    return l;
}

/// OneNote tag label -> Obsidian tag name ("Remember for later" -> "remember-for-later").
std::string tag_slug(const std::string& label) {
    std::string out;
    bool gap = false, non_digit = false;
    for (uint32_t cp : utf8_codepoints(label)) {
        bool word = cp >= 0x80 || std::isalnum(static_cast<int>(cp)) || cp == '_';
        if (!word) {
            gap = true;
            continue;
        }
        if (gap && !out.empty()) out.push_back('-');
        gap = false;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(std::tolower(static_cast<int>(cp))));
            if (!std::isdigit(static_cast<int>(cp))) non_digit = true;
        } else {
            append_utf8(out, cp);
            non_digit = true;
        }
    }
    if (!out.empty() && !non_digit) out = "tag-" + out;  // tags may not be purely numeric
    return out;
}

bool has_extension(const std::string& name, std::initializer_list<const char*> exts) {
    std::string l = to_lower(name);
    for (const char* e : exts)
        if (ends_with(l, e)) return true;
    return false;
}

// File types Obsidian can display inline (https://obsidian.md/help/file-formats)
bool obsidian_image(const std::string& n) {
    return has_extension(n, {".avif", ".bmp", ".gif", ".jpeg", ".jpg", ".png", ".svg", ".webp"});
}
bool obsidian_embeddable(const std::string& n) {
    return obsidian_image(n) || has_extension(n, {".flac", ".m4a", ".mp3", ".ogg", ".wav", ".webm", ".3gp", ".mkv",
                                                  ".mov", ".mp4", ".ogv", ".pdf"});
}

/// [[target]] or [[target|label]]. Inside tables the pipe has to be escaped.
std::string wikilink(const std::string& target, const std::string& label, bool in_table, bool embed = false) {
    std::string text;
    for (char c : label) {
        if (c == '[' || c == ']' || c == '\n' || c == '\r') continue;
        text.push_back(c == '|' ? '-' : c);
    }
    text = trim(text);
    std::string s = std::string(embed ? "![[" : "[[") + target;
    if (!text.empty() && text != target) s += std::string(in_table ? "\\|" : "|") + text;
    return s + "]]";
}

bool md_visible_equal(const TextStyle& a, const TextStyle& b) {
    return a.bold == b.bold && a.italic == b.italic && a.strike == b.strike && a.underline == b.underline &&
           a.superscript == b.superscript && a.subscript == b.subscript &&
           a.highlight.has_value() == b.highlight.has_value();
}

class MarkdownRenderer {
public:
    MarkdownRenderer(const Page& page, PageContext& ctx)
        : page_(page), ctx_(ctx), opts_(*ctx.opts), obs_(ctx.opts->flavor == MdFlavor::Obsidian) {}

    std::string render() {
        const std::string title = page_.title.empty() ? "Untitled Page" : page_.title;
        // Obsidian shows the file name as the note title, so no "# Title" line there.
        if (!obs_) {
            out_ += "# " + inline_escape_line(title) + "\n";
            last_ = Last::Para;
        }
        if (opts_.include_date && !opts_.front_matter && !page_.title_date.empty()) block("*" + esc(page_.title_date) + "*", "", false);

        for (const PageItem& item : flow_items(page_)) {
            switch (item.kind) {
                case PageItem::Kind::Outline: render_elements(item.outline->elements, ""); break;
                case PageItem::Kind::Image: block(image_md(*item.image), "", false); break;
                case PageItem::Kind::Attachment: block(attachment_md(*item.attachment), "", false); break;
                case PageItem::Kind::Ink: block(ink_md(*item.ink), "", false); break;
            }
        }
        if (!out_.empty() && out_.back() != '\n') out_ += "\n";

        // The front matter comes last because the body decides which tags the page has.
        std::string head;
        if (opts_.front_matter) {
            head += "---\n";
            if (obs_) {
                // The original title stays findable when the file name had to be changed
                if (title != ctx_.stem) head += "aliases:\n  - " + yaml_str(title) + "\n";
            } else {
                head += "title: " + yaml_str(title) + "\n";
            }
            if (!page_.created.empty()) head += "created: " + page_.created + "\n";
            if (!page_.modified.empty()) head += "modified: " + page_.modified + "\n";
            if (!page_.author.empty()) head += "author: " + yaml_str(page_.author) + "\n";
            if (!page_.id.empty()) head += "onenote-id: " + yaml_str(page_.id) + "\n";
            if (obs_ && !ctx_.parent_wiki.empty()) head += "parent: " + yaml_str("[[" + ctx_.parent_wiki + "]]") + "\n";
            if (obs_ && !page_tags_.empty()) {
                head += "tags:\n";
                for (const auto& t : page_tags_) head += "  - " + t + "\n";
            }
            head += "---\n";
            if (!obs_) head += "\n";
        } else {
            size_t first = out_.find_first_not_of('\n');
            out_.erase(0, first == std::string::npos ? out_.size() : first);
        }
        return head + out_;
    }

private:
    const Page& page_;
    PageContext& ctx_;
    const Options& opts_;
    const bool obs_;
    std::vector<std::string> page_tags_;  // Obsidian tags used on this page, in order of appearance
    std::string out_;
    enum class Last { None, Para, ListItem } last_ = Last::None;

    /// Emit a block. `text` lines after the first must already carry their indentation.
    void block(const std::string& text, const std::string& indent, bool list_item) {
        if (trim(text).empty()) return;
        if (!(list_item && last_ == Last::ListItem)) out_ += "\n";
        out_ += indent + text + "\n";
        last_ = list_item ? Last::ListItem : Last::Para;
    }

    std::string esc(const std::string& s) const { return obs_ ? obsidian_escape(s) : md_escape(s); }

    /// Escape running text, but leave bare URLs untouched: viewers turn them into links,
    /// and an escaped "\_" inside would end up in the address.
    std::string esc_text(const std::string& s) const {
        std::string out;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t a = std::min(s.find("http://", pos), s.find("https://", pos));
            if (a == std::string::npos) break;
            size_t b = a;
            while (b < s.size() && !std::isspace(static_cast<unsigned char>(s[b])) && s[b] != '<' && s[b] != '>' && s[b] != '"') ++b;
            while (b > a && std::strchr(".,;:!?)]'", s[b - 1])) --b;  // sentence punctuation is not part of the URL
            out += esc(s.substr(pos, a - pos));
            out += s.substr(a, b - a);
            pos = b;
        }
        return out + esc(s.substr(pos));
    }
    std::string inline_escape_line(const std::string& s) const { return esc(replace_all(s, "\n", " ")); }

    // ------------------------------------------------------------------ inline

    std::string format_text(const std::string& raw, const TextStyle& st) {
        // Keep surrounding whitespace outside of emphasis markers
        size_t a = 0, b = raw.size();
        while (a < b && (raw[a] == ' ' || raw[a] == '\t')) ++a;
        while (b > a && (raw[b - 1] == ' ' || raw[b - 1] == '\t')) --b;
        std::string lead = raw.substr(0, a), core = raw.substr(a, b - a), tail = raw.substr(b);
        if (core.empty()) return raw;
        std::string s = esc_text(core);
        if (opts_.md_html_tags) {
            if (st.superscript) s = "<sup>" + s + "</sup>";
            if (st.subscript) s = "<sub>" + s + "</sub>";
            if (st.underline) s = "<u>" + s + "</u>";
            if (st.highlight && !obs_) s = "<mark>" + s + "</mark>";
        }
        if (st.highlight && obs_) s = "==" + s + "==";
        if (st.strike) s = "~~" + s + "~~";
        if (st.bold && st.italic)
            s = "***" + s + "***";
        else if (st.bold)
            s = "**" + s + "**";
        else if (st.italic)
            s = "*" + s + "*";
        return lead + s + tail;
    }

    /// Render inline content. Line breaks are returned as separate lines.
    std::vector<std::string> inlines(const std::vector<Inline>& src, bool in_table = false) {
        // Merge runs that look identical in Markdown so emphasis markers do not collide.
        std::vector<Inline> ins;
        for (const auto& in : src) {
            if (!ins.empty() && in.kind == Inline::Kind::Text && ins.back().kind == Inline::Kind::Text &&
                ins.back().href == in.href && md_visible_equal(ins.back().style, in.style)) {
                ins.back().text += in.text;
                continue;
            }
            ins.push_back(in);
        }
        std::vector<std::string> lines(1);
        size_t i = 0;
        while (i < ins.size()) {
            const Inline& in = ins[i];
            if (in.kind == Inline::Kind::Break) {
                if (in_table)
                    lines.back() += "<br>";
                else
                    lines.emplace_back();
                ++i;
                continue;
            }
            if (in.kind == Inline::Kind::Text && !in.href.empty()) {
                std::string label, plain;
                size_t j = i;
                while (j < ins.size() && ins[j].kind == Inline::Kind::Text && ins[j].href == in.href) {
                    label += format_text(ins[j].text, ins[j].style);
                    plain += ins[j].text;
                    ++j;
                }
                if (obs_) {
                    if (const LinkTable::Target* t = ctx_.find_internal(in.href)) {
                        lines.back() += wikilink(t->wiki, plain, in_table);
                        i = j;
                        continue;
                    }
                }
                std::string target = ctx_.resolve_link(in.href);
                if (trim(label).empty()) label = esc(in.href);
                lines.back() += "[" + label + "](" + md_url(target) + ")";
                i = j;
                continue;
            }
            switch (in.kind) {
                case Inline::Kind::Text: lines.back() += format_text(in.text, in.style); break;
                case Inline::Kind::Math:
                    if (in.math) {
                        std::string tex = math_to_latex(*in.math);
                        if (!trim(tex).empty()) lines.back() += "$" + tex + "$";
                    } else {
                        lines.back() += esc(in.text);
                    }
                    break;
                case Inline::Kind::Ink:
                    if (in.ink) lines.back() += ink_md(*in.ink);
                    break;
                case Inline::Kind::Break: break;
            }
            ++i;
        }
        // Trailing spaces would turn into hard breaks
        for (auto& l : lines)
            while (!l.empty() && l.back() == ' ') l.pop_back();
        return lines;
    }

    std::string join_lines(const std::vector<std::string>& lines, const std::string& cont_indent) {
        std::string s;
        for (size_t i = 0; i < lines.size(); ++i) {
            // Hard line break: backslash in CommonMark; two spaces for Obsidian, whose
            // live preview does not treat the backslash form as a break.
            if (i) s += std::string(obs_ ? "  \n" : "\\\n") + cont_indent;
            s += lines[i];
        }
        return s;
    }

    std::string tag_prefix(const std::vector<NoteTag>& tags, bool skip_checkbox) {
        std::string s;
        for (const auto& t : tags) {
            if (skip_checkbox && t.is_checkbox()) continue;
            std::string sym = t.symbol();
            if (!sym.empty()) s += sym + " ";
        }
        return s;
    }

    /// Obsidian: " #important" for each tag, so OneNote tags stay searchable.
    std::string tag_suffix(const std::vector<NoteTag>& tags) {
        std::string s;
        if (!obs_) return s;
        for (const auto& t : tags) {
            if (t.is_checkbox() && t.shape == 3) continue;  // the plain "To Do" tag is just a task
            std::string slug = tag_slug(t.label);
            if (slug.empty()) continue;
            s += " #" + slug;
            if (std::find(page_tags_.begin(), page_tags_.end(), slug) == page_tags_.end()) page_tags_.push_back(slug);
        }
        return s;
    }

    // ------------------------------------------------------------------ blocks

    std::string paragraph_md(const Paragraph& p, const std::string& cont_indent, bool in_list, bool skip_checkbox) {
        if (!in_list && p.inlines.size() == 1 && p.inlines[0].kind == Inline::Kind::Math &&
            p.inlines[0].math_display && p.inlines[0].math) {
            std::string tex = math_to_latex(*p.inlines[0].math);
            if (trim(tex).empty()) return "";
            return "$$\n" + cont_indent + tex + "\n" + cont_indent + "$$";
        }
        std::vector<std::string> lines = inlines(p.inlines);
        std::string prefix = tag_prefix(p.tags, skip_checkbox);
        std::string suffix = tag_suffix(p.tags);
        int level = p.heading_level();
        if (p.style_id == "PageTitle") level = 1;
        if (level > 0) {
            std::string text;
            for (const auto& l : lines) text += (text.empty() ? "" : " ") + l;
            if (trim(text).empty()) return "";
            if (in_list) return prefix + "**" + trim(text) + "**" + suffix;
            int n = std::max(1, std::min(6, level + opts_.heading_offset));
            return std::string(static_cast<size_t>(n), '#') + " " + prefix + trim(text) + suffix;
        }
        for (auto& l : lines) l = escape_line_start(l);
        if (!lines.empty()) lines[0] = prefix + lines[0];
        std::string body = join_lines(lines, cont_indent + (p.style_id == "blockquote" ? "> " : ""));
        if (p.style_id == "blockquote") body = "> " + body;
        if (p.style_id == "cite" && !trim(body).empty() && body.find('\n') == std::string::npos) body = "*" + body + "*";
        if (!trim(body).empty()) body += suffix;
        return body;
    }

    std::string content_md(const Content& c, const std::string& cont_indent, bool in_list, bool skip_checkbox) {
        switch (c.kind) {
            case Content::Kind::Paragraph: return paragraph_md(*c.paragraph, cont_indent, in_list, skip_checkbox);
            case Content::Kind::Table: return table_md(*c.table, cont_indent);
            case Content::Kind::Image: return image_md(*c.image);
            case Content::Kind::Attachment: return attachment_md(*c.attachment);
            case Content::Kind::Ink: return ink_md(*c.ink);
        }
        return "";
    }

    static bool is_code_element(const OutlineElement& el) {
        if (el.list || el.contents.size() != 1) return false;
        const Content& c = el.contents[0];
        return c.kind == Content::Kind::Paragraph && c.paragraph->style_id == "code";
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

    void render_elements(const std::vector<OutlineElement>& els, const std::string& indent) {
        int counter = 0;
        for (size_t i = 0; i < els.size(); ++i) {
            const OutlineElement& el = els[i];
            if (is_code_element(el)) {
                std::string code;
                size_t j = i;
                while (j < els.size() && is_code_element(els[j])) {
                    const Paragraph& p = *els[j].contents[0].paragraph;
                    for (auto& line : split_lines(p.plain_text())) code += indent + line + "\n";
                    ++j;
                }
                std::string fence = code.find("```") != std::string::npos ? "~~~" : "```";
                block(fence + "\n" + code + indent + fence, indent, false);
                i = j - 1;
                counter = 0;
                continue;
            }
            if (el.list && el.list->ordered) {
                if (counter == 0 && el.list->restart) counter = *el.list->restart - 1;
                ++counter;
            } else {
                counter = 0;
            }
            render_element(el, indent, counter);
        }
    }

    static std::vector<std::string> split_lines(const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (true) {
            size_t p = s.find('\n', start);
            out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
            if (p == std::string::npos) break;
            start = p + 1;
        }
        return out;
    }

    void render_element(const OutlineElement& el, const std::string& indent, int number) {
        auto checkbox = checkbox_state(el);
        bool list = el.list.has_value() || checkbox.has_value();
        if (!list) {
            for (const auto& c : el.contents) block(content_md(c, indent, !indent.empty(), false), indent, false);
            render_elements(el.children, indent);
            return;
        }
        std::string marker;
        if (el.list && el.list->ordered)
            marker = std::to_string(std::max(1, number)) + ".";
        else
            marker = "-";
        if (checkbox) marker += *checkbox ? " [x]" : " [ ]";
        std::string cont = indent + std::string(marker.size() + 1, ' ');
        if (checkbox && !(el.list && el.list->ordered)) cont = indent + "  ";

        bool first = true;
        for (const auto& c : el.contents) {
            std::string body = content_md(c, cont, true, true);
            if (first) {
                // Block-level content (tables, display math) must start on its own line
                bool own_line = c.kind == Content::Kind::Table || starts_with(body, "$$");
                if (own_line) {
                    block(marker, indent, true);
                    block(body, cont, false);
                } else {
                    block(marker + (body.empty() ? "" : " " + body), indent, true);
                }
                first = false;
            } else {
                block(body, cont, false);
            }
        }
        if (first) block(marker, indent, true);
        render_elements(el.children, cont);
        last_ = Last::ListItem;
    }

    // ---------------------------------------------------------------- tables

    std::string cell_text(const std::vector<OutlineElement>& els, int depth = 0) {
        std::vector<std::string> parts;
        for (const auto& el : els) {
            std::string prefix;
            if (auto cb = checkbox_state(el)) prefix = *cb ? "☑ " : "☐ ";
            else if (el.list) prefix = el.list->ordered ? "1. " : "• ";
            for (const auto& c : el.contents) {
                std::string s;
                switch (c.kind) {
                    case Content::Kind::Paragraph: {
                        auto lines = inlines(c.paragraph->inlines, true);
                        s = tag_prefix(c.paragraph->tags, true);
                        for (auto& l : lines) s += l;
                        if (!trim(s).empty()) s += tag_suffix(c.paragraph->tags);
                        break;
                    }
                    case Content::Kind::Table: {
                        // Nested tables cannot be expressed; flatten them
                        for (const auto& row : c.table->rows) {
                            std::vector<std::string> cells;
                            for (const auto& cell : row) cells.push_back(cell_text(cell.elements, depth + 1));
                            std::string r;
                            for (size_t k = 0; k < cells.size(); ++k) r += (k ? " / " : "") + cells[k];
                            s += (s.empty() ? "" : "<br>") + r;
                        }
                        break;
                    }
                    case Content::Kind::Image: s = image_md(*c.image, true); break;
                    case Content::Kind::Attachment: s = attachment_md(*c.attachment); break;
                    case Content::Kind::Ink: s = ink_md(*c.ink); break;
                }
                if (!trim(s).empty()) parts.push_back(prefix + s);
                prefix.clear();
            }
            std::string children = cell_text(el.children, depth + 1);
            if (!children.empty()) parts.push_back(children);
        }
        std::string s;
        for (size_t i = 0; i < parts.size(); ++i) s += (i ? "<br>" : "") + parts[i];
        return replace_all(s, "\n", " ");
    }

    std::string table_md(const Table& t, const std::string& cont_indent) {
        size_t cols = 0;
        for (const auto& r : t.rows) cols = std::max(cols, r.size());
        if (cols == 0) return "";
        std::string s;
        for (size_t ri = 0; ri < t.rows.size(); ++ri) {
            if (ri) s += "\n" + cont_indent;
            s += "|";
            for (size_t ci = 0; ci < cols; ++ci) {
                std::string cell = ci < t.rows[ri].size() ? cell_text(t.rows[ri][ci].elements) : "";
                s += " " + cell + " |";
            }
            if (ri == 0) {
                s += "\n" + cont_indent + "|";
                for (size_t ci = 0; ci < cols; ++ci) s += " --- |";
            }
        }
        std::string tags = trim(tag_prefix(t.tags, false) + tag_suffix(t.tags));
        if (!tags.empty()) s = tags + "\n\n" + cont_indent + s;
        return s;
    }

    // ------------------------------------------------------- images and files

    std::string image_md(const Image& img, bool compact = false) {
        std::string alt = !img.alt.empty() ? img.alt : (!generic_image_name(img.filename) ? img.filename : "image");
        alt = replace_all(replace_all(esc(replace_all(alt, "\n", " ")), "\r", ""), "  ", " ");
        std::string s;
        if (img.missing || img.data.empty()) {
            s = "*[image unavailable: " + alt + "]*";
        } else {
            std::string name = !generic_image_name(img.filename) ? img.filename
                                                     : ctx_.slug + "-image-" + std::to_string(++ctx_.image_counter) +
                                                           (img.ext.empty() ? ".bin" : img.ext);
            if (!generic_image_name(img.filename) && !img.ext.empty() && !ends_with(to_lower(name), to_lower(img.ext)))
                name += img.ext;
            fs::path p = ctx_.assets->write(img.data, name);
            std::string file = path_utf8(p.filename());
            if (obs_ && img.link.empty()) {
                if (obsidian_image(file)) {
                    // ![[file|width]] keeps the size the picture had on the OneNote page
                    long w = img.width > 0 ? std::lround(img.width * 48.0f) : 0;
                    s = wikilink(file, w >= 16 && w <= 4000 ? std::to_string(w) : "", compact, true);
                } else {
                    s = "🖼️ " + wikilink(file, "", compact);  // e.g. EMF/TIFF: not displayable
                }
            } else {
                // A picture that is itself a hyperlink needs standard syntax (also valid in Obsidian)
                s = "![" + alt + "](" + md_url(ctx_.rel(p)) + ")";
                if (!img.link.empty()) s = "[" + s + "](" + md_url(ctx_.resolve_link(img.link)) + ")";
            }
        }
        s = tag_prefix(img.tags, false) + s + tag_suffix(img.tags);
        if (!compact)
            for (const auto& e : img.embeds) s += "\n\n[▶ " + esc(e) + "](" + md_url(e) + ")";
        return s;
    }

    std::string attachment_md(const Attachment& a) {
        std::string icon = a.media == Attachment::Media::Audio   ? "\U0001F3B5"
                           : a.media == Attachment::Media::Video ? "\U0001F3AC"
                                                                 : "\U0001F4CE";
        std::string s = tag_prefix(a.tags, false);
        if (a.missing || a.data.empty()) return s + icon + " " + esc(a.name) + " *(file not available)*";
        fs::path p = ctx_.assets->write(a.data, a.name);
        if (obs_) {
            std::string file = path_utf8(p.filename());
            // Audio, video and PDF play/show inline; other files become a link that opens them
            if (obsidian_embeddable(file)) return s + wikilink(file, "", false, true) + tag_suffix(a.tags);
            return s + icon + " " + wikilink(file, "", false) + tag_suffix(a.tags);
        }
        return s + "[" + icon + " " + md_escape(a.name) + "](" + md_url(ctx_.rel(p)) + ")";
    }

    std::string ink_md(const Ink& ink) {
        if (!opts_.ink || ink.empty()) return ink.recognized_text.empty() ? "" : esc(ink.recognized_text);
        std::string svg = ink_to_svg(ink, true, ink.recognized_text);
        if (svg.empty()) return "";
        fs::path p = ctx_.assets->write_text(svg, ctx_.slug + "-ink-" + std::to_string(++ctx_.ink_counter) + ".svg");
        if (obs_) return wikilink(path_utf8(p.filename()), "", false, true);
        std::string alt = ink.recognized_text.empty() ? "ink" : md_escape(ink.recognized_text);
        return "![" + alt + "](" + md_url(ctx_.rel(p)) + ")";
    }
};

}  // namespace

std::string render_markdown(const Page& page, PageContext& ctx) {
    ctx.for_html = false;
    MarkdownRenderer r(page, ctx);
    return r.render();
}

}  // namespace oneconv::render
