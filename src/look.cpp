#include "look.hpp"

#include <vector>

#include "utf8.hpp"

namespace blit {

namespace {

// A chord's parts, as the README writes them: "Cmd", "Shift", "P".
struct Named {
    std::string_view word;
    u32 symbol;
};
const Named kKeys[] = {
    {"Enter", 0x21A9},  {"Return", 0x21A9},   {"Esc", 0x238B},     {"Escape", 0x238B}, {"Tab", 0x21E5},
    {"Backspace", 0x232B}, {"Delete", 0x2326}, {"Up", 0x2191},     {"Down", 0x2193},   {"Left", 0x2190},
    {"Right", 0x2192},  {"PageUp", 0x21DE},   {"PageDown", 0x21DF}, {"Home", 0x2196},  {"End", 0x2198},
};

void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// One chord ("Cmd+Shift+P") in symbols, modifiers in Apple's order (⌃⌥⇧⌘).
std::string chord_symbols(std::string_view chord, Font& font) {
    bool ctrl = false, opt = false, shift = false, cmd = false;
    std::string_view key = chord;
    for (;;) {
        const size_t plus = key.find('+');
        if (plus == std::string_view::npos || plus + 1 >= key.size()) break;  // ("Cmd++" ends in the key +)
        const std::string_view mod = key.substr(0, plus);
        if (mod == "Cmd") cmd = true;
        else if (mod == "Ctrl") ctrl = true;
        else if (mod == "Option" || mod == "Alt") opt = true;
        else if (mod == "Shift") shift = true;
        else break;
        key.remove_prefix(plus + 1);
    }
    std::string out;
    if (ctrl) append_utf8(out, 0x2303);
    if (opt) append_utf8(out, 0x2325);
    if (shift) append_utf8(out, 0x21E7);
    if (cmd) append_utf8(out, 0x2318);
    for (const Named& n : kKeys)
        if (key == n.word) {
            if (font.can_draw(n.symbol)) append_utf8(out, n.symbol);
            else out += key;
            return out;
        }
    out += key;
    return out;
}

}  // namespace

std::string key_symbols(std::string_view keys, Font& font) {
    // (Symbols on a Mac, and only where they can all be drawn: else the words.)
#if !defined(__APPLE__)
    (void)font;
    return std::string(keys);
#else
    static constexpr u32 kMods[] = {0x2318, 0x21E7, 0x2325, 0x2303};
    for (const u32 m : kMods)
        if (!font.can_draw(m)) return std::string(keys);
    std::string out;
    size_t at = 0;
    while (at < keys.size()) {
        // A chord runs to a space ("Ctrl+Tab / Ctrl+Shift+Tab"); a comma
        // at its end ("F4, Shift+F4") is between chords, unless it is the
        // key itself ("Cmd+,").
        size_t end = keys.find(' ', at);
        if (end == std::string_view::npos) end = keys.size();
        std::string_view chord = keys.substr(at, end - at);
        const bool comma = chord.size() > 1 && chord.back() == ',' && chord[chord.size() - 2] != '+';
        if (comma) chord.remove_suffix(1);
        if (!chord.empty()) out += chord_symbols(chord, font);
        if (comma) out += ',';
        if (end < keys.size()) out += ' ';
        at = end + 1;
    }
    return out;
#endif
}

u32 hints_width(std::span<const Hint> hints, Font& font) {
    u32 w = 0;
    for (size_t i = 0; i < hints.size(); ++i) w += (i ? 3 : 0) + width_of(key_symbols(hints[i].first, font)) + 1 + width_of(hints[i].second);
    return w;
}

u32 put_hints(TextBox& box, Font& font, u32 row, u32 col, std::span<const Hint> hints) {
    for (size_t i = 0; i < hints.size(); ++i) {
        if (i) col += 3;
        col = put(box, font, row, col, key_symbols(hints[i].first, font), ink::kKey);
        col = put(box, font, row, col + 1, hints[i].second, ink::kHint);
    }
    return col;
}

std::string fit_text(std::string_view s, u32 cols, bool path) {
    if (width_of(s) <= cols) return std::string(s);
    if (cols == 0) return {};
    const std::string_view dots = "\xE2\x80\xA6";  // …
    const u8* p = reinterpret_cast<const u8*>(s.data());
    if (!path) {
        // Characters from the start while they and the ellipsis fit.
        u32 w = 0;
        size_t i = 0;
        while (i < s.size()) {
            const Decoded d = decode_utf8(p + i, s.size() - i);
            const u32 cw = cp_width(d.cp);
            if (w + cw + 1 > cols) break;
            w += cw;
            i += d.len ? d.len : 1;
        }
        return std::string(s.substr(0, i)) + std::string(dots);
    }
    // A path: its end, where the name is, after the ellipsis.
    std::vector<size_t> starts;  // (each character's first byte)
    for (size_t i = 0; i < s.size();) {
        starts.push_back(i);
        const Decoded d = decode_utf8(p + i, s.size() - i);
        i += d.len ? d.len : 1;
    }
    u32 w = 0;
    size_t from = s.size();
    for (size_t k = starts.size(); k-- > 0;) {
        const Decoded d = decode_utf8(p + starts[k], s.size() - starts[k]);
        const u32 cw = cp_width(d.cp);
        if (w + cw + 1 > cols) break;
        w += cw;
        from = starts[k];
    }
    return std::string(dots) + std::string(s.substr(from));
}

}  // namespace blit
