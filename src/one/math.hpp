// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// OneNote equations: OneNote stores math as text runs containing structure
// markers (U+FDD0 start, U+FDEE argument separator, U+FDEF end) and a
// per-run "math inline object" describing the structure (fraction, n-ary,...).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oneconv::model {

enum class MathType : uint32_t {
    SimpleText = 0,
    Accent = 10,
    Box = 11,
    BoxedFormula = 12,
    Brackets = 13,
    BracketsWithSeps = 14,
    EquationArray = 15,
    Fraction = 16,
    FunctionApply = 17,
    LeftSubSup = 18,
    LowerLimit = 19,
    Matrix = 20,
    Nary = 21,
    OpChar = 22,
    Overbar = 23,
    Phantom = 24,
    Radical = 25,
    SlashedFraction = 26,
    Stack = 27,
    StretchStack = 28,
    Subscript = 29,
    SubSup = 30,
    Superscript = 31,
    Underbar = 32,
    UpperLimit = 33,
    PlainText = 0x90000000u,
};

struct MathObject {
    MathType type = MathType::SimpleText;
    uint32_t argc = 0;
    std::optional<uint8_t> column;
    std::optional<uint8_t> align;
    std::optional<char32_t> ch;
    std::optional<char32_t> ch1;
    std::optional<char32_t> ch2;
};

struct MathNode;
using MathSeq = std::vector<MathNode>;

struct MathNode {
    bool is_object = false;
    std::string text;  // UTF-8, when !is_object
    MathObject obj;
    std::vector<MathSeq> args;
};

/// Build a math tree from (run text, object) segments. Throws std::runtime_error
/// when the marker structure is inconsistent.
MathSeq parse_math(const std::vector<std::pair<std::string, MathObject>>& segments);

std::string math_to_latex(const MathSeq& seq);
std::string math_to_mathml(const MathSeq& seq, bool display);
/// Linear plain-text rendering (markers removed).
std::string math_to_text(const MathSeq& seq);
/// Remove OneNote math markers from a raw string.
std::string strip_math_markers(const std::string& s);

}  // namespace oneconv::model
