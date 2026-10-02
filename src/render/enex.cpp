// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Evernote export (ENEX) writer. One .enex file per OneNote section; each page becomes
// a note whose body is ENML, Evernote's restricted XHTML dialect. Images, attachments
// and ink are embedded as base64 resources that the body references by MD5 hash.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

#include "../util/log.hpp"
#include "../util/md5.hpp"
#include "../util/text.hpp"
#include "render.hpp"

namespace oneconv::render {

using namespace model;

namespace {

constexpr double kPxPerHalfInch = 48.0;

/// Escape text for XML and drop code points XML 1.0 does not allow.
std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (uint32_t cp : utf8_codepoints(s)) {
        bool valid = cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
                     (cp >= 0x10000 && cp <= 0x10FFFF);
        if (!valid || (cp >= 0xFDD0 && cp <= 0xFDEF)) continue;
        switch (cp) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: append_utf8(out, cp);
        }
    }
    return out;
}

std::string mime_for(const std::string& name) {
    static const std::map<std::string, const char*> types = {
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
        {"bmp", "image/bmp"}, {"webp", "image/webp"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
        {"svg", "image/svg+xml"}, {"emf", "image/x-emf"}, {"wmf", "image/x-wmf"},
        {"pdf", "application/pdf"}, {"txt", "text/plain"}, {"csv", "text/csv"}, {"htm", "text/html"},
        {"html", "text/html"}, {"xml", "application/xml"}, {"json", "application/json"}, {"zip", "application/zip"},
        {"rtf", "application/rtf"}, {"doc", "application/msword"}, {"xls", "application/vnd.ms-excel"},
        {"ppt", "application/vnd.ms-powerpoint"}, {"msg", "application/vnd.ms-outlook"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"wav", "audio/wav"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"wma", "audio/x-ms-wma"},
        {"ogg", "audio/ogg"}, {"flac", "audio/flac"}, {"mp4", "video/mp4"}, {"mov", "video/quicktime"},
        {"wmv", "video/x-ms-wmv"}, {"avi", "video/x-msvideo"}, {"mkv", "video/x-matroska"}, {"webm", "video/webm"},
    };
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        auto it = types.find(to_lower(name.substr(dot + 1)));
        if (it != types.end()) return it->second;
    }
    return "application/octet-stream";
}

/// Image types every ENEX reader displays inline.
bool inline_image(const std::string& mime) {
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif";
}

/// "2025-10-21T11:55:31Z" -> "20251021T115531Z"
std::string enex_time(const std::string& iso) {
    std::string out;
    for (char c : iso)
        if (c != '-' && c != ':') out.push_back(c);
    return out.size() == 16 ? out : std::string();
}

std::string now_enex() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::gmtime(&t);
    char buf[80];
    std::snprintf(buf, sizeof buf, "%04d%02d%02dT%02d%02d%02dZ", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                  tm->tm_hour, tm->tm_min, tm->tm_sec);
    return buf;
}

/// ENML only accepts ordinary URL schemes; anything else is kept as plain text.
std::string safe_href(const std::string& href) {
    std::string l = to_lower(trim(href));
    static const char* allowed[] = {"http://", "https://", "mailto:", "ftp://", "tel:", "file:"};
    bool ok = false;
    for (const char* a : allowed) ok = ok || starts_with(l, a);
    if (!ok) return "";
    std::string out;
    for (char c : trim(href)) {
        if (c == ' ')
            out += "%20";
        else if (c == '\\')
            out.push_back('/');
        else if (static_cast<unsigned char>(c) >= 0x20)
            out.push_back(c);
    }
    return out;
}

struct Resource {
    Blob data;
    std::string mime;
    std::string filename;
    std::string hash;
    int width = 0, height = 0;
    bool attachment = false;
};

std::string px(double v) { return std::to_string(static_cast<long>(std::lround(v))) + "px"; }

/// Renders one page as an ENML document body.
class EnmlRenderer {
public:
    EnmlRenderer(const Page& page, const Options& opts) : page_(page), opts_(opts) {}

    std::string render() {
        std::string body;
        for (const PageItem& item : flow_items(page_)) {
            switch (item.kind) {
                case PageItem::Kind::Outline: body += elements(item.outline->elements); break;
                case PageItem::Kind::Image: body += image(*item.image); break;
                case PageItem::Kind::Attachment: body += attachment(*item.attachment); break;
                case PageItem::Kind::Ink: body += "<div>" + ink(*item.ink) + "</div>"; break;
            }
        }
        if (body.empty()) body = "<div><br/></div>";
        return "<en-note>" + body + "</en-note>";
    }

    const std::vector<Resource>& resources() const { return resources_; }
    const std::vector<std::string>& tags() const { return tags_; }

private:
    const Page& page_;
    const Options& opts_;
    std::vector<Resource> resources_;
    std::vector<std::string> tags_;
    int ink_counter_ = 0, image_counter_ = 0;

    /// Register a resource (once per distinct content) and return its hash.
    const Resource& add_resource(const Blob& data, const std::string& filename, const std::string& mime, int w, int h,
                                 bool is_attachment) {
        std::string hash = md5_hex(data.data(), data.size);
        for (const auto& r : resources_)
            if (r.hash == hash) return r;
        Resource r;
        r.data = data;
        r.mime = mime;
        r.filename = filename;
        r.hash = hash;
        r.width = w;
        r.height = h;
        r.attachment = is_attachment;
        resources_.push_back(std::move(r));
        return resources_.back();
    }

    static std::string media(const Resource& r, int w = 0, int h = 0, const std::string& alt = "") {
        std::string s = "<en-media type=\"" + r.mime + "\" hash=\"" + r.hash + "\"";
        if (w > 0) s += " width=\"" + std::to_string(w) + "\"";
        if (h > 0) s += " height=\"" + std::to_string(h) + "\"";
        if (!alt.empty()) s += " alt=\"" + xml_escape(alt) + "\"";
        return s + "/>";
    }

    // ------------------------------------------------------------------ inline

    std::string text(const Inline& in) {
        std::string s = xml_escape(in.text);
        const TextStyle& st = in.style;
        if (st.superscript) s = "<sup>" + s + "</sup>";
        if (st.subscript) s = "<sub>" + s + "</sub>";
        if (st.strike) s = "<s>" + s + "</s>";
        if (st.underline) s = "<u>" + s + "</u>";
        if (st.italic) s = "<i>" + s + "</i>";
        if (st.bold) s = "<b>" + s + "</b>";
        std::string css;
        if (st.highlight && !(*st.highlight == Color{255, 255, 255}))
            css += "background-color: " + st.highlight->hex() + ";-evernote-highlight:true;";
        if (st.color && !(*st.color == Color{0, 0, 0})) css += "color: " + st.color->hex() + ";";
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
                while (j < ins.size() && ins[j].kind == Inline::Kind::Text && ins[j].href == in.href) label += text(ins[j++]);
                // Links to other OneNote pages cannot be expressed: Evernote assigns note
                // identifiers only when the file is imported. The link text is kept.
                std::string href = safe_href(in.href);
                s += href.empty() ? label : "<a href=\"" + xml_escape(href) + "\">" + label + "</a>";
                i = j;
                continue;
            }
            switch (in.kind) {
                case Inline::Kind::Text: s += text(in); break;
                case Inline::Kind::Break: s += "<br/>"; break;
                case Inline::Kind::Math:
                    // ENML has no equation markup; LaTeX source keeps the equation lossless
                    if (in.math) {
                        std::string tex = math_to_latex(*in.math);
                        if (!trim(tex).empty()) s += xml_escape((in.math_display ? "$$" : "$") + tex + (in.math_display ? "$$" : "$"));
                    } else {
                        s += xml_escape(in.text);
                    }
                    break;
                case Inline::Kind::Ink:
                    if (in.ink) s += ink(*in.ink);
                    break;
            }
            ++i;
        }
        return s;
    }

    /// Symbols for the tags of a block; their labels become tags of the note.
    std::string tag_marks(const std::vector<NoteTag>& tags, bool checkbox_as_todo) {
        std::string s;
        for (const auto& t : tags) {
            if (t.is_checkbox()) {
                if (checkbox_as_todo) s += std::string("<en-todo checked=\"") + (t.completed ? "true" : "false") + "\"/>";
                if (t.shape == 3) continue;  // the plain "To Do" tag is just a task
            } else {
                std::string sym = t.symbol();
                if (!sym.empty()) s += xml_escape(sym) + " ";
            }
            add_tag(t.label);
        }
        return s;
    }

    void add_tag(const std::string& label) {
        // Evernote tag names: no commas, at most 100 characters, unique ignoring case
        std::string name;
        size_t count = 0;
        for (uint32_t cp : utf8_codepoints(replace_all(trim(label), ",", " "))) {
            if (cp < 0x20 || ++count > 100) continue;
            append_utf8(name, cp);
        }
        name = trim(name);
        if (name.empty()) return;
        for (const auto& t : tags_)
            if (iequals(t, name)) return;
        tags_.push_back(name);
    }

    // ------------------------------------------------------------------ blocks

    std::string paragraph(const Paragraph& p) {
        std::string body = tag_marks(p.tags, true) + inlines(p.inlines);
        if (p.empty() && p.tags.empty()) return "<div><br/></div>";
        std::string style;
        if (p.align == Align::Center) style = "text-align: center;";
        if (p.align == Align::Right) style = "text-align: right;";
        std::string attrs = style.empty() ? "" : " style=\"" + style + "\"";
        if (p.rtl) attrs += " dir=\"rtl\"";
        int level = p.heading_level();
        if (p.style_id == "PageTitle") level = 1;
        if (level > 0) {
            std::string tag = "h" + std::to_string(std::max(1, std::min(6, level + opts_.heading_offset)));
            return "<" + tag + attrs + ">" + body + "</" + tag + ">";
        }
        if (p.style_id == "blockquote") return "<blockquote" + attrs + "><div>" + body + "</div></blockquote>";
        if (p.style_id == "cite") return "<div style=\"" + style + "font-style: italic; color: #666666;\">" + body + "</div>";
        return "<div" + attrs + ">" + body + "</div>";
    }

    std::string content(const Content& c) {
        switch (c.kind) {
            case Content::Kind::Paragraph: return paragraph(*c.paragraph);
            case Content::Kind::Table: return table(*c.table);
            case Content::Kind::Image: return image(*c.image);
            case Content::Kind::Attachment: return attachment(*c.attachment);
            case Content::Kind::Ink: return "<div>" + ink(*c.ink) + "</div>";
        }
        return "";
    }

    static bool is_task(const OutlineElement& el) {
        for (const auto& c : el.contents) {
            if (c.kind != Content::Kind::Paragraph) continue;
            for (const auto& t : c.paragraph->tags)
                if (t.is_checkbox()) return true;
            break;
        }
        return false;
    }

    static bool is_code(const OutlineElement& el) {
        return !el.list && el.contents.size() == 1 && el.contents[0].kind == Content::Kind::Paragraph &&
               el.contents[0].paragraph->style_id == "code";
    }

    enum class ListKind { None, Bullet, Ordered };

    static ListKind kind_of(const OutlineElement& el) {
        if (!el.list || is_task(el)) return ListKind::None;
        return el.list->ordered ? ListKind::Ordered : ListKind::Bullet;
    }

    std::string code_block(const std::vector<OutlineElement>& els, size_t& i) {
        // The style Evernote itself writes for code blocks; other importers recognise it too
        std::string s =
            "<div style=\"box-sizing: border-box; padding: 8px; font-family: Monaco, Menlo, Consolas, &quot;Courier New&quot;, "
            "monospace; font-size: 12px; color: rgb(51, 51, 51); border-radius: 4px; background-color: rgb(251, 250, 248); "
            "border: 1px solid rgba(0, 0, 0, 0.15);-en-codeblock:true;\">";
        for (; i < els.size() && is_code(els[i]); ++i) {
            std::string plain = els[i].contents[0].paragraph->plain_text();
            size_t start = 0;
            while (true) {
                size_t nl = plain.find('\n', start);
                std::string line = plain.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                // keep indentation: leading spaces would collapse
                size_t lead = 0;
                while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
                std::string indent;
                for (size_t k = 0; k < lead; ++k) indent += line[k] == '\t' ? "&#160;&#160;&#160;&#160;" : "&#160;";
                std::string rest = xml_escape(line.substr(lead));
                s += "<div>" + (indent.empty() && rest.empty() ? std::string("<br/>") : indent + rest) + "</div>";
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
        return s + "</div>";
    }

    std::string elements(const std::vector<OutlineElement>& els, int depth = 0) {
        std::string s;
        if (depth > 64) return s;
        size_t i = 0;
        while (i < els.size()) {
            const OutlineElement& el = els[i];
            if (is_code(el)) {
                s += code_block(els, i);
                continue;
            }
            ListKind k = kind_of(el);
            if (k == ListKind::None) {
                // Plain paragraphs and tasks. A task is a line that starts with <en-todo/>.
                for (const auto& c : el.contents) s += content(c);
                if (!el.children.empty())
                    s += "<div style=\"padding-left: 40px;\">" + elements(el.children, depth + 1) + "</div>";
                ++i;
                continue;
            }
            if (k == ListKind::Ordered) {
                static const char* types[] = {"1", "I", "i", "A", "a"};
                s += "<ol";
                int ns = el.list->number_style;
                if (ns > 0 && ns <= 4) s += std::string(" type=\"") + types[ns] + "\"";
                if (el.list->restart && *el.list->restart > 1) s += " start=\"" + std::to_string(*el.list->restart) + "\"";
                s += ">";
            } else {
                s += "<ul>";
            }
            for (; i < els.size() && kind_of(els[i]) == k && !is_code(els[i]); ++i) {
                s += "<li>";
                for (const auto& c : els[i].contents) s += content(c);
                if (els[i].contents.empty()) s += "<div><br/></div>";
                if (!els[i].children.empty()) s += elements(els[i].children, depth + 1);
                s += "</li>";
            }
            s += k == ListKind::Ordered ? "</ol>" : "</ul>";
        }
        return s;
    }

    // ---------------------------------------------------------------- tables

    std::string table(const Table& t) {
        std::string s = tag_marks(t.tags, false);
        if (!s.empty()) s = "<div>" + s + "</div>";
        s += "<table style=\"border-collapse: collapse;\">";
        if (!t.col_widths.empty()) {
            s += "<colgroup>";
            for (float w : t.col_widths) s += "<col style=\"width: " + px(w * kPxPerHalfInch) + ";\"/>";
            s += "</colgroup>";
        }
        s += "<tbody>";
        const std::string border = t.borders ? "border: 1px solid #c0c0c0;" : "border: none;";
        for (const auto& row : t.rows) {
            s += "<tr>";
            for (const auto& cell : row) {
                std::string style = border + " padding: 4px 8px; vertical-align: top;";
                if (cell.background) style += " background-color: " + cell.background->hex() + ";";
                std::string inner = elements(cell.elements);
                s += "<td style=\"" + style + "\">" + (inner.empty() ? std::string("<div><br/></div>") : inner) + "</td>";
            }
            s += "</tr>";
        }
        return s + "</tbody></table>";
    }

    // ------------------------------------------------------- images and files

    std::string image(const Image& img) {
        std::string alt = !img.alt.empty() ? img.alt : (!generic_image_name(img.filename) ? img.filename : "");
        std::string s = tag_marks(img.tags, false);
        if (img.missing || img.data.empty()) {
            s += "<i>[image unavailable" + (alt.empty() ? "" : ": " + xml_escape(alt)) + "]</i>";
        } else {
            std::string name = !generic_image_name(img.filename)
                                   ? img.filename
                                   : "image-" + std::to_string(++image_counter_) + (img.ext.empty() ? ".bin" : img.ext);
            if (!generic_image_name(img.filename) && !img.ext.empty() && !ends_with(to_lower(name), to_lower(img.ext)))
                name += img.ext;
            std::string mime = mime_for(name);
            bool shown = inline_image(mime);
            int w = shown && img.width > 0 ? static_cast<int>(std::lround(img.width * kPxPerHalfInch)) : 0;
            int h = shown && img.height > 0 ? static_cast<int>(std::lround(img.height * kPxPerHalfInch)) : 0;
            const Resource& r = add_resource(img.data, name, mime, w, h, !shown);
            std::string tag = media(r, w, h, alt.size() > 300 ? "" : replace_all(alt, "\n", " "));
            std::string href = safe_href(img.link);
            s += href.empty() ? tag : "<a href=\"" + xml_escape(href) + "\">" + tag + "</a>";
        }
        s = "<div>" + s + "</div>";
        for (const auto& e : img.embeds) {
            std::string href = safe_href(e);
            if (!href.empty()) s += "<div><a href=\"" + xml_escape(href) + "\">" + xml_escape(e) + "</a></div>";
        }
        return s;
    }

    std::string attachment(const Attachment& a) {
        std::string s = "<div>" + tag_marks(a.tags, false);
        if (a.missing || a.data.empty()) return s + "<i>" + xml_escape(a.name) + " (file not available)</i></div>";
        const Resource& r = add_resource(a.data, a.name, mime_for(a.name), 0, 0, true);
        return s + media(r) + "</div>";
    }

    std::string ink(const Ink& drawing) {
        if (!opts_.ink || drawing.empty()) return xml_escape(drawing.recognized_text);
        PngImage png = ink_to_png(drawing);
        if (png.data.empty()) return "";
        int w = png.width, h = png.height;
        const Resource& r = add_resource(Blob::from_vector(std::move(png.data)), "ink-" + std::to_string(++ink_counter_) + ".png",
                                         "image/png", w, h, false);
        return media(r, w, h, drawing.recognized_text.size() > 300 ? "" : drawing.recognized_text);
    }
};

class UniqueNames {
public:
    std::string take(const std::string& wanted) {
        std::string candidate = wanted;
        for (int i = 2; used_.count(to_lower(candidate)); ++i) candidate = wanted + " (" + std::to_string(i) + ")";
        used_.insert(to_lower(candidate));
        return candidate;
    }

private:
    std::set<std::string> used_;
};

class EnexExporter {
public:
    EnexExporter(const Notebook& nb, fs::path out, const Options& opts) : nb_(nb), out_(std::move(out)), opts_(opts) {}

    ExportStats run() {
        UniqueNames names;
        walk(nb_.entries, out_, names);
        return stats_;
    }

private:
    const Notebook& nb_;
    fs::path out_;
    const Options& opts_;
    ExportStats stats_;

    void walk(const std::vector<NotebookEntry>& entries, const fs::path& dir, UniqueNames& names) {
        for (const auto& e : entries) {
            if (e.group) {
                UniqueNames inner;
                walk(e.group->entries, dir / u8path(names.take(sanitize_filename(e.group->name))), inner);
            } else if (e.section) {
                const Section& sec = *e.section;
                if (!sec.error.empty()) {
                    ++stats_.failed_sections;
                    continue;
                }
                if (sec.encrypted) {
                    ++stats_.encrypted_sections;
                    continue;
                }
                fs::path file = dir / u8path(names.take(sanitize_filename(sec.name)) + ".enex");
                try {
                    write_section(sec, file);
                    ++stats_.sections;
                } catch (const std::exception& ex) {
                    Log::warn("failed to write " + path_utf8(file) + ": " + ex.what());
                    ++stats_.failed_sections;
                }
            }
        }
    }

    void write_section(const Section& sec, const fs::path& file) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary);
        if (!f) throw std::runtime_error("cannot create file");
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE en-export SYSTEM \"http://xml.evernote.com/pub/evernote-export4.dtd\">\n"
             "<en-export export-date=\""
          << now_enex() << "\" application=\"oneconv\" version=\"" ONECONV_VERSION "\">\n";
        for (const Page& page : sec.pages) {
            try {
                write_note(f, page);
                ++stats_.pages;
            } catch (const std::exception& ex) {
                Log::warn("failed to convert page '" + page.title + "': " + ex.what());
            }
        }
        f << "</en-export>\n";
        if (!f) throw std::runtime_error("write error");
    }

    void write_note(std::ostream& f, const Page& page) {
        // Render fully before writing anything, so a failing page leaves no partial note
        EnmlRenderer renderer(page, opts_);
        std::string enml = renderer.render();

        // Title: one line, at most 255 characters, never empty
        std::string title;
        size_t count = 0;
        for (uint32_t cp : utf8_codepoints(replace_all(replace_all(page.title, "\n", " "), "\r", " "))) {
            if (cp < 0x20) cp = ' ';
            if (++count > 255) break;
            append_utf8(title, cp);
        }
        title = trim(title);
        if (title.empty()) title = "Untitled Page";

        f << "<note>\n<title>" << xml_escape(title) << "</title>\n";
        f << "<content><![CDATA[<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
             "<!DOCTYPE en-note SYSTEM \"http://xml.evernote.com/pub/enml2.dtd\">\n"
          << replace_all(enml, "]]>", "]]]]><![CDATA[>") << "]]></content>\n";
        std::string created = enex_time(page.created), updated = enex_time(page.modified);
        if (!created.empty()) f << "<created>" << created << "</created>\n";
        if (!updated.empty()) f << "<updated>" << updated << "</updated>\n";
        for (const auto& tag : renderer.tags()) f << "<tag>" << xml_escape(tag) << "</tag>\n";
        if (!page.author.empty()) f << "<note-attributes><author>" << xml_escape(page.author) << "</author></note-attributes>\n";

        size_t total = enml.size();
        for (const Resource& r : renderer.resources()) {
            f << "<resource>\n<data encoding=\"base64\">\n";
            write_base64(f, r.data.data(), r.data.size);
            f << "</data>\n<mime>" << r.mime << "</mime>\n";
            if (r.width > 0) f << "<width>" << r.width << "</width>\n";
            if (r.height > 0) f << "<height>" << r.height << "</height>\n";
            f << "<resource-attributes><file-name>" << xml_escape(sanitize_filename(r.filename)) << "</file-name>";
            if (r.attachment) f << "<attachment>true</attachment>";
            f << "</resource-attributes>\n</resource>\n";
            total += r.data.size;
            ++stats_.assets;
        }
        f << "</note>\n";
        if (total > 200u * 1024 * 1024)
            Log::warn("note '" + title + "' is larger than 200 MB, the most Evernote accepts for one note");
    }
};

}  // namespace

ExportStats export_enex(const Notebook& notebook, const fs::path& out_dir, const Options& opts) {
    EnexExporter e(notebook, out_dir, opts);
    return e.run();
}

}  // namespace oneconv::render
