#pragma once
// How every box looks, in one place: the spacing every box takes its
// numbers from (App::look() turns these points into pixels), the inks of
// each role (indices into a box's palette, so a change of theme needs no
// cells drawn again), keys written as macOS writes them, and hint lines.
//
// The rules (README, "Looks"):
// - Text starts kLookEdgePt from a box's edge, or the window's, in every
//   box and bar. A lit row's rounded bar reaches to kLookInsetPt from the
//   box's edge, and a floating box is rounded by the bar's radius plus that
//   inset, so both curves keep one centre.
// - One left edge for the text of a box: its title, input, rows and footer.
// - The accent is for "you are here, or yours": the caret, matched letters,
//   the current item, the active segment, the one button that does it.
//   Headings are muted bold; secondary text muted; keys in hints plain.
// - Nothing is cut off without an ellipsis (fit_text).
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "font.hpp"
#include "grid.hpp"
#include "textbox.hpp"

namespace blit {

inline constexpr float kLookEdgePt = 11;   // a box's edge (or the window's) to its text
inline constexpr float kLookInsetPt = 4;   // a box's edge to the bar behind a lit row
inline constexpr float kLookGapPt = 3;     // a floating box to what it belongs to
inline constexpr u32 kLookPageCols = 72;   // a whole-page column (the start screen, New project)
inline constexpr u32 kLookPickerCols = 80; // the widest a picker (Go to file, the command search) grows

// An ink and a face, by what the text is for.
struct Ink {
    u32 ink = 0, style = 0;
};
namespace ink {
inline constexpr Ink kText{0, 0};                            // what a row is: a file's name, a command
inline constexpr Ink kStrong{0, Font::kBold};                // the name that matters most (the file in the status bar)
inline constexpr Ink kHeading{kInkMuted, Font::kBold};       // a box's or a section's title
inline constexpr Ink kMuted{kInkMuted, 0};                   // secondary: paths, counts, descriptions, placeholders
inline constexpr Ink kAccent{kInkAccent, 0};                 // you are here: the current item, the key that does it
inline constexpr Ink kAccentStrong{kInkAccent, Font::kBold}; // the active segment, matched letters, the one button
inline constexpr Ink kKey{0, 0};                             // a key in a hint line
inline constexpr Ink kHint{kInkMuted, 0};                    // what that key does
inline constexpr Ink kError{kInkDiagnostic, 0};
inline constexpr Ink kWarning{kInkDiagnostic + 1, 0};
inline constexpr Ink kInfo{kInkDiagnostic + 2, 0};
inline constexpr Ink kAdded{kInkAdded, 0};                   // git: a new line, an added file
inline constexpr Ink kChanged{kInkChanged, 0};               // git: a changed line, a modified file
inline constexpr Ink kRemoved{kInkDiagnostic, 0};            // git: a removed line, a deleted file
}  // namespace ink

// Text written into a box in an ink's role. Returns the column after it.
inline u32 put(TextBox& box, Font& font, u32 row, u32 col, std::string_view s, Ink in) { return box.put(font, row, col, s, in.ink, in.style); }

// Keys as macOS writes them ("Cmd+Shift+P" becomes "⇧⌘P", "Ctrl+`" "⌃`",
// "Enter" "↩"), where the font (or a fallback) can draw the symbols; in
// words elsewhere (key_words()). Several chords ("Ctrl+Tab / Ctrl+Shift+Tab", "F4,
// Shift+F4") keep what is between them.
std::string key_symbols(std::string_view keys, Font& font);
// Keys written as for a Mac ("Cmd+Option+B") in the words of the system
// blit runs on: as they are on a Mac, Ctrl and Alt elsewhere ("Ctrl+Alt+B").
std::string key_words(std::string_view keys);

// A hint line: each pair is the keys (written by key_symbols) and what they
// do: the keys plain, what they do muted, three columns between pairs.
using Hint = std::pair<std::string_view, std::string_view>;
u32 hints_width(std::span<const Hint> hints, Font& font);
u32 put_hints(TextBox& box, Font& font, u32 row, u32 col, std::span<const Hint> hints);

// Text cut to at most `cols` columns, an ellipsis at the end where it was
// cut ("render.c…"), or at the start for a path ("…/blit/src").
std::string fit_text(std::string_view s, u32 cols, bool path = false);
// Text in lines of at most `cols` columns, broken between words (a word
// longer than a line cut as fit_text cuts it).
std::vector<std::string> wrap_words(std::string_view text, u32 cols);

}  // namespace blit
