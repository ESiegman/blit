#pragma once
// A terminal: the screen a program running under a pseudo-terminal draws
// on, as an xterm draws it. Bytes from the program go in (feed); what is on
// the screen, the lines scrolled off its top, the cursor and the modes the
// program has set come out. Nothing here knows about windows or processes
// (appterm.cpp shows it, and runs the program).
//
// What it understands: UTF-8 text with wide characters, the C0 controls,
// the escape and CSI sequences xterm-compatible programs use (cursor
// movement, erasing, inserting and deleting, scrolling regions, colours
// in 16, 256 and 24 bits, bold, italic, underline, inverse), the
// alternate screen, bracketed paste, application cursor keys, DEC's
// line-drawing characters, and window titles and the working directory
// (OSC 0, 2 and 7). Mouse reporting is not.
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

// A colour: the default, one of 256, or red-green-blue.
inline constexpr u32 kTermDefault = 0;
inline constexpr u32 kTermIndexed = 1u << 24;  // | index (0-255)
inline constexpr u32 kTermRgb = 2u << 24;      // | 0xRRGGBB

struct TermCell {
    u32 cp = ' ';
    u32 fg = kTermDefault, bg = kTermDefault;
    u16 attr = 0;
    enum : u16 {
        kBold = 1,
        kDim = 2,
        kItalic = 4,
        kUnderline = 8,
        kInverse = 16,
        kHidden = 32,
        kStrike = 64,
        kWide = 128,      // the left half of a character two columns wide
        kWideTail = 256,  // and its right half (no character of its own)
    };
    bool operator==(const TermCell&) const = default;
};

class Term {
public:
    explicit Term(u32 cols = 80, u32 rows = 24, size_t scrollback = 10000);

    void resize(u32 cols, u32 rows);
    void feed(std::string_view bytes);
    // What the terminal says back to the program (answers to its questions:
    // where the cursor is, what kind of terminal this is), taken once.
    std::string take_reply();

    u32 cols() const { return cols_; }
    u32 rows() const { return rows_; }
    // The screen, row 0 at the top.
    const TermCell& at(u32 row, u32 col) const { return screen()[static_cast<size_t>(row) * cols_ + col]; }
    // The lines scrolled off its top, the oldest first (none on the alternate screen).
    size_t history() const { return alt_ ? 0 : history_.size(); }
    const std::vector<TermCell>& history_line(size_t i) const { return history_[i]; }
    void clear_history() { history_.clear(), ++generation_; }

    u32 cursor_row() const { return row_; }
    u32 cursor_col() const { return std::min(col_, cols_ - 1); }
    bool cursor_visible() const { return cursor_visible_; }
    bool alt_screen() const { return alt_; }
    bool app_cursor_keys() const { return app_cursor_; }
    bool bracketed_paste() const { return bracketed_paste_; }
    const std::string& title() const { return title_; }
    const std::string& cwd() const { return cwd_; }  // from OSC 7, a path
    u64 bells() const { return bells_; }
    u64 generation() const { return generation_; }  // counts up with every change

    // A row as text (trailing blanks dropped): of the screen, and of the history.
    std::string row_text(u32 row) const;
    static std::string line_text(const TermCell* cells, u32 n);

private:
    std::vector<TermCell>& screen() { return alt_ ? alt_cells_ : cells_; }
    const std::vector<TermCell>& screen() const { return alt_ ? alt_cells_ : cells_; }
    TermCell blank() const;  // an erased cell: the current background
    TermCell* line(u32 r) { return screen().data() + static_cast<size_t>(r) * cols_; }

    void print(u32 cp);
    void control(u8 c);
    void escape(u8 final);
    void csi(u8 final);
    void osc();
    void sgr();
    void set_mode(bool on);
    void index();          // down a line, scrolling at the bottom margin
    void reverse_index();  // up a line, scrolling at the top margin
    void scroll_up(u32 top, u32 bottom, u32 n);
    void scroll_down(u32 top, u32 bottom, u32 n);
    void erase(u32 r, u32 c0, u32 c1);  // [c0, c1) on row r
    void move_to(i64 r, i64 c);
    void reset();
    int param(size_t i, int otherwise) const;

    u32 cols_, rows_;
    size_t scrollback_;
    std::vector<TermCell> cells_, alt_cells_;
    std::deque<std::vector<TermCell>> history_;
    bool alt_ = false;

    // The cursor, the pen it writes with, and what is saved of them.
    u32 row_ = 0, col_ = 0;
    bool wrap_next_ = false;  // the last column was just written: the next character starts a new line
    TermCell pen_;
    struct Saved {
        u32 row = 0, col = 0;
        TermCell pen;
        bool graphics = false;
    } saved_, alt_saved_;
    u32 top_ = 0, bottom_ = 0;  // the scrolling region, rows [top, bottom]
    std::vector<bool> tabs_;
    bool graphics_ = false;     // G0 is DEC's line-drawing set
    bool autowrap_ = true, insert_ = false, origin_ = false;
    bool cursor_visible_ = true, app_cursor_ = false, bracketed_paste_ = false;

    // The parser.
    enum State : u8 { kGround, kEscape, kEscapeMore, kCsi, kCsiIgnore, kOsc, kOscEscape, kString, kStringEscape };
    State state_ = kGround;
    std::vector<int> params_;   // -1: not given
    std::vector<u8> sub_;       // with each parameter, whether it came after ':' (a sub-parameter)
    bool param_open_ = false;
    u8 private_ = 0, intermediate_ = 0;
    u8 designate_ = 0;          // ESC ( and the like: which set comes next
    std::string osc_;
    u32 utf8_cp_ = 0;
    int utf8_left_ = 0;

    std::string reply_, title_, cwd_;
    u64 bells_ = 0, generation_ = 0;
};

// How many columns a character takes: 2 for East Asian wide ones and most
// emoji, 0 for combining marks, 1 for the rest.
int term_width(u32 cp);

}  // namespace blit
