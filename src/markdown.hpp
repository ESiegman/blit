#pragma once
// Markdown, drawn as cells: headings, paragraphs with their emphasis, code
// and links, lists (and task lists), quotes, tables, code blocks, rules, and
// Mermaid diagrams (mermaid.hpp), laid out to a width. What blit's preview
// tab shows (appmarkdown.cpp); nothing here knows about the window.
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

// The colours a drawn document uses: indices into the preview's palette.
enum MdInk : u8 {
    kMdText,
    kMdHeading1,
    kMdHeading2,
    kMdHeading3,
    kMdHeading,  // the levels below
    kMdCode,
    kMdLink,
    kMdQuote,
    kMdMuted,
    kMdAccent,
    kMdBorder,  // a diagram's boxes, a table's lines
    kMdEdge,    // a diagram's arrows
    kMdLabel,   // what an arrow says
    kMdError,
    kMdSlice0,  // a pie's slices, these and the headings' colours in turn
    kMdSlice1,
    kMdSlice2,
    kMdSlice3,
    kMdInkCount
};
// Backgrounds behind runs or rows (the preview's tones).
enum MdTone : u8 { kMdNoTone, kMdCodeTone, kMdHeaderTone, kMdNodeTone };
// Styles, which the preview maps to the font's.
enum : u8 { kMdBold = 1, kMdItalic = 2 };

struct MdRun {
    std::string text;
    u8 ink = kMdText, style = 0, tone = kMdNoTone;
    std::string link;  // where a click on it goes ("" if nowhere)
    MdRun() = default;
    MdRun(std::string t, u8 i = kMdText, u8 s = 0, u8 tn = kMdNoTone, std::string l = {}) : text(std::move(t)), ink(i), style(s), tone(tn), link(std::move(l)) {}
};
struct MdRow {
    std::vector<MdRun> runs;
    u32 line = 0;          // the line of the source it comes from (from 0)
    u8 fill = kMdNoTone;   // a background for the rest of the row (a code block's)
    u32 fill_from = 0, fill_to = 0;  // the columns it covers, [from, to)
};

// The text drawn `width` columns wide.
std::vector<MdRow> md_render(std::string_view text, u32 width);
// A row as plain text (for the tests): what its runs say.
std::string md_row_text(const MdRow& r);

}  // namespace blit
