#pragma once
// Text as cells: what popups and the side panel are made of.
#include <string_view>
#include <vector>

#include "font.hpp"
#include "grid.hpp"
#include "utf8.hpp"

namespace blit {

// Display width of UTF-8 text, in cells.
inline u32 width_of(std::string_view s) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        w += cp_width(d.cp);
        i += d.len;
    }
    return w;
}

// The cells of one popup.
struct TextBox {
    std::vector<u32> cells;
    u32 cols = 0, rows = 0;

    void resize(u32 c, u32 r) {
        cols = c;
        rows = r;
        cells.assign(static_cast<size_t>(c) * r, 0);
    }
    // Writes UTF-8 text at (row, col), clipped to the box. Returns the
    // column after it.
    u32 put(Font& font, u32 row, u32 col, std::string_view s, u32 ink, u32 style = 0) {
        if (row >= rows) return col;
        if (style && !font.ensure_style(style)) style = 0;
        const u8* p = reinterpret_cast<const u8*>(s.data());
        u32* line = cells.data() + static_cast<size_t>(row) * cols;
        for (size_t i = 0; i < s.size() && col < cols;) {
            const Decoded d = decode_utf8(p + i, s.size() - i);
            i += d.len;
            const u32 cp = d.cp < 0x20 || d.cp == 0x7F ? ' ' : d.cp;
            if (cp >= 0x300 && cp_width(cp) == 0) {  // (no cell of its own: over the one before)
                if (col > 0) line[col - 1] = (line[col - 1] & ~0xFFFFu) | font.mark(line[col - 1], cp);
                continue;
            }
            // An emoji with more after it (a family joined by zero width
            // joiners, a skin tone, a flag's second letter): all of it read
            // first, and drawn as the colour font's one picture of it, if
            // there is one, in its first's two cells.
            const u32 cg = cp < 0x80 ? cp : font.cell_glyph(cp);
            const bool ri = (cg & (Font::kWide | Font::kJoin)) == (Font::kWide | Font::kJoin);
            if ((cg & Font::kWide) && ((cg & Font::kPict) || ri) && col + 1 < cols) {
                u32 seq[12] = {cp};
                u32 n = 1;
                size_t j = i;
                bool joined = false;
                while (j < s.size() && n < std::size(seq)) {
                    const Decoded e = decode_utf8(p + j, s.size() - j);
                    const u32 c = e.cp, ce = c < 0x80 ? c : font.cell_glyph(c);
                    const bool next_ri = (ce & (Font::kWide | Font::kJoin)) == (Font::kWide | Font::kJoin);
                    if (!(c == 0x200D || c - 0xFE00 < 16 || c - 0x1F3FB < 5 || (joined && (ce & Font::kPict)) || (ri && next_ri && n == 1))) break;
                    joined = c == 0x200D;
                    seq[n++] = c;
                    j += e.len;
                }
                if (n > 1)
                    if (const u32 slot = font.sequence_glyph(seq, n)) {
                        line[col++] = slot | (ink << kCellInkShift);
                        line[col++] = (slot + 1) | (ink << kCellInkShift);
                        i = j;
                        continue;
                    }
            }
            const u32 g = font.glyph(cp, style);
            const u32 slot = g & (Font::kWide - 1);
            line[col++] = slot | (ink << kCellInkShift);
            if ((g & Font::kWide) && col < cols) line[col++] = (slot + 1) | (ink << kCellInkShift);
        }
        return col;
    }
    void select_row(u32 row) {
        if (row >= rows) return;
        for (u32 c = 0; c < cols; ++c) cells[static_cast<size_t>(row) * cols + c] |= kCellSelected;
    }
};

}  // namespace blit
