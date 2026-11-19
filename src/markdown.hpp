#pragma once
// Markdown, drawn as cells: headings, paragraphs with their emphasis, code
// and links, lists (and task lists), quotes, tables, code blocks, rules, and
// Mermaid diagrams (as pictures: diagram.hpp), laid out to a width, with
// room left for pictures. What blit's preview tab shows (appmarkdown.cpp);
// nothing here knows about the window, or reads a picture.
#include <functional>
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
    // A code block in a language with a grammar: its kinds of token, a few
    // inks between all the captures a grammar has.
    kMdCodeKeyword,
    kMdCodeString,
    kMdCodeComment,
    kMdCodeFunction,
    kMdCodeType,
    kMdCodeConstant,  // numbers, booleans, constants
    kMdCodeOperator,  // operators and punctuation
    kMdCodePlain,     // the rest of such a block
    // The colours a terminal's text names (a notebook's outputs), as the
    // theme has them: red is kMdError, black and white the text's own.
    kMdAnsiGreen,
    kMdAnsiYellow,
    kMdAnsiBlue,
    kMdAnsiMagenta,
    kMdAnsiCyan,
    kMdInkCount
};
static_assert(kMdInkCount <= 32);  // (a popup's palette)
// Backgrounds behind runs or rows (the preview's tones; an error's in a notebook).
enum MdTone : u8 { kMdNoTone, kMdCodeTone, kMdHeaderTone, kMdNodeTone, kMdErrorTone };
// Styles, which the preview maps to the font's.
enum : u8 { kMdBold = 1, kMdItalic = 2, kMdStrike = 4 };

struct MdRun {
    std::string text;
    u8 ink = kMdText, style = 0, tone = kMdNoTone;
    std::string link;  // where a click on it goes ("" if nowhere)
    MdRun() = default;
    MdRun(std::string t, u8 i = kMdText, u8 s = 0, u8 tn = kMdNoTone, std::string l = {}) : text(std::move(t)), ink(i), style(s), tone(tn), link(std::move(l)) {}
};
// A picture: the room it takes, in cells, and its size in pixels in that room.
struct MdPic {
    std::string src;   // the file, as the text names it
    std::string link;  // where a click on it goes
    u32 col = 0;       // its first column
    u32 part = 0;      // which of its rows a row is (from 0)
    u32 cols = 0, rows = 0, w = 0, h = 0;
};
struct MdRow {
    std::vector<MdRun> runs;
    u32 line = 0;          // the line of the source it comes from (from 0)
    u8 fill = kMdNoTone;   // a background for the rest of the row (a code block's)
    u32 fill_from = 0, fill_to = 0;  // the columns it covers, [from, to)
    std::vector<MdPic> pics;  // the pictures over it, each a row of its own
};

// The size of a picture that is a paragraph of its own (or one of a row
// of them), at most `room` columns wide; want_w and want_h are what the
// text asks for (<img width=...>, in points; 0: nothing). False if it is
// not known (not there, not read yet): it is shown as text then.
using MdPictureFn = std::function<bool(const std::string& src, u32 room, u32 want_w, u32 want_h, MdPic* out)>;

// The colours of a fenced code block: `lang` is the word after the fence,
// `code` its lines, each ended by a newline. Out: an ink for each byte of
// it. False when the language is not one there is a grammar for: the block
// stays in kMdCode.
using MdCodeFn = std::function<bool(std::string_view lang, std::string_view code, std::vector<u8>* inks)>;

// A Mermaid diagram (a ```mermaid block's text) drawn as a picture at
// most `room` columns wide. kMdDiagramPicture: drawn, its size in `out`
// (its src the picture's name); kMdDiagramError: a mistake in it, why in
// `note` (its code is shown, under that; a kind not drawn says so there
// too); kMdDiagramLater: not drawn yet (its code is shown until it is).
enum MdDiagram : u8 { kMdDiagramLater, kMdDiagramPicture, kMdDiagramError };
using MdDiagramFn = std::function<u8(std::string_view source, u32 room, MdPic* out, std::string* note)>;

// The text drawn `width` columns wide.
std::vector<MdRow> md_render(std::string_view text, u32 width, const MdPictureFn& picture = {}, const MdCodeFn& code = {}, const MdDiagramFn& diagram = {});
// Code drawn as a fenced block of it in `lang` is (without the language's
// name over it), and a table as one in Markdown is, its first row the
// header; align, for each column: -1 left, 0 centre, 1 right. The table's
// cells are Markdown (md_escape: text that says only itself).
std::vector<MdRow> md_code(std::string_view lang, std::string_view code, u32 width, const MdCodeFn& colour = {});
std::vector<MdRow> md_table(const std::vector<std::vector<std::string>>& cells, const std::vector<int>& align, u32 width);
std::string md_escape(std::string_view text);
// A row as plain text (for the tests): what its runs say.
std::string md_row_text(const MdRow& r);

}  // namespace blit
