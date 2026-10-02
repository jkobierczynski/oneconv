// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Self-contained unit tests (no framework dependency). Run: oneconv_tests
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include "cab/cab.hpp"
#include "one/math.hpp"
#include "one/model.hpp"
#include "onestore/store.hpp"
#include "render/render.hpp"
#include "util/guid.hpp"
#include "util/md5.hpp"
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

static void test_obsidian_render() {
    using namespace model;
    Page page;
    page.title = "Plan: Q3 [draft]";
    page.id = "{11111111-1111-1111-1111-111111111111}";
    auto outline = std::make_shared<Outline>();

    Paragraph tagged = para("Call #support about ==this==");
    NoteTag important;
    important.label = "Remember for later";
    important.shape = 13;
    tagged.tags.push_back(important);
    outline->elements.push_back(element(tagged));

    Paragraph rich;
    Inline hl;
    hl.text = "marked";
    hl.style.highlight = Color{255, 255, 0};
    Inline link;
    link.text = "other page";
    link.href = "onenote:#Other&section-id={22222222-2222-2222-2222-222222222222}"
                "&page-id={88D803A5-4F43-48D4-9B16-4C024F5787DC}&end";
    Inline br;
    br.kind = Inline::Kind::Break;
    Inline url;
    url.text = "see https://example.com/a_b_c.";
    rich.inlines = {hl, link, br, url};
    outline->elements.push_back(element(rich));
    outline->elements.push_back(element(para("# not a heading")));
    outline->elements.push_back(element(para("1. not a list")));
    outline->elements.push_back(element(para("Real heading", "h1")));

    auto image = std::make_shared<Image>();
    image->data = Blob::from_vector({0x89, 'P', 'N', 'G', 1, 2, 3, 4});
    image->ext = ".png";
    image->width = 5.0f;  // half-inches -> 240 px
    OutlineElement img_el;
    Content ic;
    ic.kind = Content::Kind::Image;
    ic.image = image;
    img_el.contents.push_back(ic);
    outline->elements.push_back(img_el);

    auto doc = std::make_shared<Attachment>();
    doc->data = Blob::from_vector({1, 2, 3});
    doc->name = "report [v2].docx";
    OutlineElement doc_el;
    Content dc;
    dc.kind = Content::Kind::Attachment;
    dc.attachment = doc;
    doc_el.contents.push_back(dc);
    outline->elements.push_back(doc_el);

    PageItem pi;
    pi.outline = outline;
    page.items.push_back(pi);

    render::Options opts;
    opts.flavor = render::MdFlavor::Obsidian;
    opts.heading_offset = 0;
    render::LinkTable links;
    links.pages["{88D803A5-4F43-48D4-9B16-4C024F5787DC}"] = {"/v/S/Other.md", "/v/S/Other.html", "Other"};
    auto tmp = std::filesystem::temp_directory_path() / "oneconv_test_vault";
    std::set<std::string> names;
    render::AssetWriter assets(tmp, &names, true);
    render::PageContext ctx;
    ctx.opts = &opts;
    ctx.links = &links;
    ctx.assets = &assets;
    ctx.page_dir = tmp;
    ctx.slug = "Plan";
    ctx.stem = "Plan_ Q3 _draft_";
    ctx.parent_wiki = "Roadmap";
    std::string md = render::render_markdown(page, ctx);

    CHECK(md.find("# Plan") == std::string::npos);                                  // no title heading
    CHECK(md.find("aliases:\n  - \"Plan: Q3 [draft]\"") != std::string::npos);       // original title kept
    CHECK(md.find("parent: \"[[Roadmap]]\"") != std::string::npos);
    CHECK(md.find("tags:\n  - remember-for-later\n") != std::string::npos);
    CHECK(md.find("Call \\#support about \\=\\=this\\=\\= #remember-for-later") != std::string::npos);
    CHECK(md.find("==marked==[[Other|other page]]  \nsee https://example.com/a_b_c.") != std::string::npos);
    CHECK(md.find("\n\\# not a heading\n") != std::string::npos);
    CHECK(md.find("\n1\\. not a list\n") != std::string::npos);
    CHECK(md.find("\n# Real heading\n") != std::string::npos);
    CHECK(md.find("![[Plan-image-1.png|240]]") != std::string::npos);
    CHECK(md.find("[[report _v2_.docx]]") != std::string::npos);                      // link-safe file name
    CHECK(names.count("plan-image-1.png") == 1);

    // The standard flavour is unaffected
    opts.flavor = render::MdFlavor::Standard;
    opts.heading_offset = 1;
    render::AssetWriter assets2(tmp);
    ctx.assets = &assets2;
    ctx.image_counter = 0;
    std::string std_md = render::render_markdown(page, ctx);
    CHECK(std_md.find("# Plan: Q3 \\[draft\\]\n") != std::string::npos);
    CHECK(std_md.find("<mark>marked</mark>[other page](") != std::string::npos);
    CHECK(std_md.find("![image](Plan-image-1.png)") != std::string::npos);
    CHECK(std_md.find("#remember") == std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

static void test_md5_base64() {
    auto md5s = [](const std::string& t) { return md5_hex(reinterpret_cast<const uint8_t*>(t.data()), t.size()); };
    // RFC 1321 test suite
    CHECK_EQ(md5s(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(md5s("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(md5s("message digest"), std::string("f96b697d7cb7938d525a2f31aaf161d0"));
    CHECK_EQ(md5s("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
             std::string("57edf4a22be3c955ac49da2e2107b67a"));
    CHECK_EQ(md5s(std::string(55, 'a')), std::string("ef1772b6dff9a122358552954ad0df65"));  // padding boundaries
    CHECK_EQ(md5s(std::string(56, 'a')), std::string("3b0c8ac703f828b04c6c197006d17218"));
    CHECK_EQ(md5s(std::string(64, 'a')), std::string("014842d480b571495a4a0363793f7367"));
    auto b64 = [](const std::string& t) { return base64(reinterpret_cast<const uint8_t*>(t.data()), t.size()); };
    CHECK_EQ(b64(""), std::string(""));
    CHECK_EQ(b64("f"), std::string("Zg=="));
    CHECK_EQ(b64("fo"), std::string("Zm8="));
    CHECK_EQ(b64("foobar"), std::string("Zm9vYmFy"));
    std::ostringstream wrapped;
    std::string long_text(100, 'x');
    write_base64(wrapped, reinterpret_cast<const uint8_t*>(long_text.data()), long_text.size(), 76);
    CHECK_EQ(wrapped.str().find('\n'), size_t(76));
}

static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

static void test_ink_png() {
    using namespace model;
    Ink ink;
    InkStroke red;
    red.color = 0x0000FF;  // COLORREF: red
    red.width = red.height = 53;
    for (int i = 0; i <= 40; ++i) red.points.emplace_back(100.0f * static_cast<float>(i), 50.0f * static_cast<float>(i % 7));
    ink.strokes.push_back(red);
    render::PngImage png = render::ink_to_png(ink);
    CHECK(png.width > 100 && png.height > 5);
    const Buffer& d = png.data;
    CHECK(d.size() > 60 && std::memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    if (d.size() <= 60) return;
    // Walk the chunks, then inflate the image data with the project's own decoder
    int w = 0, h = 0;
    Buffer idat;
    bool end = false;
    for (size_t pos = 8; pos + 12 <= d.size();) {
        uint32_t len = be32(&d[pos]);
        std::string type(reinterpret_cast<const char*>(&d[pos + 4]), 4);
        if (pos + 12 + len > d.size()) break;
        if (type == "IHDR") {
            w = static_cast<int>(be32(&d[pos + 8]));
            h = static_cast<int>(be32(&d[pos + 12]));
            CHECK_EQ(int(d[pos + 16]), 8);  // bit depth
            CHECK_EQ(int(d[pos + 17]), 2);  // colour type RGB
        }
        if (type == "IDAT") idat.insert(idat.end(), d.begin() + static_cast<long>(pos + 8), d.begin() + static_cast<long>(pos + 8 + len));
        if (type == "IEND") end = true;
        pos += 12 + len;
    }
    CHECK(end);
    CHECK_EQ(w, png.width * 2);  // rendered at twice the display size
    CHECK(idat.size() > 6);
    Buffer raw;
    cab::inflate_append(idat.data() + 2, idat.size() - 6, raw, size_t(1) << 28);  // strip zlib header and Adler-32
    size_t stride = static_cast<size_t>(w) * 3 + 1;
    CHECK_EQ(raw.size(), stride * static_cast<size_t>(h));
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    CHECK_EQ((b << 16) | a, be32(&idat[idat.size() - 4]));
    size_t red_px = 0, white_px = 0;
    for (int y = 0; y < h && raw.size() == stride * static_cast<size_t>(h); ++y) {
        const uint8_t* row = &raw[static_cast<size_t>(y) * stride];
        CHECK(row[0] == 0);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + 1 + x * 3;
            if (p[0] == 255 && p[1] == 0 && p[2] == 0) ++red_px;
            if (p[0] == 255 && p[1] == 255 && p[2] == 255) ++white_px;
        }
    }
    CHECK(red_px > 200);                // the stroke was drawn in its colour
    CHECK(white_px > red_px);           // on a white background
    CHECK(d.size() < raw.size() / 4);   // and the encoder actually compresses
    CHECK(render::ink_to_png(Ink()).data.empty());
}

static std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_enex_export() {
    using namespace model;
    auto section = std::make_shared<Section>();
    section->name = "Work: notes";
    Page page;
    page.title = "  Plan <A&B>\n";
    page.created = "2024-01-02T03:04:05Z";
    page.modified = "2024-02-03T04:05:06Z";
    page.author = "J. K.";
    auto outline = std::make_shared<Outline>();

    Paragraph task = para("buy milk");
    NoteTag todo;
    todo.shape = 3;
    todo.completed = true;
    todo.label = "To Do";
    task.tags.push_back(todo);
    outline->elements.push_back(element(task));

    Paragraph star = para("remember ]]> this");
    NoteTag imp;
    imp.shape = 13;
    imp.label = "Important, urgent";
    star.tags.push_back(imp);
    outline->elements.push_back(element(star));

    Paragraph links;
    Inline ext;
    ext.text = "site";
    ext.href = "https://example.com/?a=1&b=2";
    Inline internal;
    internal.text = "other page";
    internal.href = "onenote:#Other&page-id={88D803A5-4F43-48D4-9B16-4C024F5787DC}&end";
    Inline evil;
    evil.text = "x";
    evil.href = "javascript:alert(1)";
    links.inlines = {ext, internal, evil};
    outline->elements.push_back(element(links));
    outline->elements.push_back(element(para("Heading", "h2")));

    OutlineElement bullet = element(para("item"));
    bullet.list = ListFormat();
    outline->elements.push_back(bullet);

    Blob picture = Blob::from_vector({0x89, 'P', 'N', 'G', 1, 2, 3, 4});
    for (int i = 0; i < 2; ++i) {  // the same picture twice must be stored once
        auto image = std::make_shared<Image>();
        image->data = picture;
        image->ext = ".png";
        image->width = 4.0f;
        image->height = 2.0f;
        OutlineElement el;
        Content c;
        c.kind = Content::Kind::Image;
        c.image = image;
        el.contents.push_back(c);
        outline->elements.push_back(el);
    }
    auto doc = std::make_shared<Attachment>();
    doc->data = Blob::from_vector({'h', 'e', 'l', 'l', 'o'});
    doc->name = "report.docx";
    OutlineElement doc_el;
    Content dc;
    dc.kind = Content::Kind::Attachment;
    dc.attachment = doc;
    doc_el.contents.push_back(dc);
    outline->elements.push_back(doc_el);

    PageItem pi;
    pi.outline = outline;
    page.items.push_back(pi);
    section->pages.push_back(page);
    section->pages.push_back(Page());  // an empty, untitled page

    Notebook nb;
    nb.name = "NB";
    auto group = std::make_shared<SectionGroup>();
    group->name = "Group";
    NotebookEntry se;
    se.section = section;
    group->entries.push_back(se);
    NotebookEntry ge;
    ge.group = group;
    nb.entries.push_back(ge);

    render::Options opts;
    opts.enex = true;
    opts.heading_offset = 0;
    auto tmp = std::filesystem::temp_directory_path() / "oneconv_test_enex";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    render::ExportStats st = render::export_enex(nb, tmp, opts);
    CHECK_EQ(st.sections, 1);
    CHECK_EQ(st.pages, 2);
    CHECK_EQ(st.assets, 2);
    std::string x = slurp(tmp / "Group" / "Work_ notes.enex");
    CHECK(x.find("<!DOCTYPE en-export SYSTEM \"http://xml.evernote.com/pub/evernote-export4.dtd\">") != std::string::npos);
    CHECK(x.find("<title>Plan &lt;A&amp;B&gt;</title>") != std::string::npos);
    CHECK(x.find("<title>Untitled Page</title>") != std::string::npos);
    CHECK(x.find("<created>20240102T030405Z</created>\n<updated>20240203T040506Z</updated>") != std::string::npos);
    CHECK(x.find("<tag>Important  urgent</tag>") != std::string::npos);      // commas are not allowed in tag names
    CHECK(x.find("<tag>To Do</tag>") == std::string::npos);                    // a plain task is not a tag
    CHECK(x.find("<note-attributes><author>J. K.</author></note-attributes>") != std::string::npos);
    CHECK(x.find("<div><en-todo checked=\"true\"/>buy milk</div>") != std::string::npos);
    CHECK(x.find("remember ]]&gt; this") != std::string::npos);                 // cannot close the CDATA section
    CHECK(x.find("<a href=\"https://example.com/?a=1&amp;b=2\">site</a>other pagex</div>") != std::string::npos);
    CHECK(x.find("javascript") == std::string::npos);
    CHECK(x.find("onenote:") == std::string::npos);
    CHECK(x.find("<h2>Heading</h2>") != std::string::npos);
    CHECK(x.find("<ul><li><div>item</div></li></ul>") != std::string::npos);
    const uint8_t png_bytes[] = {0x89, 'P', 'N', 'G', 1, 2, 3, 4};
    std::string hash = md5_hex(png_bytes, sizeof png_bytes);
    std::string media = "<en-media type=\"image/png\" hash=\"" + hash + "\" width=\"192\" height=\"96\"/>";
    size_t first = x.find(media);
    CHECK(first != std::string::npos && x.find(media, first + 1) != std::string::npos);  // referenced twice
    CHECK(x.find("<data encoding=\"base64\">\niVBORwECAwQ=\n</data>\n<mime>image/png</mime>\n<width>192</width>") != std::string::npos);
    CHECK(x.find("iVBORwECAwQ=") == x.rfind("iVBORwECAwQ="));                    // ...but stored once
    CHECK(x.find("<file-name>report.docx</file-name><attachment>true</attachment>") != std::string::npos);
    CHECK(x.find("application/vnd.openxmlformats-officedocument.wordprocessingml.document") != std::string::npos);
    CHECK(x.find("<en-note><div><br/></div></en-note>") != std::string::npos);  // the empty page
    CHECK(x.rfind("</en-export>\n") == x.size() - 13);
    std::filesystem::remove_all(tmp, ec);
}

static void test_internal_links() {
    render::Options opts;
    render::LinkTable links;
    links.pages["{88D803A5-4F43-48D4-9B16-4C024F5787DC}"] = {"/out/Sec/Target Page.md", "/out/Sec/Target Page.html", "Target Page"};
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
    test_obsidian_render();
    test_md5_base64();
    test_ink_png();
    test_enex_export();
    test_internal_links();
    std::cout << g_passed << " checks passed, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}
