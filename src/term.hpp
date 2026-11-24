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
// line-drawing characters, window titles and the working directory (OSC 0,
// 2 and 7), the modes in which a program asks for the mouse (9, 1000,
// 1002, 1003, and 1006 for how it is told), and pictures: sixel, and the
// kitty graphics protocol. Its lines are kept as the program wrote them: a
// change of width wraps them again.
#include <deque>
#include <map>
#include <memory>
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
        kWrapped = 512,   // (a row's last cell) the line went on into the next row: one line, not two
        kPad = 1024,      // (a row's last cell) left blank where a wide character did not fit: not part of the line
    };
    bool operator==(const TermCell&) const = default;
};

// A picture a program put on the screen (sixel, or kitty's protocol): its
// pixels, and the cell its top left is in, on a line counted from the first
// the terminal ever had (so that it goes up with the text as it scrolls).
struct TermPicture {
    u64 serial = 0;  // its own number: a new one for each picture put there
    std::shared_ptr<const std::vector<u8>> rgba;  // w * h pixels, colours multiplied by alpha
    u32 w = 0, h = 0;
    u64 line = 0;            // (on the alternate screen: its row)
    u32 col = 0;
    u32 cols = 0, rows = 0;  // the cells it covers
    bool fit = false;        // drawn to fill those cells (kitty's c and r), not at its own size
    u32 off_x = 0, off_y = 0;  // pixels into its first cell (kitty's X and Y)
    bool alt = false;        // on the alternate screen
    u32 image = 0, placement = 0;  // kitty's numbers for it
};

class Term {
public:
    // What reads a PNG (kitty's f=100) into pixels, colours multiplied by
    // alpha; set by whoever has a reader (none: such a picture is refused).
    using PictureReader = bool (*)(std::string_view bytes, std::vector<u8>* rgba, u32* w, u32* h);
    static void set_picture_reader(PictureReader r);
    explicit Term(u32 cols = 80, u32 rows = 24, size_t scrollback = 10000);

    void resize(u32 cols, u32 rows);
    // A cell's size in pixels, as it is drawn: what a picture's size is
    // counted in, and what a program that asks is told.
    void set_cell_px(u32 w, u32 h) { cell_w_ = std::max(w, 1u), cell_h_ = std::max(h, 1u); }
    void feed(std::string_view bytes);
    // What the terminal says back to the program (answers to its questions:
    // where the cursor is, what kind of terminal this is), taken once.
    std::string take_reply();

    u32 cols() const { return cols_; }
    u32 rows() const { return rows_; }
    // The screen, row 0 at the top.
    const TermCell& at(u32 row, u32 col) const { return screen()[row][col]; }
    // The lines scrolled off its top, the oldest first (none on the alternate screen).
    size_t history() const { return alt_ ? 0 : history_.size(); }
    const std::vector<TermCell>& history_line(size_t i) const { return history_[i]; }
    void clear_history();
    // The pictures on the screen and in the history, oldest first; how many
    // lines have gone off the top of the history (a picture's line, less
    // this, is its row in the history and the screen together); and a count
    // that changes whenever a picture comes or goes.
    const std::vector<TermPicture>& pictures() const { return pictures_; }
    u64 lines_gone() const { return lines_gone_; }
    u64 pictures_generation() const { return pictures_gen_; }

    u32 cursor_row() const { return row_; }
    u32 cursor_col() const { return std::min(col_, cols_ - 1); }
    bool cursor_visible() const { return cursor_visible_; }
    bool alt_screen() const { return alt_; }
    bool app_cursor_keys() const { return app_cursor_; }
    bool bracketed_paste() const { return bracketed_paste_; }
    // The mouse, as the program asked for it: 0 not at all; 9 presses; 1000
    // presses and releases; 1002 and moves while a button is held; 1003 and
    // every move. And whether it is told in SGR's form (1006) or X10's.
    u32 mouse_mode() const { return mouse_mode_; }
    bool mouse_sgr() const { return mouse_sgr_; }
    // The pen the program writes with (its colours and attributes) as it is
    // now, and set: text of blit's own put among the program's (a serial
    // tab's times) is written with a pen of its own, and the program's put back.
    const TermCell& pen() const { return pen_; }
    void set_pen(const TermCell& p) { pen_ = p; }
    const std::string& title() const { return title_; }
    const std::string& cwd() const { return cwd_; }  // from OSC 7, a path
    u64 bells() const { return bells_; }
    u64 generation() const { return generation_; }  // counts up with every change

    // A row as text (trailing blanks dropped): of the screen, and of the history.
    std::string row_text(u32 row) const;
    static std::string line_text(const TermCell* cells, u32 n);

private:
    // A screen is its rows, each a line of cells of its own: scrolling moves
    // rows, not cells, and a row scrolled into the history goes there whole.
    using Rows = std::vector<std::vector<TermCell>>;
    Rows& screen() { return alt_ ? alt_cells_ : cells_; }
    const Rows& screen() const { return alt_ ? alt_cells_ : cells_; }
    TermCell blank() const;  // an erased cell: the current background
    TermCell* line(u32 r) { return screen()[r].data(); }

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
    void reflow(u32 cols, u32 rows, u32* cur_row, u32* cur_col, bool* pending);
    void dcs();  // a device control string (sixel), and an application program command (kitty's pictures)
    void apc();
    void sixel(std::string_view data);
    void kitty(std::string_view control, std::string payload);
    void place(TermPicture p, bool move_cursor, bool kitty_cursor);
    void forget_pictures(bool alt, u64 from_line, u64 to_line);  // those on the screen given (and on lines [from, to) on the main one)
    void history_pop();
    void move_to(i64 r, i64 c);
    void reset();
    int param(size_t i, int otherwise) const;

    u32 cols_, rows_;
    size_t scrollback_;
    Rows cells_, alt_cells_;
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
    u32 mouse_mode_ = 0;
    bool mouse_sgr_ = false;

    // Pictures, and kitty's images (sent once, put on the screen as often as wanted), by number.
    std::vector<TermPicture> pictures_;
    struct KittyImage {
        std::shared_ptr<const std::vector<u8>> rgba;
        u32 w = 0, h = 0;
        u64 used = 0;  // (when it was last put on the screen: the least used go first)
    };
    std::map<u32, KittyImage> kitty_images_;
    std::string kitty_control_, kitty_payload_;  // (a picture coming in parts: its first part's keys, and its data so far)
    u64 lines_gone_ = 0, pictures_gen_ = 0, next_serial_ = 1, kitty_used_ = 0;
    u32 cell_w_ = 8, cell_h_ = 16;

    // The parser.
    enum State : u8 { kGround, kEscape, kEscapeMore, kCsi, kCsiIgnore, kOsc, kOscEscape, kString, kStringEscape, kDcs, kDcsEscape, kApc, kApcEscape };
    State state_ = kGround;
    std::vector<int> params_;   // -1: not given
    std::vector<u8> sub_;       // with each parameter, whether it came after ':' (a sub-parameter)
    bool param_open_ = false;
    u8 private_ = 0, intermediate_ = 0;
    u8 designate_ = 0;          // ESC ( and the like: which set comes next
    std::string osc_;
    std::string dcs_;  // (a device control string, or an application program command, as it comes)
    u32 utf8_cp_ = 0;
    int utf8_left_ = 0;

    std::string reply_, title_, cwd_;
    u64 bells_ = 0, generation_ = 0;
};

// How many columns a character takes: 2 for East Asian wide ones and most
// emoji, 0 for combining marks, 1 for the rest.
int term_width(u32 cp);

}  // namespace blit
