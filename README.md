# oneconv

Convert Microsoft OneNote notebooks to **Markdown**, **HTML** or an **Obsidian** vault.

`oneconv` is a self-contained C++17 command-line tool. It reads OneNote's native
binary format directly, so it needs neither OneNote, Windows, nor the Microsoft Graph API.
It has no third-party dependencies and builds the same way on Windows, macOS and Linux.

```
oneconv -f both -o export "My Notebook.onepkg"
```

## What it reads

| Input | Notes |
|---|---|
| `Section.one` | OneNote 2010–2016/2019/2021/365 desktop format (MS-ONESTORE revision store) |
| `Section.one` from OneDrive/SharePoint | The alternative "packaging" format (MS-ONESTORE 2.8 / MS-FSSHTTPB), also produced by OneNote for Windows 10 |
| Notebook folder | Sections, nested section groups, `.onetoc2` ordering and section colours |
| `Notebook.onetoc2` | Converts the notebook folder it belongs to |
| `Notebook.onepkg` | OneNote's "Export → Notebook" package (a CAB archive, LZX or MSZIP compressed) |

OneNote 2007 files (an older format) are not supported, and neither are password-protected
sections. Encrypted sections are detected, reported and skipped.

## What it converts

| OneNote content | Markdown | HTML |
|---|---|---|
| Page title, date, author | `# Title` + YAML front matter | header with metadata |
| Subpages | nested entries in `index.md` | nested list in `index.html` |
| Headings 1–6, Quote, Citation, Code | `##`…, `>`, *italic*, fenced code blocks | `<h2>`…, `<blockquote>`, `<pre>` |
| Bold, italic, underline, strike, super/subscript, highlight | `**`, `*`, `~~`, `<u>`, `<sup>`, `<sub>`, `<mark>` | same + font colour, face and size |
| Bulleted and numbered lists (nested, restart values, Roman/alpha) | `-` / `1.` | `<ul>` / `<ol type>` |
| To-do tags (checkboxes) | GFM task lists `- [x]` | disabled checkboxes |
| Other tags (Important, Question, Idea, …) | emoji symbol | symbol with tag label tooltip |
| Tables (incl. nested content, cell shading) | GFM tables | `<table>` with column widths and shading |
| Images (with alt text, OCR text, hyperlinks) | `![alt](assets/…)` | `<img>` at original size |
| Attached files, audio and video recordings | links to extracted files | download links, `<audio>`/`<video>` players |
| Ink drawings and handwriting | SVG files | inline SVG |
| Equations | LaTeX `$…$` / `$$…$$` | MathML |
| Hyperlinks | links | links |
| Links between OneNote pages | rewritten to relative links to the exported page | same |
| Free-form page layout | reading order (top-to-bottom, left-to-right) | flowing layout, or `--html-layout canvas` for OneNote's absolute positions |

## Usage

```
oneconv [options] <input>...

  -o, --output DIR        output directory (default: ./<input>-export)
  -f, --format FORMAT     md, html, both or obsidian (default: md)
      --html-layout MODE  flow (default) or canvas (absolute positions like OneNote)
      --heading-offset N  shift OneNote headings down N levels (default 1:
                          Heading 1 becomes ## below the page title; 0 for obsidian)
      --no-front-matter   no YAML front matter in Markdown pages
      --no-html-in-md     pure Markdown: drop <u>, <sup>, <sub> and <mark>
      --no-ink            skip ink drawings / handwriting
      --no-date           omit the page date line
      --include-recycle-bin  also export OneNote_RecycleBin contents
      --list              print the notebook structure and exit
      --dump              print the raw object tree of a file (debugging)
  -v, --verbose / -q, --quiet
```

Examples:

```sh
# A whole notebook folder (e.g. synced OneDrive folder or OneNote 2016 local notebook)
oneconv -f both -o ~/export "~/Documents/OneNote Notebooks/Work"

# A single section, plain Markdown for Joplin, a static site, Git, ...
oneconv -o notes "Meeting Notes.one"

# Straight into an Obsidian vault
oneconv -f obsidian -o ~/Vault/OneNote Work.onepkg

# What's inside a package?
oneconv --list Project.onepkg
```

### Output layout

```
export/
├── index.md / index.html            notebook table of contents
├── Section A/
│   ├── Page title.md / .html
│   ├── Another page.md / .html
│   └── assets/                      images, attachments, ink SVGs
└── Section Group/
    └── Section B/
        └── …
```

File names are sanitised so they are valid on Windows, macOS and Linux; duplicate page titles
get ` (2)`, ` (3)` suffixes. Markdown pages carry YAML front matter (`title`, `created`,
`modified`, `author`, `onenote-id`), which Obsidian, Hugo, Jekyll and most other tools read.

### Obsidian

`-f obsidian` writes notes in Obsidian's own dialect. Point `-o` at a folder inside your vault
(or open the output folder as a vault).

| OneNote | In the vault |
|---|---|
| Notebook → section group → section → page | folders, one note per page; the note is named after the page title |
| Links between pages | `[[Page]]`, or `[[Section/Page\|Page]]` when two pages share a name |
| Subpages | `parent: "[[Parent page]]"` property, and nested in the contents note |
| Notebook structure | a contents note named after the notebook (`Work.md`) linking every page |
| Images | `![[picture.png\|320]]` embeds at the size they had in OneNote |
| Audio / video recordings, PDFs | `![[recording.wav]]` embeds, played or shown inline |
| Other attachments | `[[report.docx]]` links |
| Ink and handwriting | `![[page-ink-1.svg]]` |
| To-do tags | tasks: `- [ ]` / `- [x]` |
| Other tags (Important, Question, …) | their symbol plus an Obsidian tag: `⭐ Call the supplier #important`; all tags of a page are also listed in its `tags` property |
| Highlighted text | `==highlight==` |
| Equations | `$…$` and `$$…$$` (MathJax) |
| Title, dates, author | properties `aliases` (if the file name had to differ from the title), `created`, `modified`, `author`, `onenote-id` |

Details:

* There is no `# Title` line because Obsidian shows the file name as the title, so OneNote's
  *Heading 1* becomes `#` (change with `--heading-offset`).
* Characters that break Obsidian links (`# ^ [ ] |`) are replaced in file names; the original
  title is kept as an alias, so search and `[[` completion still find it.
* Attachment and image file names are unique across the whole export, so embeds never
  point at the wrong file.
* Text that would accidentally become Obsidian syntax (`#word`, `==`, `%%`) is escaped.
* Links into notebooks that are not part of the export stay `onenote:` links.

### Getting your notebooks as files

* **OneNote 2016 / 2019 / 2021 desktop:** local notebooks are folders of `.one` files; point
  `oneconv` at the folder. For cloud notebooks use *File → Export → Notebook → OneNote Package*.
* **OneNote for Windows 10/11 or the web:** download the notebook folder from OneDrive
  (select the notebook in OneDrive → *Download*), unzip, and convert the folder.

## Download

Prebuilt binaries are attached to each [GitHub Release](../../releases):

| Archive | Platform |
|---|---|
| `oneconv-<version>-windows-x64.zip` | Windows 10/11, x64 (no runtime DLLs needed) |
| `oneconv-<version>-windows-arm64.zip` | Windows 11 on ARM |
| `oneconv-<version>-linux-x64.tar.gz` | Any Linux distribution, x86-64 (fully static) |
| `oneconv-<version>-linux-arm64.tar.gz` | Any Linux distribution, ARM64, e.g. Raspberry Pi 4/5 (fully static) |
| `oneconv-<version>-macos-universal.tar.gz` | macOS 10.15+, Apple Silicon and Intel |

`SHA256SUMS.txt` lists the checksums. The macOS binary is not notarised: on first run use
right-click → Open, or `xattr -d com.apple.quarantine oneconv`.

## Building

Requirements: CMake ≥ 3.16 and a C++17 compiler (GCC ≥ 8, Clang ≥ 7, MSVC 2019+, Apple Clang 11+).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release          # unit tests
```

The binary is `build/oneconv` (`build\Release\oneconv.exe` with Visual Studio).

Windows notes: build from a *Developer PowerShell for VS* with the commands above, or with
MinGW-w64. The program uses a Unicode `wmain` entry point, so non-ASCII paths work.
Cross-compiling from Linux: `cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=<mingw toolchain>`.

CMake options: `-DONECONV_STATIC=ON` links the runtime statically (what the release
binaries use), `-DONECONV_VERSION_STRING=x.y.z` sets the version shown by `--version`.

### Continuous integration and releases

* `.github/workflows/build.yml` builds and tests every push and pull request on
  Ubuntu, macOS and Windows.
* `.github/workflows/release.yml` runs when a tag starting with `v` is pushed. It builds
  and tests the five release targets, packages them, and publishes a GitHub Release with
  generated release notes and checksums:

  ```sh
  git tag v1.0.0
  git push origin v1.0.0
  ```

  Tags with a suffix (`v1.1.0-rc1`) are published as pre-releases. Running the workflow
  manually from the Actions tab builds the archives as workflow artifacts without
  publishing anything.

## Testing

* `oneconv_tests` — unit tests: GUID/ExGUID encodings, UTF-16 decoding, property set
  reference bookkeeping, equation → LaTeX/MathML, DEFLATE and LZX decoding (fixtures
  cross-checked against `cabextract`), Markdown/HTML/Obsidian rendering and link rewriting.
* `tests/run_samples.sh` — fetches the public OneNote sample corpora of the
  [onenote.rs](https://github.com/msiemens/onenote.rs) and
  [Apache Tika](https://github.com/apache/tika) projects and converts every file.
* `tests/make_lzx_cab.py` — a small LZX cabinet writer used to produce test `.onepkg`
  files (verbatim, aligned-offset and uncompressed blocks).

During development the converter was run over 68 sample files in both on-disk formats
(page counts match the onenote.rs reference snapshots) and over ~860 randomly corrupted
OneNote files and cabinets under AddressSanitizer/UBSan without crashes or hangs.
The Obsidian output was checked in Obsidian 1.13 itself: a test vault of 69 exported notes
was opened and its link index queried, and all 159 links and embeds resolved to the
intended files, including same-named pages and exports placed in a subfolder of the vault.

## Source layout

```
src/util/       byte reader, GUIDs, UTF-8/16 helpers, logging
src/onestore/   MS-ONESTORE: desktop revision store, FSSHTTPB packaging, property sets
src/one/        MS-ONE object model → document model (pages, outlines, rich text, ink, math)
src/render/     Markdown and HTML renderers, asset writer, notebook exporter
src/cab/        Cabinet reader with LZX and DEFLATE (MSZIP) decoders for .onepkg
src/app/        input discovery and command line
```

## Limitations

* Password-protected sections cannot be decrypted.
* OneNote 2007 (`.one` with the older format) is not supported.
* File printouts are exported as their page images plus the attached file; live embedded
  Excel/Visio content is exported as the attachment, not as a rendered table.
* Markdown has no free-form layout: content positioned side by side on a page is
  emitted in reading order. Use `-f html --html-layout canvas` to keep the layout.
* Ink is exported as vector strokes; pressure-sensitive stroke width is not reproduced.

## Acknowledgements

Format knowledge comes from Microsoft's open specifications
[[MS-ONESTORE]](https://learn.microsoft.com/en-us/openspecs/office_file_formats/ms-onestore/),
[[MS-ONE]](https://learn.microsoft.com/en-us/openspecs/office_file_formats/ms-one/),
[[MS-FSSHTTPB]](https://learn.microsoft.com/en-us/openspecs/sharepoint_protocols/ms-fsshttpb/) and
[[MS-CAB]](https://learn.microsoft.com/en-us/previous-versions/bb417343(v=msdn.10)).
The undocumented parts (ink paths, equations, some property IDs) were cross-referenced with the
[onenote.rs](https://github.com/msiemens/onenote.rs) / [one2html](https://github.com/msiemens/one2html)
projects by Markus Siemens; this is an independent C++ implementation.

OneNote is a trademark of Microsoft Corporation. This project is not affiliated with Microsoft.

## License

Copyright (C) 2026 Jurgen Kobierczynski

This program is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the [GNU General Public License](LICENSE) for more details.
