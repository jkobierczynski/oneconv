// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "math.hpp"

#include <cctype>
#include <map>
#include <stdexcept>

#include "../util/text.hpp"

namespace oneconv::model {

namespace {

constexpr uint32_t kStart = 0xFDD0;
constexpr uint32_t kSep = 0xFDEE;
constexpr uint32_t kEnd = 0xFDEF;

struct Token {
    enum Kind { Text, Start, Sep, End, Eof } kind = Eof;
    std::string text;
    MathObject obj;
};

std::vector<Token> tokenize(const std::vector<std::pair<std::string, MathObject>>& segments) {
    std::vector<Token> out;
    for (const auto& seg : segments) {
        std::string pending;
        auto flush = [&]() {
            if (!pending.empty()) {
                Token t;
                t.kind = Token::Text;
                t.text = pending;
                out.push_back(t);
                pending.clear();
            }
        };
        for (uint32_t cp : utf8_codepoints(seg.first)) {
            if (cp == kStart || cp == kSep || cp == kEnd) {
                flush();
                Token t;
                t.kind = cp == kStart ? Token::Start : cp == kSep ? Token::Sep : Token::End;
                t.obj = seg.second;
                out.push_back(t);
            } else {
                append_utf8(pending, cp);
            }
        }
        flush();
    }
    return out;
}

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    MathSeq parse_all() {
        MathSeq seq = parse_seq(0);
        if (pos_ < toks_.size()) throw std::runtime_error("unbalanced math markers");
        return seq;
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    MathSeq parse_seq(int depth) {
        if (depth > 64) throw std::runtime_error("math nested too deeply");
        MathSeq seq;
        while (pos_ < toks_.size()) {
            const Token& t = toks_[pos_];
            if (t.kind == Token::Sep || t.kind == Token::End) {
                if (depth == 0) throw std::runtime_error("unexpected math separator");
                break;
            }
            if (t.kind == Token::Text) {
                MathNode n;
                n.text = t.text;
                seq.push_back(std::move(n));
                ++pos_;
                continue;
            }
            // Start of an object
            MathNode n;
            n.is_object = true;
            n.obj = t.obj;
            ++pos_;
            if (n.obj.type == MathType::OpChar && n.obj.argc == 0) {
                // OpChar has no arguments: expect an immediate end marker.
                if (pos_ < toks_.size() && toks_[pos_].kind == Token::End) ++pos_;
                seq.push_back(std::move(n));
                continue;
            }
            while (true) {
                n.args.push_back(parse_seq(depth + 1));
                if (pos_ >= toks_.size()) throw std::runtime_error("unterminated math object");
                if (toks_[pos_].kind == Token::Sep) {
                    ++pos_;
                    continue;
                }
                ++pos_;  // End
                break;
            }
            seq.push_back(std::move(n));
        }
        return seq;
    }
};

// ---------------------------------------------------------------------------
// Character mapping

const char* greek_names[] = {"Alpha",  "Beta",   "Gamma", "Delta",    "Epsilon", "Zeta",    "Eta",   "Theta",
                             "Iota",   "Kappa",  "Lambda", "Mu",      "Nu",      "Xi",      "Omicron", "Pi",
                             "Rho",    "Theta",  "Sigma", "Tau",      "Upsilon", "Phi",     "Chi",   "Psi",
                             "Omega",  "nabla",  "alpha", "beta",     "gamma",   "delta",   "epsilon", "zeta",
                             "eta",    "theta",  "iota",  "kappa",    "lambda",  "mu",      "nu",    "xi",
                             "omicron", "pi",    "rho",   "varsigma", "sigma",   "tau",     "upsilon", "phi",
                             "chi",    "psi",    "omega", "partial",  "epsilon", "vartheta", "varkappa", "phi",
                             "varrho", "varpi"};

struct Decoded {
    uint32_t base = 0;     // ASCII letter/digit or Greek code point
    const char* style = "";  // LaTeX font command, "" for italic/normal
    int greek_index = -1;  // index into greek_names when Greek
};

/// Map Mathematical Alphanumeric Symbols (U+1D400..U+1D7FF) to base characters.
std::optional<Decoded> decode_math_alnum(uint32_t cp) {
    static const char* latin_styles[] = {"\\mathbf",   "",          "\\boldsymbol", "\\mathcal", "\\mathcal",
                                         "\\mathfrak", "\\mathbb",  "\\mathfrak",   "\\mathsf",  "\\mathsf",
                                         "\\mathsf",   "\\mathsf",  "\\mathtt"};
    if (cp == 0x210E) return Decoded{'h', "", -1};  // planck constant = italic h
    if (cp >= 0x1D400 && cp < 0x1D400 + 13 * 52) {
        uint32_t off = cp - 0x1D400;
        uint32_t style = off / 52, idx = off % 52;
        uint32_t base = idx < 26 ? 'A' + idx : 'a' + (idx - 26);
        return Decoded{base, latin_styles[style], -1};
    }
    if (cp >= 0x1D6A8 && cp < 0x1D6A8 + 5 * 58) {
        uint32_t off = cp - 0x1D6A8;
        uint32_t style = off / 58, idx = off % 58;
        static const char* greek_styles[] = {"\\boldsymbol", "", "\\boldsymbol", "\\boldsymbol", "\\boldsymbol"};
        return Decoded{0, greek_styles[style], static_cast<int>(idx)};
    }
    if (cp >= 0x1D7CE && cp <= 0x1D7FF) {
        uint32_t off = cp - 0x1D7CE;
        static const char* digit_styles[] = {"\\mathbf", "\\mathbb", "\\mathsf", "\\mathsf", "\\mathtt"};
        return Decoded{'0' + off % 10, digit_styles[off / 10], -1};
    }
    // Letterlike symbols used to fill holes in the math alphabets
    static const std::map<uint32_t, std::pair<char, const char*>> letterlike = {
        {0x212C, {'B', "\\mathcal"}}, {0x2130, {'E', "\\mathcal"}}, {0x2131, {'F', "\\mathcal"}},
        {0x210B, {'H', "\\mathcal"}}, {0x2110, {'I', "\\mathcal"}}, {0x2112, {'L', "\\mathcal"}},
        {0x2133, {'M', "\\mathcal"}}, {0x211B, {'R', "\\mathcal"}}, {0x212F, {'e', "\\mathcal"}},
        {0x210A, {'g', "\\mathcal"}}, {0x2134, {'o', "\\mathcal"}}, {0x212D, {'C', "\\mathfrak"}},
        {0x210C, {'H', "\\mathfrak"}}, {0x2111, {'I', "\\mathfrak"}}, {0x211C, {'R', "\\mathfrak"}},
        {0x2128, {'Z', "\\mathfrak"}}, {0x2102, {'C', "\\mathbb"}},   {0x210D, {'H', "\\mathbb"}},
        {0x2115, {'N', "\\mathbb"}},   {0x2119, {'P', "\\mathbb"}},   {0x211A, {'Q', "\\mathbb"}},
        {0x211D, {'R', "\\mathbb"}},   {0x2124, {'Z', "\\mathbb"}},
    };
    auto it = letterlike.find(cp);
    if (it != letterlike.end()) return Decoded{static_cast<uint32_t>(it->second.first), it->second.second, -1};
    // Plain Greek block
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
        int idx = static_cast<int>(cp - 0x391);
        if (cp > 0x3A2) idx -= 1;  // no capital final sigma
        // capital order in our table skips nothing up to Rho (16), then Theta-symbol at 17
        static const int cap_map[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20, 21, 22, 23, 24};
        if (idx >= 0 && idx < 24) return Decoded{0, "", cap_map[idx]};
    }
    if (cp >= 0x3B1 && cp <= 0x3C9) return Decoded{0, "", static_cast<int>(26 + (cp - 0x3B1))};
    if (cp == 0x3D5) return Decoded{0, "", 47};  // phi symbol
    if (cp == 0x3D1) return Decoded{0, "", 53};
    if (cp == 0x3F5) return Decoded{0, "", 52};
    if (cp == 0x3D6) return Decoded{0, "", 57};
    if (cp == 0x3F1) return Decoded{0, "", 56};
    return std::nullopt;
}

const std::map<uint32_t, const char*>& latex_symbols() {
    static const std::map<uint32_t, const char*> m = {
        {0x2212, "-"},          {0x22C5, "\\cdot "},     {0x00B7, "\\cdot "},    {0x00D7, "\\times "},
        {0x00F7, "\\div "},     {0x00B1, "\\pm "},       {0x2213, "\\mp "},      {0x2264, "\\le "},
        {0x2265, "\\ge "},      {0x2260, "\\neq "},      {0x2248, "\\approx "},  {0x221E, "\\infty "},
        {0x2192, "\\to "},      {0x2190, "\\leftarrow "}, {0x2194, "\\leftrightarrow "}, {0x21D2, "\\Rightarrow "},
        {0x21D0, "\\Leftarrow "}, {0x21D4, "\\Leftrightarrow "}, {0x2202, "\\partial "}, {0x2207, "\\nabla "},
        {0x2208, "\\in "},      {0x2209, "\\notin "},    {0x220B, "\\ni "},      {0x2282, "\\subset "},
        {0x2283, "\\supset "},  {0x2286, "\\subseteq "}, {0x2287, "\\supseteq "}, {0x222A, "\\cup "},
        {0x2229, "\\cap "},     {0x2200, "\\forall "},   {0x2203, "\\exists "},  {0x00AC, "\\neg "},
        {0x2227, "\\wedge "},   {0x2228, "\\vee "},      {0x2261, "\\equiv "},   {0x221D, "\\propto "},
        {0x00B0, "^\\circ "},   {0x2032, "'"},           {0x2033, "''"},         {0x2026, "\\ldots "},
        {0x22EF, "\\cdots "},   {0x22EE, "\\vdots "},    {0x22F1, "\\ddots "},   {0x2218, "\\circ "},
        {0x2205, "\\emptyset "}, {0x2211, "\\sum "},     {0x220F, "\\prod "},    {0x222B, "\\int "},
        {0x222C, "\\iint "},    {0x222D, "\\iiint "},    {0x222E, "\\oint "},    {0x221A, "\\surd "},
        {0x2223, "\\mid "},     {0x2225, "\\parallel "}, {0x22A5, "\\perp "},    {0x2220, "\\angle "},
        {0x226A, "\\ll "},      {0x226B, "\\gg "},       {0x223C, "\\sim "},     {0x2245, "\\cong "},
        {0x2243, "\\simeq "},   {0x2295, "\\oplus "},    {0x2297, "\\otimes "},  {0x2299, "\\odot "},
        {0x210F, "\\hbar "},    {0x2113, "\\ell "},      {0x2135, "\\aleph "},   {0x2146, "\\mathrm{d}"},
        {0x2147, "\\mathrm{e}"}, {0x2148, "\\mathrm{i}"}, {0x27E8, "\\langle "}, {0x27E9, "\\rangle "},
        {0x2016, "\\| "},       {0x230A, "\\lfloor "},   {0x230B, "\\rfloor "},  {0x2308, "\\lceil "},
        {0x2309, "\\rceil "},   {0x2234, "\\therefore "}, {0x2235, "\\because "}, {0x21A6, "\\mapsto "},
        {0x2272, "\\lesssim "}, {0x2273, "\\gtrsim "},   {0x2266, "\\leqq "},    {0x2267, "\\geqq "},
        {0x00A0, "~"},          {0x2009, "\\,"},         {0x2005, "\\;"},        {0x2003, "\\quad "},
        {0x2002, "\\enspace "},
    };
    return m;
}

bool is_invisible(uint32_t cp) {
    return cp == 0x2061 || cp == 0x2062 || cp == 0x2063 || cp == 0x2064 || cp == 0x200B || cp == 0x200C ||
           cp == 0x200D || cp == 0xFEFF;
}

std::string latex_char(uint32_t cp) {
    if (is_invisible(cp)) return "";
    if (auto d = decode_math_alnum(cp)) {
        std::string body;
        if (d->greek_index >= 0)
            body = std::string("\\") + greek_names[d->greek_index] + " ";
        else
            append_utf8(body, d->base);
        // Capital Greek letters that look like Latin ones have no LaTeX macro
        static const char* latin_like[] = {"\\Alpha ", "\\Beta ", "\\Epsilon ", "\\Zeta ", "\\Eta ", "\\Iota ",
                                           "\\Kappa ", "\\Mu ", "\\Nu ", "\\Omicron ", "\\Rho ", "\\Tau ",
                                           "\\Chi ", "\\omicron "};
        static const char* repl[] = {"A", "B", "E", "Z", "H", "I", "K", "M", "N", "O", "P", "T", "X", "o"};
        for (size_t i = 0; i < sizeof(repl) / sizeof(repl[0]); ++i)
            if (body == latin_like[i]) body = repl[i];
        if (*d->style) return std::string(d->style) + "{" + body + "}";
        return body;
    }
    auto& sym = latex_symbols();
    auto it = sym.find(cp);
    if (it != sym.end()) return it->second;
    switch (cp) {
        case '{': return "\\{";
        case '}': return "\\}";
        case '#': return "\\#";
        case '%': return "\\%";
        case '$': return "\\$";
        case '_': return "\\_";
        case '\\': return "\\backslash ";
        case '~': return "\\sim ";
        case '^': return "\\hat{}";
        default: break;
    }
    std::string s;
    append_utf8(s, cp);
    return s;
}

std::string latex_text(const std::string& text) {
    std::string out;
    for (uint32_t cp : utf8_codepoints(text)) out += latex_char(cp);
    return out;
}

std::string cp_str(char32_t c) {
    std::string s;
    append_utf8(s, c);
    return s;
}

bool is_empty_seq(const MathSeq& s) {
    for (const auto& n : s) {
        if (n.is_object) return false;
        for (uint32_t cp : utf8_codepoints(n.text))
            if (!is_invisible(cp) && cp != ' ') return false;
    }
    return true;
}

std::string latex_bracket(std::optional<char32_t> c, bool open) {
    if (!c) return open ? "(" : ")";
    switch (*c) {
        case 0: return ".";
        case '{': return "\\{";
        case '}': return "\\}";
        case 0x27E8: return "\\langle ";
        case 0x27E9: return "\\rangle ";
        case 0x230A: return "\\lfloor ";
        case 0x230B: return "\\rfloor ";
        case 0x2308: return "\\lceil ";
        case 0x2309: return "\\rceil ";
        case '|': return "|";
        case 0x2016: return "\\|";
        default: return cp_str(*c);
    }
}

/// OneNote's empty-equation prompt: a box with a dotted-square marker.
bool is_placeholder(const MathNode& n) {
    return n.is_object && n.obj.type == MathType::Box && n.obj.ch && *n.obj.ch == 0x2B1A;
}

std::string latex_seq(const MathSeq& seq);

std::string arg(const MathNode& n, size_t i) { return i < n.args.size() ? latex_seq(n.args[i]) : ""; }
bool arg_empty(const MathNode& n, size_t i) { return i >= n.args.size() || is_empty_seq(n.args[i]); }
std::string group(const std::string& s) { return "{" + s + "}"; }

std::string latex_node(const MathNode& n) {
    if (!n.is_object) return latex_text(n.text);
    const MathObject& o = n.obj;
    if (is_placeholder(n)) return "";
    switch (o.type) {
        case MathType::PlainText: {
            std::string s;
            for (size_t i = 0; i < n.args.size(); ++i) s += "\\text{" + math_to_text(n.args[i]) + "}";
            return s;
        }
        case MathType::SimpleText:
        case MathType::Box:
        case MathType::Phantom: {
            std::string s;
            for (size_t i = 0; i < n.args.size(); ++i) s += arg(n, i);
            return s;
        }
        case MathType::Accent: {
            char32_t c = o.ch.value_or(0x302);
            const char* cmd = "\\hat";
            switch (c) {
                case 0x300: cmd = "\\grave"; break;
                case 0x301: cmd = "\\acute"; break;
                case 0x303: cmd = "\\tilde"; break;
                case 0x304:
                case 0x305: cmd = "\\bar"; break;
                case 0x306: cmd = "\\breve"; break;
                case 0x307: cmd = "\\dot"; break;
                case 0x308: cmd = "\\ddot"; break;
                case 0x30C: cmd = "\\check"; break;
                case 0x20D7:
                case 0x20D1: cmd = "\\vec"; break;
                default: break;
            }
            return std::string(cmd) + group(arg(n, 0));
        }
        case MathType::BoxedFormula: return "\\boxed" + group(arg(n, 0));
        case MathType::Brackets:
            return "\\left" + latex_bracket(o.ch, true) + arg(n, 0) + "\\right" + latex_bracket(o.ch1, false);
        case MathType::BracketsWithSeps: {
            std::string sep = o.ch2 ? latex_char(*o.ch2) : "|";
            std::string s = "\\left" + latex_bracket(o.ch, true);
            for (size_t i = 0; i < n.args.size(); ++i) {
                if (i) s += sep == "|" ? "\\middle|" : sep;
                s += arg(n, i);
            }
            return s + "\\right" + latex_bracket(o.ch1, false);
        }
        case MathType::EquationArray: {
            std::string s = "\\begin{aligned}";
            for (size_t i = 0; i < n.args.size(); ++i) {
                if (i) s += " \\\\ ";
                s += arg(n, i);
            }
            return s + "\\end{aligned}";
        }
        case MathType::Fraction: {
            // '/' (or none) is the normal stacked fraction
            if (o.ch && *o.ch == 0xA6) return "\\genfrac{}{}{0pt}{}" + group(arg(n, 0)) + group(arg(n, 1));
            if (o.ch && *o.ch == 0x2298) return "\\tfrac" + group(arg(n, 0)) + group(arg(n, 1));
            return "\\frac" + group(arg(n, 0)) + group(arg(n, 1));
        }
        case MathType::SlashedFraction: return group(arg(n, 0)) + "/" + group(arg(n, 1));
        case MathType::Stack: return "\\genfrac{}{}{0pt}{}" + group(arg(n, 0)) + group(arg(n, 1));
        case MathType::FunctionApply: {
            std::string fn = n.args.empty() ? "" : math_to_text(n.args[0]);
            static const char* known[] = {"sin",  "cos",  "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan",
                                          "sinh", "cosh", "tanh", "log", "ln",  "lg",  "exp",    "det",    "dim",
                                          "max",  "min",  "sup", "inf", "lim", "gcd", "arg",    "deg",    "ker"};
            std::string f = arg(n, 0);
            for (const char* k : known)
                if (fn == k) f = std::string("\\") + k;
            if (f == fn && !fn.empty() && fn.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos && fn.size() > 1)
                f = "\\operatorname{" + fn + "}";
            return f + " " + arg(n, 1);
        }
        case MathType::LeftSubSup: {
            std::string s = "{}";
            if (!arg_empty(n, 0)) s += "_" + group(arg(n, 0));
            if (!arg_empty(n, 1)) s += "^" + group(arg(n, 1));
            return s + arg(n, 2);
        }
        case MathType::LowerLimit: {
            std::string base = n.args.empty() ? "" : math_to_text(n.args[0]);
            if (base == "lim" || base == "max" || base == "min" || base == "sup" || base == "inf")
                return "\\" + base + "_" + group(arg(n, 1));
            return "\\underset" + group(arg(n, 1)) + group(arg(n, 0));
        }
        case MathType::UpperLimit: return "\\overset" + group(arg(n, 1)) + group(arg(n, 0));
        case MathType::Matrix: {
            int cols = o.column.value_or(1);
            if (cols <= 0) cols = 1;
            std::string env = "matrix";
            if (o.ch) {
                if (*o.ch == 0x24A8) env = "pmatrix";
                else if (*o.ch == 0x24B1) env = "vmatrix";
                else if (*o.ch == 0x24A9) env = "Vmatrix";
            }
            std::string s = "\\begin{" + env + "}";
            for (size_t i = 0; i < n.args.size(); ++i) {
                if (i) s += (i % static_cast<size_t>(cols) == 0) ? " \\\\ " : " & ";
                s += arg(n, i);
            }
            return s + "\\end{" + env + "}";
        }
        case MathType::Nary: {
            char32_t op = o.ch.value_or(0x222B);
            std::string s;
            auto& sym = latex_symbols();
            static const std::map<char32_t, const char*> big = {{0x22C3, "\\bigcup "}, {0x22C2, "\\bigcap "},
                                                                {0x2210, "\\coprod "}, {0x22C1, "\\bigvee "},
                                                                {0x22C0, "\\bigwedge "}, {0x2A01, "\\bigoplus "},
                                                                {0x2A02, "\\bigotimes "}, {0x2A00, "\\bigodot "}};
            auto b = big.find(op);
            auto it = sym.find(op);
            if (b != big.end())
                s = b->second;
            else if (it != sym.end())
                s = it->second;
            else
                s = cp_str(op);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            if (!arg_empty(n, 0)) s += "_" + group(arg(n, 0));
            if (!arg_empty(n, 1)) s += "^" + group(arg(n, 1));
            return s + " " + arg(n, 2);
        }
        case MathType::OpChar: return o.ch ? latex_char(*o.ch) : "";
        case MathType::Overbar: return "\\overline" + group(arg(n, 0));
        case MathType::Underbar: return "\\underline" + group(arg(n, 0));
        case MathType::Radical:
            if (arg_empty(n, 0)) return "\\sqrt" + group(arg(n, 1));
            return "\\sqrt[" + arg(n, 0) + "]" + group(arg(n, 1));
        case MathType::StretchStack: {
            char32_t c = o.ch.value_or(0x23DF);
            if (c == 0x23DF) return "\\underbrace" + group(arg(n, 0));
            if (c == 0x23DE) return "\\overbrace" + group(arg(n, 0));
            if (c == 0x2192) return "\\overrightarrow" + group(arg(n, 0));
            if (c == 0x2190) return "\\overleftarrow" + group(arg(n, 0));
            bool below = o.align.value_or(0) == 0;
            return std::string(below ? "\\underset" : "\\overset") + group(latex_char(c)) + group(arg(n, 0));
        }
        case MathType::Subscript: return group(arg(n, 0)) + "_" + group(arg(n, 1));
        case MathType::Superscript: return group(arg(n, 0)) + "^" + group(arg(n, 1));
        case MathType::SubSup: return group(arg(n, 0)) + "_" + group(arg(n, 1)) + "^" + group(arg(n, 2));
    }
    std::string s;
    for (size_t i = 0; i < n.args.size(); ++i) s += arg(n, i);
    return s;
}

std::string latex_seq(const MathSeq& seq) {
    std::string s;
    for (const auto& n : seq) {
        std::string part = latex_node(n);
        // Keep commands from merging with following letters (e.g. "\alpha" + "x").
        if (!s.empty() && !part.empty() && s.back() != ' ' && std::isalpha(static_cast<unsigned char>(part[0]))) {
            size_t bs = s.rfind('\\');
            if (bs != std::string::npos) {
                bool word = bs + 1 < s.size();
                for (size_t k = bs + 1; k < s.size(); ++k)
                    if (!std::isalpha(static_cast<unsigned char>(s[k]))) word = false;
                if (word) s.push_back(' ');
            }
        }
        s += part;
    }
    return s;
}

// ---------------------------------------------------------------------------
// MathML

bool is_function_name(const std::string& w) {
    static const char* names[] = {"sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh",
                                  "tanh", "log", "ln", "lg", "exp", "det", "dim", "max", "min", "sup", "inf", "lim",
                                  "gcd", "arg", "deg", "ker", "mod"};
    for (const char* n : names)
        if (w == n) return true;
    return false;
}

std::string mml_text(const std::string& text) {
    // Known function names (sin, lim, ...) become a single upright identifier
    std::vector<uint32_t> cps = utf8_codepoints(text);
    std::string out;
    std::string num;
    std::string word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        if (is_function_name(word)) {
            out += "<mi>" + word + "</mi>";
        } else {
            for (char c : word) out += std::string("<mi>") + c + "</mi>";
        }
        word.clear();
    };
    auto flush_num = [&]() {
        if (!num.empty()) {
            out += "<mn>" + num + "</mn>";
            num.clear();
        }
    };
    for (uint32_t cp : cps) {
        if (cp < 0x80 && std::isalpha(static_cast<int>(cp))) {
            flush_num();
            word.push_back(static_cast<char>(cp));
            continue;
        }
        flush_word();
        if (is_invisible(cp)) {
            flush_num();
            if (cp == 0x2061) out += "<mo>&#x2061;</mo>";
            continue;
        }
        if ((cp >= '0' && cp <= '9') || (cp == '.' && !num.empty())) {
            append_utf8(num, cp);
            continue;
        }
        flush_num();
        std::string ch;
        append_utf8(ch, cp);
        bool letter = (cp < 0x80 && std::isalpha(static_cast<int>(cp))) || decode_math_alnum(cp).has_value() ||
                      (cp >= 0x370 && cp <= 0x3FF);
        if (cp == ' ')
            out += "<mspace width=\"0.25em\"/>";
        else if (letter)
            out += "<mi>" + html_escape(ch) + "</mi>";
        else
            out += "<mo>" + html_escape(ch) + "</mo>";
    }
    flush_num();
    flush_word();
    return out;
}

std::string mml_seq(const MathSeq& seq);
std::string mrow(const std::string& s) { return "<mrow>" + s + "</mrow>"; }
std::string marg(const MathNode& n, size_t i) { return mrow(i < n.args.size() ? mml_seq(n.args[i]) : ""); }
std::string mo(const std::string& s, const char* attrs = "") {
    return std::string("<mo") + attrs + ">" + html_escape(s) + "</mo>";
}

std::string mml_node(const MathNode& n) {
    if (!n.is_object) return mml_text(n.text);
    if (is_placeholder(n)) return "";
    const MathObject& o = n.obj;
    switch (o.type) {
        case MathType::SimpleText:
        case MathType::PlainText:
        case MathType::Box:
        case MathType::Phantom: {
            std::string s;
            for (size_t i = 0; i < n.args.size(); ++i) s += marg(n, i);
            return s;
        }
        case MathType::Accent:
            return "<mover accent=\"true\">" + marg(n, 0) + mo(cp_str(o.ch.value_or(0x302))) + "</mover>";
        case MathType::BoxedFormula:
            return "<mrow style=\"border:1px solid currentColor;padding:2px\">" + marg(n, 0) + "</mrow>";
        case MathType::Brackets: {
            std::string open = o.ch ? (*o.ch ? cp_str(*o.ch) : "") : "(";
            std::string close = o.ch1 ? (*o.ch1 ? cp_str(*o.ch1) : "") : ")";
            return mrow(mo(open, " fence=\"true\" lspace=\"0\" rspace=\"0\"") + marg(n, 0) +
                        mo(close, " fence=\"true\" lspace=\"0\" rspace=\"0\""));
        }
        case MathType::BracketsWithSeps: {
            std::string s = mo(o.ch ? cp_str(*o.ch) : "(");
            for (size_t i = 0; i < n.args.size(); ++i) {
                if (i) s += mo(o.ch2 ? cp_str(*o.ch2) : "|");
                s += marg(n, i);
            }
            return mrow(s + mo(o.ch1 ? cp_str(*o.ch1) : ")"));
        }
        case MathType::EquationArray: {
            std::string s = "<mtable>";
            for (size_t i = 0; i < n.args.size(); ++i) s += "<mtr><mtd>" + marg(n, i) + "</mtd></mtr>";
            return s + "</mtable>";
        }
        case MathType::Fraction:
            if (o.ch && *o.ch == 0xA6) return "<mfrac linethickness=\"0\">" + marg(n, 0) + marg(n, 1) + "</mfrac>";
            return "<mfrac>" + marg(n, 0) + marg(n, 1) + "</mfrac>";
        case MathType::SlashedFraction: return mrow(marg(n, 0) + mo("/") + marg(n, 1));
        case MathType::Stack: return "<mfrac linethickness=\"0\">" + marg(n, 0) + marg(n, 1) + "</mfrac>";
        case MathType::FunctionApply: {
            std::string fn = n.args.empty() ? "" : math_to_text(n.args[0]);
            std::string f = (fn.size() > 1 && fn.find_first_not_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos)
                                ? "<mi>" + html_escape(fn) + "</mi>"
                                : marg(n, 0);
            return mrow(f + "<mo>&#x2061;</mo>" + marg(n, 1));
        }
        case MathType::LeftSubSup:
            return "<mmultiscripts>" + marg(n, 2) + "<mprescripts/>" + marg(n, 0) + marg(n, 1) + "</mmultiscripts>";
        case MathType::LowerLimit: return "<munder>" + marg(n, 0) + marg(n, 1) + "</munder>";
        case MathType::UpperLimit: return "<mover>" + marg(n, 0) + marg(n, 1) + "</mover>";
        case MathType::Matrix: {
            int cols = o.column.value_or(1);
            if (cols <= 0) cols = 1;
            std::string s = "<mtable>";
            for (size_t i = 0; i < n.args.size(); ++i) {
                if (i % static_cast<size_t>(cols) == 0) s += (i ? "</mtr><mtr>" : "<mtr>");
                s += "<mtd>" + marg(n, i) + "</mtd>";
            }
            if (!n.args.empty()) s += "</mtr>";
            s += "</mtable>";
            if (o.ch && *o.ch == 0x24A8) return mrow(mo("(") + s + mo(")"));
            if (o.ch && *o.ch == 0x24B1) return mrow(mo("|") + s + mo("|"));
            if (o.ch && *o.ch == 0x24A9) return mrow(mo("‖") + s + mo("‖"));
            return s;
        }
        case MathType::Nary: {
            char32_t opc = o.ch.value_or(0x222B);
            std::string op = mo(cp_str(opc), " largeop=\"true\"");
            bool has_sub = !arg_empty(n, 0), has_sup = !arg_empty(n, 1);
            bool integral = (opc >= 0x222B && opc <= 0x2233);
            std::string head;
            if (integral && (has_sub || has_sup)) {
                if (has_sub && has_sup)
                    head = "<msubsup>" + op + marg(n, 0) + marg(n, 1) + "</msubsup>";
                else if (has_sub)
                    head = "<msub>" + op + marg(n, 0) + "</msub>";
                else
                    head = "<msup>" + op + marg(n, 1) + "</msup>";
            } else if (has_sub && has_sup)
                head = "<munderover>" + op + marg(n, 0) + marg(n, 1) + "</munderover>";
            else if (has_sub)
                head = "<munder>" + op + marg(n, 0) + "</munder>";
            else if (has_sup)
                head = "<mover>" + op + marg(n, 1) + "</mover>";
            else
                head = op;
            return mrow(head + marg(n, 2));
        }
        case MathType::OpChar: return o.ch ? mo(cp_str(*o.ch)) : "";
        case MathType::Overbar: return "<mover>" + marg(n, 0) + mo("‾") + "</mover>";
        case MathType::Underbar: return "<munder>" + marg(n, 0) + mo("_") + "</munder>";
        case MathType::Radical:
            if (arg_empty(n, 0)) return "<msqrt>" + marg(n, 1) + "</msqrt>";
            return "<mroot>" + marg(n, 1) + marg(n, 0) + "</mroot>";
        case MathType::StretchStack: {
            bool below = o.align.value_or(0) == 0;
            std::string c = mo(cp_str(o.ch.value_or(0x23DF)), " stretchy=\"true\"");
            return below ? "<munder>" + marg(n, 0) + c + "</munder>" : "<mover>" + marg(n, 0) + c + "</mover>";
        }
        case MathType::Subscript: return "<msub>" + marg(n, 0) + marg(n, 1) + "</msub>";
        case MathType::Superscript: return "<msup>" + marg(n, 0) + marg(n, 1) + "</msup>";
        case MathType::SubSup: return "<msubsup>" + marg(n, 0) + marg(n, 1) + marg(n, 2) + "</msubsup>";
    }
    std::string s;
    for (size_t i = 0; i < n.args.size(); ++i) s += marg(n, i);
    return s;
}

std::string mml_seq(const MathSeq& seq) {
    std::string s;
    for (const auto& n : seq) s += mml_node(n);
    return s;
}

std::string text_seq(const MathSeq& seq) {
    std::string s;
    for (const auto& n : seq) {
        if (!n.is_object) {
            for (uint32_t cp : utf8_codepoints(n.text))
                if (!is_invisible(cp)) append_utf8(s, cp);
            continue;
        }
        switch (n.obj.type) {
            case MathType::Fraction:
            case MathType::SlashedFraction:
                s += "(" + (n.args.size() > 0 ? text_seq(n.args[0]) : "") + ")/(" +
                     (n.args.size() > 1 ? text_seq(n.args[1]) : "") + ")";
                break;
            case MathType::Superscript:
                s += (n.args.size() > 0 ? text_seq(n.args[0]) : "") + "^" + (n.args.size() > 1 ? text_seq(n.args[1]) : "");
                break;
            case MathType::Subscript:
                s += (n.args.size() > 0 ? text_seq(n.args[0]) : "") + "_" + (n.args.size() > 1 ? text_seq(n.args[1]) : "");
                break;
            case MathType::Radical:
                s += "√(" + (n.args.size() > 1 ? text_seq(n.args[1]) : "") + ")";
                break;
            case MathType::Brackets:
                s += (n.obj.ch ? cp_str(*n.obj.ch) : "(") + (n.args.empty() ? "" : text_seq(n.args[0])) +
                     (n.obj.ch1 ? cp_str(*n.obj.ch1) : ")");
                break;
            case MathType::Nary:
                s += cp_str(n.obj.ch.value_or(0x222B));
                if (n.args.size() > 0 && !is_empty_seq(n.args[0])) s += "_" + text_seq(n.args[0]);
                if (n.args.size() > 1 && !is_empty_seq(n.args[1])) s += "^" + text_seq(n.args[1]);
                if (n.args.size() > 2) s += " " + text_seq(n.args[2]);
                break;
            case MathType::OpChar:
                if (n.obj.ch) s += cp_str(*n.obj.ch);
                break;
            default:
                for (size_t i = 0; i < n.args.size(); ++i) {
                    if (i) s += " ";
                    s += text_seq(n.args[i]);
                }
        }
    }
    return s;
}

}  // namespace

MathSeq parse_math(const std::vector<std::pair<std::string, MathObject>>& segments) {
    Parser p(tokenize(segments));
    return p.parse_all();
}

std::string math_to_latex(const MathSeq& seq) {
    std::string s = latex_seq(seq);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string math_to_mathml(const MathSeq& seq, bool display) {
    return std::string("<math xmlns=\"http://www.w3.org/1998/Math/MathML\"") +
           (display ? " display=\"block\"" : "") + ">" + mrow(mml_seq(seq)) + "</math>";
}

std::string math_to_text(const MathSeq& seq) { return text_seq(seq); }

std::string strip_math_markers(const std::string& s) {
    std::string out;
    for (uint32_t cp : utf8_codepoints(s)) {
        if (cp == kStart || cp == kSep || cp == kEnd || is_invisible(cp)) continue;
        append_utf8(out, cp);
    }
    return out;
}

}  // namespace oneconv::model
