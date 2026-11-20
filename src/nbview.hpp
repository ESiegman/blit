#pragma once
// A notebook drawn as Jupyter shows one, in the rows the Markdown preview
// draws (markdown.hpp): each cell under the one before, a Markdown cell's
// text drawn as Markdown, a code cell's as a block of code in the
// notebook's language with its "In [n]:" beside it (above it, when the
// view is narrow), and its outputs under it: text, an error's traceback
// in colour, pictures, a table (a pandas DataFrame's HTML), Markdown, JSON;
// a result's "Out[n]:" beside it. A long output is cut. The script is the
// text being edited (notebook.hpp); each row says which of its lines it is
// of (an output's, its cell's first; a blank row between cells, the next
// one's "# %%"). Nothing here knows about the window, or reads a picture.
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "markdown.hpp"
#include "notebook.hpp"

namespace blit {

// A picture's size, as MdPictureFn gives it; and whether it is not known
// yet (it is being read) or cannot be (it is not shown: what stands for it is).
enum NbPicture : u8 { kNbPictureReady, kNbPictureWaiting, kNbPictureFailed };
using NbPictureFn = std::function<NbPicture(const std::string& src, u32 room, u32 want_w, u32 want_h, MdPic* out)>;
// An output's picture (its bytes, and what kind of file they are: "png",
// "jpeg", "svg") kept as a file it can be read from: that file ("" if it
// cannot be).
using NbKeepFn = std::function<std::string(std::string_view bytes, std::string_view kind)>;

struct NbLook {
    u32 width = 80;
    std::string lang = "python";  // the code's (its colours)
    MdCodeFn colour;
    NbPictureFn picture;
    NbKeepFn keep;
    std::string key;  // what else the rows depend on (the pictures known): kept rows are let go of when it changes
};

// An output, read: what is drawn of it, in parts (an HTML output's text
// and its tables), and a result's count (its "Out[n]:"; -1: none).
struct NbPart {
    enum Kind : u8 { kText, kPicture, kTable, kMarkdown } kind = kText;
    std::string text;  // text (with a terminal's escapes), Markdown, or what stands for a picture that cannot be drawn
    u8 ink = kMdText, tone = kMdNoTone;
    std::string src;  // a picture's file
    u32 want_w = 0, want_h = 0;  // and the size it asks for (in points; 0: its own)
    std::vector<std::vector<std::string>> cells;  // a table's (Markdown), its header first
    std::vector<int> align;
    u32 more = 0;  // and how many of its rows were not read (past what is shown)
};
struct NbOutput {
    i64 count = -1;
    std::vector<NbPart> parts;
    std::string plain;  // a table's output: its text/plain too (drawn instead when the table is too wide and it is not)
};

// What one drawing leaves for the next, so that typing in one cell does
// not draw the others again: each cell's outputs read (their pictures
// kept), by the outputs they are; and the rows of each cell's outputs and
// of each Markdown cell, as they were drawn (let go when what they depend
// on changes: the width, NbLook::key).
struct NbCache {
    struct Read {
        std::shared_ptr<const std::string> outputs;  // (held: its address is the key)
        std::vector<NbOutput> read;
    };
    std::map<const std::string*, Read> read;
    std::string look;
    std::map<std::string, std::vector<MdRow>> rows;  // (by "m", "c" and a cell's text, "o" and its outputs)
    std::set<std::string> drawn;  // (the rows the last drawing has: the rest let go of)
};

// The notebook whose script this is, with the outputs in `model`: its rows
// (each row's line, its script's).
std::vector<MdRow> nb_render(std::string_view script, const std::vector<NotebookCell>& model, const NbLook& look, NbCache* cache = nullptr);

// A terminal's text with its colours (SGR escapes: 16, 256 and 24-bit
// colours, bold, italic) as runs of the theme's inks; `ink` where it names
// none. Other escapes are dropped.
std::vector<MdRun> nb_ansi(std::string_view text, u8 ink);

}  // namespace blit
