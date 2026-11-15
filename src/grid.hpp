#pragma once
// Turns the visible part of the document into a grid of cells for the GPU.
// A cell is a u32: bits 0-15 glyph slot, bits 16-20 ink colour (an index
// into the theme's palette), bits 21-23 a wavy underline (0 none, else the
// kind of diagnostic; 5 is not an underline but an indent line down the
// cell's left edge, in the cell's ink), bit 30 "more than a glyph here: the
// cursor or scroll thumb overlaps this cell, or it is underlined or has an
// indent line", bit 31 "selected".
#include <span>
#include <vector>

#include "buffer.hpp"
#include "font.hpp"
#include "highlight.hpp"

namespace blit {

inline constexpr u32 kCellSelected = 0x80000000u;
inline constexpr u32 kCellOverlay = 0x40000000u;  // the shader has more to do here than draw a glyph
inline constexpr u32 kCellInkShift = 16;  // ink colour index goes here
inline constexpr u32 kCellDecoShift = 21;  // underline kind goes here
inline constexpr u32 kCellDecoMask = 7u << kCellDecoShift;
inline constexpr u32 kCellHover = 1u << 24;  // in a popup: the mouse is over it (a fainter background than selected)
// In a popup whose lit runs are drawn as rounded boxes: a run starts in this
// cell, and one ends in it. (The renderer sets them: see mark_runs().)
inline constexpr u32 kCellRunStart = 1u << 25, kCellRunEnd = 1u << 26;
// In the text: which corners of the cell the selection's outline turns at,
// top left, top right, bottom left, bottom right (see round_selection()).
inline constexpr u32 kCellCornerShift = 24;

// Palette entries with a fixed meaning. Syntax colours use the ones below.
inline constexpr u32 kInkSyntaxEnd = 18;
inline constexpr u32 kInkGuide = 18;       // + level: the indent lines' colours
inline constexpr u32 kInkGuides = 6;
inline constexpr u32 kDecoGuide = 5;       // the "underline kind" that is an indent line
inline constexpr u32 kDecoMatch = 6;       // and the one that is the background of a search match
inline constexpr u32 kDecoSelect = 7;      // not drawn as such: the cells of the line's text that it covers are shown selected
inline constexpr u32 kInkDiagnostic = 24;  // + severity - 1: error, warning, information, hint
inline constexpr u32 kInkMuted = 28;
inline constexpr u32 kInkAccent = 29;
inline constexpr u32 kInkAdded = 30;    // lines added since the last commit
inline constexpr u32 kInkChanged = 31;  // lines changed since then (lines removed use the errors' colour)

// A wavy underline on one line, over display columns [col0, col1).
struct GridDeco {
    u64 line = 0;
    u32 col0 = 0, col1 = 0;
    u32 kind = 0;  // 1 error, 2 warning, 3 information, 4 hint
};

// Cells to show on a line from a display column on (a diagnostic's message,
// after the end of the line's text).
struct GridNote {
    u64 line = 0;
    u32 col = 0;
    const u32* cells = nullptr;
    u32 count = 0;
};

struct GridSpec {
    u64 top_line = 0;   // document line shown in grid row 0
    u32 first_col = 0;  // display column shown in grid column 0
    u32 cols = 0;
    u32 rows = 0;
    u32 lead = 0;  // blank grid columns before the text (left padding)
    u64 revision = 0;  // of the text; lets caches notice edits
    u64 doc = 0;       // which text: caches kept from frame to frame must not mix two up
    u32 tab_width = 4;
    // Syntax highlighting: style runs, and the cell bits for each style id:
    // an ink (<< kCellInkShift) and, for italic or bold, a font style
    // (<< Font::kStyleShift) the font has said yes to. Both null when there
    // is no highlighting.
    const Highlights* highlights = nullptr;
    const u32* style_bits = nullptr;
    // Indent lines, drawn by guide_grid(): columns per level of
    // indentation (0: none) and how many of the kInkGuide inks to cycle
    // through.
    u32 guide_step = 0;
    u32 guide_inks = 0;
    // Underlines and notes, laid over the text by decorate_grid().
    std::span<const GridDeco> decos;
    std::span<const GridNote> notes;
    // Selection as (line, display column) pairs; has_sel false when empty.
    bool has_sel = false;
    u64 sel_line0 = 0, sel_line1 = 0;
    u32 sel_col0 = 0, sel_col1 = 0;
};

// Finding display column N of a line means walking the line from its start,
// because tabs and wide characters make columns and bytes differ. For a line
// millions of columns long that walk would dominate every frame, so the
// renderer remembers a (column, byte offset) anchor every 65536 columns of the
// long lines it has drawn and resumes from the nearest one.
struct LineAnchors {
    static constexpr u32 kStep = 1u << 16;
    u64 line = ~0ull;
    u64 used = 0;  // last-use stamp, for recycling
    std::vector<u32> cols;
    std::vector<u64> offs;
};

class AnchorCache {
public:
    // Anchors hold absolute offsets, so any edit drops them all, and so
    // does another document.
    void sync(u64 doc, u64 revision);
    LineAnchors* find(u64 line);
    LineAnchors& claim(u64 line);  // finds, or recycles the stalest entry

private:
    static constexpr int kLines = 128;
    u64 revision_ = ~0ull, doc_ = ~0ull;
    u64 clock_ = 0;
    LineAnchors lines_[kLines];
};

// Writes rows * cols cells. `anchors` is optional.
void fill_grid(const Buffer& buf, Font& font, const GridSpec& g, u32* cells, AnchorCache* anchors = nullptr);

// How far each line on screen is indented, kept from frame to frame: it
// changes only when the text or the top line does.
struct GuideCache {
    u64 doc = ~0ull, revision = ~0ull, top_line = ~0ull;
    u32 rows = 0, tab_width = 0;
    // Per row: columns of white space the line starts with, or -1 for a
    // line with nothing else in it. `next` is the same for the nearest line
    // below that has text.
    std::vector<i32> indent, next;
    i32 above = -1, below = -1;  // the nearest lines with text off the top and the bottom
};
// Marks the cells that carry an indent line. Call after fill_grid() and
// before decorate_grid(): an underline takes a cell over.
void guide_grid(const Buffer& buf, const GridSpec& g, u32* cells, GuideCache& cache);
// Lays the spec's underlines and notes over cells that fill_grid() made.
void decorate_grid(const GridSpec& g, u32* cells);
// Marks the corners at which the outline of what is selected turns, for
// the shader that rounds them. Call last. Only the rows the selection is on
// are looked at, and of those only the cells at the ends of its runs.
void round_selection(const GridSpec& g, u32* cells);
// Marks where each run of lit cells (selected, or under the mouse) in a
// popup's rows starts and ends.
void mark_runs(u32* cells, u32 cols, u32 rows);

}  // namespace blit
