// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// oneconv — convert Microsoft OneNote files to Markdown and HTML.
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "../onestore/store.hpp"
#include "../render/render.hpp"
#include "../util/log.hpp"
#include "../util/text.hpp"
#include "loader.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef ONECONV_VERSION
#define ONECONV_VERSION "1.0.0"
#endif

namespace fs = std::filesystem;
using namespace oneconv;

namespace {

void usage(std::ostream& os) {
    os << "oneconv " ONECONV_VERSION " - convert Microsoft OneNote files to Markdown and HTML\n"
          "\n"
          "Usage: oneconv [options] <input>...\n"
          "\n"
          "Inputs:\n"
          "  section.one           a single section (OneNote 2010+ desktop or OneDrive download)\n"
          "  notebook folder       a folder with .one files, section groups and an optional .onetoc2\n"
          "  Notebook.onetoc2      the table of contents of a notebook folder\n"
          "  Notebook.onepkg       a notebook exported from OneNote (\"Export > Notebook\")\n"
          "\n"
          "Options:\n"
          "  -o, --output DIR        output directory (default: ./<input>-export)\n"
          "  -f, --format FORMAT     md, html or both (default: md)\n"
          "      --html-layout MODE  flow (default) or canvas (absolute positions like OneNote)\n"
          "      --heading-offset N  shift OneNote headings down N levels (default: 1, so\n"
          "                          Heading 1 becomes ## below the page title)\n"
          "      --no-front-matter   do not write YAML front matter in Markdown pages\n"
          "      --no-html-in-md     pure Markdown: drop <u>, <sup>, <sub> and <mark>\n"
          "      --no-ink            do not export ink drawings / handwriting\n"
          "      --no-date           do not show the page date line\n"
          "      --include-recycle-bin  also export OneNote_RecycleBin contents\n"
          "      --list              print the notebook structure and exit\n"
          "      --dump              print the raw object tree of a .one/.onetoc2 file (debugging)\n"
          "  -v, --verbose           more output (repeat for debug output)\n"
          "  -q, --quiet             only print errors\n"
          "  -h, --help              show this help\n"
          "      --version           show the version\n";
}

void print_tree(const model::Notebook& nb) {
    std::cout << nb.name << "\n";
    std::function<void(const std::vector<model::NotebookEntry>&, int)> walk = [&](const auto& entries, int depth) {
        std::string pad(static_cast<size_t>(depth) * 2, ' ');
        for (const auto& e : entries) {
            if (e.group) {
                std::cout << pad << "+ " << e.group->name << "/\n";
                walk(e.group->entries, depth + 1);
            } else if (e.section) {
                const auto& s = *e.section;
                std::cout << pad << "# " << s.name;
                if (!s.error.empty()) std::cout << "  [error: " << s.error << "]";
                if (s.encrypted) std::cout << "  [password protected]";
                std::cout << "  (" << s.pages.size() << " pages)\n";
                for (const auto& p : s.pages)
                    std::cout << pad << "  " << std::string(static_cast<size_t>(p.level) * 2, ' ') << "- "
                              << (p.title.empty() ? "Untitled Page" : p.title) << "\n";
            }
        }
    };
    walk(nb.entries, 1);
}

int dump(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        Log::error("cannot read " + path_utf8(p));
        return 1;
    }
    auto buf = std::make_shared<Buffer>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Store s = load_store(buf);
    std::cout << dump_store(s);
    return 0;
}

int run(const std::vector<std::string>& args) {
    render::Options opts;
    LoadOptions load_opts;
    std::vector<std::string> inputs;
    std::string output;
    bool list = false, do_dump = false;
    int verbosity = 1;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error(std::string("missing value for ") + name);
            return args[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(std::cout);
            return 0;
        } else if (a == "--version") {
            std::cout << "oneconv " ONECONV_VERSION "\n"
                         "Copyright (C) 2026 Jurgen Kobierczynski\n"
                         "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
                         "This is free software: you are free to change and redistribute it.\n"
                         "There is NO WARRANTY, to the extent permitted by law.\n";
            return 0;
        } else if (a == "-o" || a == "--output") {
            output = value("--output");
        } else if (a == "-f" || a == "--format") {
            std::string f = to_lower(value("--format"));
            if (f == "md" || f == "markdown") {
                opts.markdown = true;
                opts.html = false;
            } else if (f == "html") {
                opts.markdown = false;
                opts.html = true;
            } else if (f == "both" || f == "all") {
                opts.markdown = opts.html = true;
            } else {
                throw std::runtime_error("unknown format '" + f + "' (use md, html or both)");
            }
        } else if (a == "--html-layout") {
            std::string m = to_lower(value("--html-layout"));
            if (m != "flow" && m != "canvas") throw std::runtime_error("--html-layout must be flow or canvas");
            opts.html_canvas = m == "canvas";
        } else if (a == "--heading-offset") {
            opts.heading_offset = std::stoi(value("--heading-offset"));
            if (opts.heading_offset < 0 || opts.heading_offset > 5) throw std::runtime_error("--heading-offset must be 0..5");
        } else if (a == "--no-front-matter") {
            opts.front_matter = false;
        } else if (a == "--no-html-in-md") {
            opts.md_html_tags = false;
        } else if (a == "--no-ink") {
            opts.ink = false;
        } else if (a == "--no-date") {
            opts.include_date = false;
        } else if (a == "--include-recycle-bin") {
            load_opts.include_recycle_bin = true;
        } else if (a == "--list") {
            list = true;
        } else if (a == "--dump") {
            do_dump = true;
        } else if (a == "-v" || a == "--verbose") {
            ++verbosity;
        } else if (a == "-vv") {
            verbosity += 2;
        } else if (a == "-q" || a == "--quiet") {
            verbosity = 0;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            throw std::runtime_error("unknown option " + a + " (see --help)");
        } else {
            inputs.push_back(a);
        }
    }
    Log::level() = static_cast<LogLevel>(std::min(verbosity, 3));

    if (inputs.empty()) {
        usage(std::cerr);
        return 2;
    }
    if (do_dump) {
        for (const auto& in : inputs) dump(u8path(in));
        return 0;
    }

    int failures = 0;
    for (const auto& in : inputs) {
        fs::path input = u8path(in);
        try {
            model::Notebook nb = load_input(input, load_opts);
            if (list) {
                print_tree(nb);
                continue;
            }
            fs::path out;
            if (!output.empty()) {
                out = u8path(output);
                if (inputs.size() > 1) out /= u8path(sanitize_filename(nb.name));
            } else {
                out = u8path(sanitize_filename(nb.name) + "-export");
            }
            render::ExportStats st = render::export_notebook(nb, out, opts);
            std::string summary = "✓ " + nb.name + ": " + std::to_string(st.pages) + " pages from " +
                                  std::to_string(st.sections) + " sections, " + std::to_string(st.assets) +
                                  " files -> " + path_utf8(out);
            Log::info(summary);
            if (st.encrypted_sections) Log::info("  " + std::to_string(st.encrypted_sections) + " password-protected section(s) skipped");
            if (st.failed_sections) {
                Log::info("  " + std::to_string(st.failed_sections) + " section(s) could not be read");
                ++failures;
            }
        } catch (const std::exception& e) {
            Log::error(in + ": " + e.what());
            ++failures;
        }
    }
    int warnings = Log::warning_count();
    if (warnings > 0 && Log::level() >= LogLevel::Normal && !list)
        std::cerr << warnings << " warning(s); run with -v for details\n";
    return failures ? 1 : 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(s);
    }
    try {
        return run(args);
    } catch (const std::exception& e) {
        Log::error(e.what());
        return 2;
    }
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        return run(args);
    } catch (const std::exception& e) {
        Log::error(e.what());
        return 2;
    }
}
#endif
