// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Self-contained unit tests (no framework dependency). Run: oneconv_tests
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cab/cab.hpp"
#include "one/math.hpp"
#include "one/model.hpp"
#include "onestore/store.hpp"
#include "render/render.hpp"
#include "util/guid.hpp"
#include "util/text.hpp"

#include "fixtures.inc"

using namespace oneconv;

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            ++g_passed;                                                                   \
        } else {                                                                          \
            ++g_failed;                                                                   \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond << "\n"; \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                        \
    do {                                                                                                      \
        auto va = (a);                                                                                        \
        auto vb = (b);                                                                                        \
        if (va == vb) {                                                                                       \
            ++g_passed;                                                                                       \
        } else {                                                                                              \
            ++g_failed;                                                                                       \
            std::cerr << __FILE__ << ":" << __LINE__ << ": " #a " == " #b "\n    got:      " << va         \
                      << "\n    expected: " << vb << "\n";                                                    \
        }                                                                                                     \
    } while (0)

static void test_guid() {
    Guid g = Guid::from_string("{7B5C52E4-D88C-4DA7-AEB1-5378D02996D3}");
    CHECK_EQ(g.b[0], 0xE4);  // Data1 is little-endian on disk
    CHECK_EQ(g.str(), std::string("7b5c52e4-d88c-4da7-aeb1-5378d02996d3"));
    CHECK_EQ(g.braced_upper(), std::string("{7B5C52E4-D88C-4DA7-AEB1-5378D02996D3}"));
    CHECK(!g.is_nil());
    CHECK(Guid().is_nil());

    // Compact ExGUID encodings (MS-FSSHTTPB 2.2.1.7)
    Buffer five = {0x2C};  // value 5 (0b00101 << 3 | 0b100)
    five.insert(five.end(), g.b.begin(), g.b.end());
    Reader r = Reader::from_bytes(five);
    ExGuid e = ExGuid::parse_compact(r);
    CHECK_EQ(e.n, 5u);
    CHECK(e.guid == g);
    Buffer nil = {0x00};
    Reader rn = Reader::from_bytes(nil);
    CHECK(ExGuid::parse_compact(rn).is_nil());
}

static void test_text() {
    const uint8_t utf16[] = {'H', 0, 0xE9, 0, 0x3D, 0xD8, 0x00, 0xDE, 0, 0};  // "Hé😀"
    CHECK_EQ(utf16le_to_utf8(utf16, sizeof utf16), std::string("H\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK_EQ(utf16_to_utf8(utf8_to_utf16("Grüße \xF0\x9F\x98\x80")), std::string("Grüße \xF0\x9F\x98\x80"));
    const uint8_t cp[] = {0x80, 'a'};
    CHECK_EQ(cp1252_to_utf8(cp, 2), std::string("\xE2\x82\xAC" "a"));

    CHECK_EQ(sanitize_filename("a/b:c*?.md"), std::string("a_b_c__.md"));
    CHECK_EQ(sanitize_filename("  CON  "), std::string("_CON"));
    CHECK_EQ(sanitize_filename("trailing dots..."), std::string("trailing dots"));
    CHECK_EQ(sanitize_filename(""), std::string("untitled"));
    CHECK_EQ(md_escape("a*b_[c]"), std::string("a\\*b\\_\\[c\\]"));
    CHECK_EQ(html_escape("<a href=\"x\">&"), std::string("&lt;a href=&quot;x&quot;&gt;&amp;"));
    CHECK_EQ(url_encode_path("My Notes/Page #1.md"), std::string("My%20Notes/Page%20%231.md"));
    CHECK_EQ(filetime_to_iso(132205810729520000ULL), std::string("2019-12-11T23:37:52Z"));
    CHECK_EQ(time32_to_iso(0x4B22D7D4), std::string("2019-12-11T23:37:56Z"));
}

static void test_property_set() {
    // Two properties: a 4-byte integer and an ObjectID array of 2, then a nested array
    Buffer b;
    auto u16 = [&](uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); };
    u16(3);
    u32(0x14001C01);  // FourBytes
    u32(0x24001C20);  // ObjectIds
    u32(0x40003489);  // ArrayOfPropertyValues
    u32(1234);
    u32(2);           // two object ids
    u32(1);           // one nested set
    u32(0x44000811);  // element property id
    u16(1);
    u32(0x20003488);  // nested ObjectId
    Reader r = Reader::from_bytes(b);
    RefCursor c;
    PropertySet ps = parse_property_set(r, c);
    CHECK_EQ(ps.values.size(), size_t(3));
    CHECK_EQ(ps.get(0x14001C01)->num, uint64_t(1234));
    CHECK_EQ(ps.get(0x24001C20)->count, 2u);
    CHECK_EQ(ps.get(0x24001C20)->ref_index, 0u);
    const PropertyValue* arr = ps.get(0x3489);
    CHECK(arr && arr->sets.size() == 1);
    // nested reference comes after the two outer ones in the object-id stream
    CHECK_EQ(arr->sets[0].get(0x20003488)->ref_index, 2u);
    CHECK_EQ(c.oid, 3u);
    CHECK(r.eof());
}

static void test_math() {
    using namespace model;
    MathObject frac;
    frac.type = MathType::Fraction;
    frac.argc = 2;
    frac.ch = U'/';
    MathObject sup;
    sup.type = MathType::Superscript;
    sup.argc = 2;
    // "x = <frac>a<sep>b<end> + <sup>c<sep>2<end>"
    std::vector<std::pair<std::string, MathObject>> segs = {
        {"\xF0\x9D\x91\xA5=", MathObject()},                             // math italic x
        {"\xEF\xB7\x90" "a\xEF\xB7\xAE" "b\xEF\xB7\xAF", frac},           // FDD0 a FDEE b FDEF
        {"+", MathObject()},
        {"\xEF\xB7\x90" "c\xEF\xB7\xAE" "2\xEF\xB7\xAF", sup},
    };
    MathSeq seq = parse_math(segs);
    CHECK_EQ(math_to_latex(seq), std::string("x=\\frac{a}{b}+{c}^{2}"));
    std::string mml = math_to_mathml(seq, false);
    CHECK(mml.find("<mfrac>") != std::string::npos);
    CHECK(mml.find("<msup>") != std::string::npos);
    CHECK_EQ(strip_math_markers("\xEF\xB7\x90" "a\xEF\xB7\xAF"), std::string("a"));

    // Greek and operators
    MathSeq g = parse_math({{"\xCE\xB1\xE2\x89\xA4\xE2\x88\x9E", MathObject()}});  // α≤∞
    CHECK_EQ(math_to_latex(g), std::string("\\alpha \\le \\infty"));

    bool threw = false;
    try {
        parse_math({{"\xEF\xB7\xAF", MathObject()}});  // stray end marker
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

static void test_inflate() {
    Buffer out;
    cab::inflate_append(kDeflate, sizeof kDeflate, out, 1 << 20);
    std::string expect = std::string(kFixtureText) + kFixtureText + kFixtureText;
    CHECK_EQ(std::string(out.begin(), out.end()), expect);
}

static void test_lzx_cab() {
    auto buf = std::make_shared<const Buffer>(kLzxCab, kLzxCab + sizeof kLzxCab);
    auto files = cab::extract(buf);
    CHECK_EQ(files.size(), size_t(1));
    if (!files.empty()) {
        CHECK_EQ(files[0].name, std::string("dir/fixture.txt"));
        CHECK_EQ(std::string(reinterpret_cast<const char*>(files[0].data.data()), files[0].data.size),
                 std::string(kFixtureText));
    }
    // Corrupt input must fail cleanly, not crash
    Buffer bad(kLzxCab, kLzxCab + sizeof kLzxCab);
    for (size_t i = 60; i < bad.size(); i += 7) bad[i] ^= 0x5A;
    try {
        cab::extract(std::make_shared<const Buffer>(bad));
    } catch (const std::exception&) {
    }
    CHECK(true);
}

static void test_garbage_store() {
    Buffer junk(4096, 0xAB);
    bool threw = false;
    try {
        load_store(std::make_shared<const Buffer>(junk));
    } catch (const ParseError&) {
        threw = true;
    }
    CHECK(threw);
}

static model::Paragraph para(const std::string& text, const std::string& style = "p") {
    model::Paragraph p;
    model::Inline in;
    in.text = text;
    p.inlines.push_back(in);
    p.style_id = style;
    return p;
}

static model::OutlineElement element(model::Paragraph p) {
    model::OutlineElement el;
    model::Content c;
    c.paragraph = std::make_shared<model::Paragraph>(std::move(p));
    el.contents.push_back(c);
    return el;
}

static void test_markdown_render() {
    using namespace model;
    Page page;
    page.title = "Test *page*";
    page.created = "2024-01-02T03:04:05Z";
    auto outline = std::make_shared<Outline>();
    outline->elements.push_back(element(para("Heading", "h1")));

    Paragraph rich;
    Inline a;
    a.text = "bold ";
    a.style.bold = true;
    Inline b;
    b.text = "link";
    b.href = "https://example.com/a b";
    rich.inlines = {a, b};
    outline->elements.push_back(element(rich));

    OutlineElement item = element(para("first"));
    item.list = ListFormat();
    item.list->bullet = "•";
    OutlineElement child = element(para("nested"));
    child.list = ListFormat();
    child.list->ordered = true;
    item.children.push_back(child);
    outline->elements.push_back(item);

    Paragraph task = para("todo");
    NoteTag tag;
    tag.shape = 3;
    tag.completed = true;
    task.tags.push_back(tag);
    outline->elements.push_back(element(task));

    PageItem pi;
    pi.outline = outline;
    page.items.push_back(pi);

    render::Options opts;
    render::LinkTable links;
    auto tmp = std::filesystem::temp_directory_path() / "oneconv_test_assets";
    render::AssetWriter assets(tmp);
    render::PageContext ctx;
    ctx.opts = &opts;
    ctx.links = &links;
    ctx.assets = &assets;
    ctx.page_dir = tmp;
    ctx.slug = "test";
    std::string md = render::render_markdown(page, ctx);
    CHECK(md.find("title: \"Test *page*\"") != std::string::npos);
    CHECK(md.find("# Test \\*page\\*\n") != std::string::npos);
    CHECK(md.find("\n## Heading\n") != std::string::npos);
    CHECK(md.find("**bold** [link](<https://example.com/a b>)") != std::string::npos);
    CHECK(md.find("- first\n  1. nested\n") != std::string::npos);
    CHECK(md.find("- [x] todo") != std::string::npos);

    std::string html = render::render_html(page, ctx, render::NavInfo());
    CHECK(html.find("<h2>Heading</h2>") != std::string::npos);
    CHECK(html.find("<strong>bold </strong>") != std::string::npos);
    CHECK(html.find("<input type=\"checkbox\" disabled checked>") != std::string::npos);
    CHECK(html.find("<ol><li>nested</li></ol>") != std::string::npos);
}

static void test_internal_links() {
    render::Options opts;
    render::LinkTable links;
    links.pages["{88D803A5-4F43-48D4-9B16-4C024F5787DC}"] = {"/out/Sec/Target Page.md", "/out/Sec/Target Page.html"};
    render::PageContext ctx;
    ctx.opts = &opts;
    ctx.links = &links;
    ctx.page_dir = "/out/Other";
    std::string href =
        "onenote:https://d.docs.live.net/x/Notes.one#Target&section-id={262ADDFB-A4DC-4453-A239-0024D6769962}"
        "&page-id={88d803a5-4f43-48d4-9b16-4c024f5787dc}&end";
    CHECK_EQ(ctx.resolve_link(href), std::string("../Sec/Target%20Page.md"));
    ctx.for_html = true;
    CHECK_EQ(ctx.resolve_link(href), std::string("../Sec/Target%20Page.html"));
    CHECK_EQ(ctx.resolve_link("https://example.com"), std::string("https://example.com"));
    std::string unknown = "onenote:#x&page-id={00000000-0000-0000-0000-000000000001}&end";
    CHECK_EQ(ctx.resolve_link(unknown), unknown);
}

int main() {
    test_guid();
    test_text();
    test_property_set();
    test_math();
    test_inflate();
    test_lzx_cab();
    test_garbage_store();
    test_markdown_render();
    test_internal_links();
    std::cout << g_passed << " checks passed, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}
