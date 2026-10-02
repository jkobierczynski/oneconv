// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Writes a notebook to disk: page files, assets and index pages.
#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#include "../util/log.hpp"
#include "../util/text.hpp"
#include "render.hpp"

namespace oneconv::render {

using namespace model;

namespace {

struct PlannedPage {
    const Page* page = nullptr;
    fs::path md, html;
    std::string stem;
    std::string wiki;         // Obsidian link target
    std::string parent_wiki;  // Obsidian link target of the parent page, for subpages
};

struct PlannedSection {
    const Section* section = nullptr;
    fs::path dir;
    std::string display_path;  // "Group › Section"
    std::vector<PlannedPage> pages;
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

void write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path_utf8(p));
    f << content;
}

class Exporter {
public:
    Exporter(const Notebook& nb, fs::path out, const Options& opts)
        : nb_(nb), out_(std::move(out)), opts_(opts), obs_(opts.markdown && opts.flavor == MdFlavor::Obsidian) {}

    ExportStats run() {
        UniqueNames top;
        top.take("assets");
        top.take("index");
        // Obsidian: the table of contents is a note named after the notebook
        // (a file, so it cannot clash with the section folders reserved in `top`)
        if (obs_) index_stem_ = safe_name(nb_.name);
        plan_entries(nb_.entries, out_, "", top);
        if (obs_) plan_wikilinks();

        // Link table for onenote: links
        for (const auto& ps : sections_) {
            for (const auto& pp : ps.pages) {
                if (!pp.page->id.empty()) links_.pages[pp.page->id] = {pp.md, pp.html, pp.wiki};
            }
            if (!ps.section->id.empty() && !ps.pages.empty())
                links_.sections[ps.section->id] = {ps.pages.front().md, ps.pages.front().html, ps.pages.front().wiki};
        }

        for (auto& ps : sections_) write_section(ps);
        if (obs_)
            write_file(out_ / u8path(index_stem_ + ".md"), index_obsidian());
        else if (opts_.markdown)
            write_file(out_ / "index.md", index_markdown());
        if (opts_.html) write_file(out_ / "index.html", index_html());
        return stats_;
    }

private:
    const Notebook& nb_;
    fs::path out_;
    const Options& opts_;
    std::vector<PlannedSection> sections_;
    const bool obs_;
    std::string index_stem_;
    std::set<std::string> vault_names_;  // Obsidian: every file name in the export (lower case)
    LinkTable links_;
    ExportStats stats_;

    /// File or folder name; in Obsidian mode also safe to use inside [[links]].
    std::string safe_name(const std::string& name, size_t max_len = 120) const {
        std::string s = sanitize_filename(name, max_len);
        return obs_ ? sanitize_filename(wiki_safe_name(s), max_len) : s;
    }

    /// Decide how each note is addressed. A name that is unique in the export is linked
    /// by name alone. Notes that share a name with another one are linked by their path
    /// from the export folder; Obsidian matches such a path against the end of the file
    /// path, so the links keep working when the export is put in a subfolder of a vault.
    void plan_wikilinks() {
        std::map<std::string, int> count;
        for (const auto& ps : sections_)
            for (const auto& pp : ps.pages) ++count[to_lower(pp.stem)];
        // The contents note must not share its name with a page: "Math/Math" would then
        // match the contents note "Math.md" inside the export folder "Math" as well.
        if (count.count(to_lower(index_stem_))) {
            std::string base = index_stem_ + " (contents)";
            index_stem_ = base;
            for (int i = 2; count.count(to_lower(index_stem_)); ++i) index_stem_ = base + " " + std::to_string(i);
        }
        for (auto& ps : sections_) {
            std::vector<PlannedPage*> ancestors;  // most recent page at each level
            for (auto& pp : ps.pages) {
                if (count[to_lower(pp.stem)] == 1) {
                    pp.wiki = pp.stem;
                } else {
                    std::string rel = path_utf8(pp.md.lexically_normal().lexically_relative(out_.lexically_normal()));
                    std::replace(rel.begin(), rel.end(), '\\', '/');
                    if (ends_with(rel, ".md")) rel.resize(rel.size() - 3);
                    pp.wiki = rel;
                }
                size_t level = static_cast<size_t>(std::max(0, pp.page->level));
                if (level > 0 && level <= ancestors.size() && ancestors[level - 1]) pp.parent_wiki = ancestors[level - 1]->wiki;
                ancestors.resize(level + 1);
                ancestors[level] = &pp;
                vault_names_.insert(to_lower(pp.stem) + ".md");
            }
        }
        vault_names_.insert(to_lower(index_stem_) + ".md");
    }

    void plan_entries(const std::vector<NotebookEntry>& entries, const fs::path& dir, const std::string& prefix,
                      UniqueNames& names) {
        for (const auto& e : entries) {
            if (e.group) {
                std::string name = names.take(safe_name(e.group->name));
                UniqueNames inner;
                inner.take("assets");
                plan_entries(e.group->entries, dir / u8path(name), prefix + e.group->name + " › ", inner);
            } else if (e.section) {
                PlannedSection ps;
                ps.section = e.section.get();
                ps.dir = dir / u8path(names.take(safe_name(e.section->name)));
                ps.display_path = prefix + e.section->name;
                UniqueNames page_names;
                page_names.take("assets");
                for (const auto& page : e.section->pages) {
                    PlannedPage pp;
                    pp.page = &page;
                    pp.stem = page_names.take(safe_name(page.title.empty() ? "Untitled Page" : page.title, 80));
                    pp.md = ps.dir / u8path(pp.stem + ".md");
                    pp.html = ps.dir / u8path(pp.stem + ".html");
                    ps.pages.push_back(pp);
                }
                sections_.push_back(std::move(ps));
            }
        }
    }

    void write_section(const PlannedSection& ps) {
        const Section& sec = *ps.section;
        if (!sec.error.empty()) {
            ++stats_.failed_sections;
            return;
        }
        if (sec.encrypted) {
            ++stats_.encrypted_sections;
            return;
        }
        ++stats_.sections;
        std::error_code ec;
        fs::create_directories(ps.dir, ec);
        // Obsidian embeds address files by name, so asset names are unique across the whole export
        AssetWriter assets(ps.dir / "assets", obs_ ? &vault_names_ : nullptr, obs_);
        for (size_t i = 0; i < ps.pages.size(); ++i) {
            const PlannedPage& pp = ps.pages[i];
            PageContext ctx;
            ctx.opts = &opts_;
            ctx.links = &links_;
            ctx.assets = &assets;
            ctx.page_dir = ps.dir;
            ctx.slug = sanitize_filename(pp.stem, 40);
            ctx.stem = pp.stem;
            ctx.parent_wiki = pp.parent_wiki;
            try {
                if (opts_.markdown) write_file(pp.md, render_markdown(*pp.page, ctx));
                if (opts_.html) {
                    NavInfo nav;
                    nav.notebook_name = nb_.name;
                    nav.section_path = ps.display_path;
                    nav.index = out_ / "index.html";
                    if (i > 0) {
                        nav.prev = ps.pages[i - 1].html;
                        nav.prev_title = ps.pages[i - 1].page->title;
                    }
                    if (i + 1 < ps.pages.size()) {
                        nav.next = ps.pages[i + 1].html;
                        nav.next_title = ps.pages[i + 1].page->title;
                    }
                    write_file(pp.html, render_html(*pp.page, ctx, nav));
                }
                ++stats_.pages;
            } catch (const std::exception& e) {
                Log::warn("failed to write page '" + pp.page->title + "': " + e.what());
            }
        }
        stats_.assets += static_cast<int>(assets.count());
    }

    // ----------------------------------------------------------------- indexes

    const PlannedSection* planned(const Section* s) const {
        for (const auto& ps : sections_)
            if (ps.section == s) return &ps;
        return nullptr;
    }

    std::string section_note(const Section& s) const {
        if (!s.error.empty()) return " (could not be read: " + s.error + ")";
        if (s.encrypted) return " (password protected — not exported)";
        if (s.pages.empty()) return " (empty)";
        return "";
    }

    std::string index_markdown() const {
        std::string s = "# " + md_escape(nb_.name) + "\n";
        std::function<void(const std::vector<NotebookEntry>&, int)> walk = [&](const std::vector<NotebookEntry>& es,
                                                                                int depth) {
            for (const auto& e : es) {
                std::string hashes(static_cast<size_t>(std::min(6, 2 + depth)), '#');
                if (e.group) {
                    s += "\n" + hashes + " " + md_escape(e.group->name) + "\n";
                    walk(e.group->entries, depth + 1);
                } else if (e.section) {
                    const PlannedSection* ps = planned(e.section.get());
                    s += "\n" + hashes + " " + md_escape(e.section->name) + section_note(*e.section) + "\n\n";
                    if (!ps || e.section->encrypted || !e.section->error.empty()) continue;
                    for (const auto& pp : ps->pages) {
                        std::string indent(static_cast<size_t>(std::max(0, pp.page->level)) * 2, ' ');
                        std::string title = pp.page->title.empty() ? "Untitled Page" : pp.page->title;
                        s += indent + "- [" + md_escape(title) + "](" + relative_link(out_, pp.md) + ")\n";
                    }
                }
            }
        };
        walk(nb_.entries, 0);
        return s;
    }

    /// Table of contents note for an Obsidian vault: sections as headings, pages as links.
    std::string index_obsidian() const {
        std::string s;
        std::function<void(const std::vector<NotebookEntry>&, int)> walk = [&](const std::vector<NotebookEntry>& es,
                                                                                int depth) {
            for (const auto& e : es) {
                std::string hashes(static_cast<size_t>(std::min(6, 1 + depth)), '#');
                if (e.group) {
                    s += "\n" + hashes + " " + md_escape(e.group->name) + "\n";
                    walk(e.group->entries, depth + 1);
                } else if (e.section) {
                    const PlannedSection* ps = planned(e.section.get());
                    s += "\n" + hashes + " " + md_escape(e.section->name) + section_note(*e.section) + "\n\n";
                    if (!ps || e.section->encrypted || !e.section->error.empty()) continue;
                    for (const auto& pp : ps->pages) {
                        std::string indent(static_cast<size_t>(std::max(0, pp.page->level)) * 2, ' ');
                        std::string title = pp.page->title.empty() ? "Untitled Page" : pp.page->title;
                        std::string label;
                        for (char c : title)
                            if (c != '[' && c != ']' && c != '\n') label.push_back(c == '|' ? '-' : c);
                        s += indent + "- [[" + pp.wiki + (label != pp.wiki ? "|" + label : "") + "]]\n";
                    }
                }
            }
        };
        walk(nb_.entries, 0);
        size_t first = s.find_first_not_of('\n');
        return first == std::string::npos ? s : s.substr(first);
    }

    std::string index_html() const {
        std::string body;
        std::function<void(const std::vector<NotebookEntry>&)> walk = [&](const std::vector<NotebookEntry>& es) {
            for (const auto& e : es) {
                if (e.group) {
                    body += "<div class=\"group\">\U0001F4C1 " + html_escape(e.group->name) + "</div><div class=\"indent\">";
                    walk(e.group->entries);
                    body += "</div>";
                } else if (e.section) {
                    const Section& sec = *e.section;
                    std::string style = sec.color ? " style=\"border-left-color:" + sec.color->hex() + "\"" : "";
                    body += "<div class=\"section\"" + style + ">" + html_escape(sec.name) + " <span class=\"count\">" +
                            html_escape(section_note(sec)) + "</span></div>";
                    const PlannedSection* ps = planned(&sec);
                    if (!ps || sec.encrypted || !sec.error.empty() || ps->pages.empty()) continue;
                    // Nested list following page levels
                    body += "<ul>";
                    int level = 0;
                    bool open_li = false;
                    for (const auto& pp : ps->pages) {
                        int target = std::max(0, pp.page->level);
                        while (level < target) {
                            body += "<ul>";
                            ++level;
                            open_li = false;
                        }
                        while (level > target) {
                            body += "</li></ul>";
                            --level;
                        }
                        if (open_li) body += "</li>";
                        std::string title = pp.page->title.empty() ? "Untitled Page" : pp.page->title;
                        body += "<li><a href=\"" + html_escape(relative_link(out_, pp.html)) + "\">" + html_escape(title) + "</a>";
                        open_li = true;
                    }
                    while (level > 0) {
                        body += "</li></ul>";
                        --level;
                    }
                    body += "</li></ul>";
                }
            }
        };
        walk(nb_.entries);
        return "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
               "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
               html_escape(nb_.name) + "</title>\n<style>" + html_stylesheet() + "</style>\n</head>\n<body>\n<main>\n" +
               "<header class=\"page-head\"><h1>" + html_escape(nb_.name) + "</h1></header>\n<nav class=\"toc\">" + body +
               "</nav>\n</main>\n</body>\n</html>\n";
    }
};

}  // namespace

ExportStats export_notebook(const Notebook& notebook, const fs::path& out_dir, const Options& opts) {
    Exporter e(notebook, out_dir, opts);
    return e.run();
}

}  // namespace oneconv::render
