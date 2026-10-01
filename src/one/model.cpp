// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
#include "model.hpp"

#include <cstdio>

#include "../util/text.hpp"

namespace oneconv::model {

std::string Color::hex() const {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}

bool NoteTag::is_checkbox() const {
    switch (shape) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12:
        case 28: case 30: case 32: case 48: case 50: case 52: case 69: case 71: case 73:
        case 94: case 95: case 96: case 97: case 98: case 99:
            return true;
        default:
            return false;
    }
}

std::string NoteTag::symbol() const {
    if (is_checkbox()) return completed ? "☑" : "☐";  // ☑ ☐
    switch (shape) {
        case 13: case 40: case 61: return "⭐";             // star
        case 14: case 89: case 90: case 91: case 92: case 93: return "\U0001F6A9";  // flag
        case 15: case 111: return "❓";                     // question
        case 16: case 59: case 80: return "➡️";       // right arrow
        case 17: case 115: return "❗";                     // high priority
        case 18: case 118: return "\U0001F4C7";                 // contact
        case 19: case 120: return "\U0001F4C5";                 // meeting / calendar
        case 20: case 117: return "⏰";                     // time sensitive / bell
        case 21: return "\U0001F4A1";                           // idea
        case 22: return "\U0001F4CC";                           // pushpin
        case 23: return "\U0001F3E0";                           // home
        case 24: return "\U0001F4AC";                           // comment
        case 25: return "\U0001F642";                           // smiley
        case 26: return "\U0001F397️";                     // award
        case 27: return "\U0001F511";                           // key
        case 35: case 55: case 76: return "✔️";       // check mark
        case 47: case 68: case 88: return "❌";             // x
        case 106: case 107: case 108: return "✉️";    // envelope
        case 109: case 110: return "\U0001F4F1";                // phone
        case 112: return "\U0001F4CE";                          // paperclip
        case 113: return "\U0001F641";                          // frown
        case 116: return "\U0001F465";                          // people
        case 119: return "\U0001F339";                          // rose
        case 121: return "\U0001F3B5";                          // music
        case 122: return "\U0001F3AC";                          // movie
        case 123: return "❝";                              // quote
        case 124: case 125: return "\U0001F310";                // globe
        case 126: return "\U0001F4BB";                          // laptop
        case 127: return "✈️";                        // plane
        case 128: return "\U0001F697";                          // car
        case 129: return "\U0001F52D";                          // binoculars
        case 130: return "\U0001F4CA";                          // presentation
        case 131: return "\U0001F512";                          // padlock
        case 132: case 133: return "\U0001F4D6";                // book
        case 134: return "\U0001F4C4";                          // paper
        case 135: return "\U0001F50E";                          // research
        case 136: return "\U0001F58A️";                    // pen
        case 137: case 138: return "\U0001F4B2";                // money
        case 139: return "\U0001F5D3️";                    // scheduled task
        case 140: return "⚡";                              // lightning
        case 141: return "☁️";                        // cloud
        case 142: return "❤️";                        // heart
        case 143: return "\U0001F33B";                          // sunflower
        case 0: return "";
        default: return "●";                               // generic marker
    }
}

std::string Paragraph::plain_text() const {
    std::string s;
    for (const auto& in : inlines) {
        switch (in.kind) {
            case Inline::Kind::Text:
            case Inline::Kind::Math: s += in.text; break;
            case Inline::Kind::Break: s += "\n"; break;
            case Inline::Kind::Ink:
                if (in.ink) s += in.ink->recognized_text;
                break;
        }
    }
    return s;
}

bool Paragraph::empty() const {
    for (const auto& in : inlines) {
        if (in.kind == Inline::Kind::Math || in.kind == Inline::Kind::Ink) return false;
        if (in.kind == Inline::Kind::Text && !trim(in.text).empty()) return false;
    }
    return tags.empty();
}

int Paragraph::heading_level() const {
    if (style_id.size() == 2 && style_id[0] == 'h' && style_id[1] >= '1' && style_id[1] <= '6')
        return style_id[1] - '0';
    return 0;
}

std::optional<float> PageItem::x() const {
    switch (kind) {
        case Kind::Outline: return outline ? outline->x : std::nullopt;
        case Kind::Image: return image ? image->x : std::nullopt;
        case Kind::Attachment: return attachment ? attachment->x : std::nullopt;
        case Kind::Ink: return ink ? ink->offset_x : std::nullopt;
    }
    return std::nullopt;
}

std::optional<float> PageItem::y() const {
    switch (kind) {
        case Kind::Outline: return outline ? outline->y : std::nullopt;
        case Kind::Image: return image ? image->y : std::nullopt;
        case Kind::Attachment: return attachment ? attachment->y : std::nullopt;
        case Kind::Ink: return ink ? ink->offset_y : std::nullopt;
    }
    return std::nullopt;
}

}  // namespace oneconv::model
