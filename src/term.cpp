#include "term.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

#include "base64.hpp"

namespace blit {

namespace {

Term::PictureReader g_picture_reader = nullptr;
// The longest string kept (a picture's data comes as one), and the most a
// terminal keeps of pictures: their count, and the bytes of kitty's images.
constexpr size_t kStringMost = 64u << 20, kPicturesMost = 256, kKittyBytesMost = 128u << 20;
constexpr u32 kPictureSideMost = 8192;

}  // namespace

void Term::set_picture_reader(PictureReader r) { g_picture_reader = r; }

int term_width(u32 cp) {
    if (cp < 0x300) return 1;
    // Combining marks and zero-width characters.
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFE0F || cp == 0xFE0E)
        return 0;
    // East Asian wide and full-width, and emoji.
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
        (cp >= 0x1F680 && cp <= 0x1F6FF) || (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x1FA70 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

namespace {

// DEC's special graphics: the line-drawing characters ESC ( 0 selects.
u32 graphics_char(u32 c) {
    static const u16 kMap[] = {0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
                               0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7};
    return c >= 0x60 && c <= 0x7E ? kMap[c - 0x60] : c;
}

}  // namespace

Term::Term(u32 cols, u32 rows, size_t scrollback) : cols_(std::max(cols, 1u)), rows_(std::max(rows, 1u)), scrollback_(scrollback) {
    cells_.assign(rows_, std::vector<TermCell>(cols_));
    alt_cells_ = cells_;
    bottom_ = rows_ - 1;
    tabs_.assign(cols_, false);
    for (u32 c = 8; c < cols_; c += 8) tabs_[c] = true;
}

TermCell Term::blank() const {
    TermCell b;
    b.bg = pen_.bg;
    return b;
}

void Term::reset() {
    pen_ = TermCell{};
    row_ = col_ = 0;
    wrap_next_ = false;
    top_ = 0;
    bottom_ = rows_ - 1;
    graphics_ = insert_ = origin_ = app_cursor_ = bracketed_paste_ = mouse_sgr_ = false;
    mouse_mode_ = 0;
    autowrap_ = cursor_visible_ = true;
    if (alt_) alt_ = false;
    for (std::vector<TermCell>& r : cells_) std::ranges::fill(r, TermCell{});
    for (std::vector<TermCell>& r : alt_cells_) std::ranges::fill(r, TermCell{});
    tabs_.assign(cols_, false);
    for (u32 c = 8; c < cols_; c += 8) tabs_[c] = true;
    if (!pictures_.empty() || !kitty_images_.empty()) ++pictures_gen_;
    pictures_.clear();
    kitty_images_.clear();
    kitty_control_.clear(), kitty_payload_.clear();
    ++generation_;
}

void Term::clear_history() {
    lines_gone_ += history_.size();  // (a line's number stays what it was)
    history_.clear();
    forget_pictures(false, 0, lines_gone_);
    ++generation_;
}

// The oldest line of the history goes (and the pictures that were wholly above it).
void Term::history_pop() {
    history_.pop_front();
    ++lines_gone_;
    if (!pictures_.empty()) forget_pictures(false, 0, lines_gone_);
}

// The pictures wholly on lines [from, to) of the main screen and its
// history, or (alt) all those on the alternate screen.
void Term::forget_pictures(bool alt, u64 from, u64 to) {
    const size_t had = pictures_.size();
    std::erase_if(pictures_, [&](const TermPicture& p) { return alt ? p.alt : !p.alt && p.line >= from && p.line + std::max(p.rows, 1u) <= to; });
    if (pictures_.size() != had) ++pictures_gen_;
}

void Term::resize(u32 cols, u32 rows) {
    cols = std::max(cols, 1u);
    rows = std::max(rows, 1u);
    if (cols == cols_ && rows == rows_) return;
    // (The main screen's cursor is in alt_saved_ while the alternate screen is up.)
    u32& main_row = alt_ ? alt_saved_.row : row_;
    u32& main_col = alt_ ? alt_saved_.col : col_;
    auto remake = [&](Rows& cells, bool keep_history, u32* cur_row) {
        // Fewer rows: the top ones go (into the history on the main
        // screen), so that the cursor's line stays on show.
        const u32 drop = rows < rows_ && *cur_row >= rows ? *cur_row - rows + 1 : 0;
        for (u32 r = 0; r < drop && keep_history; ++r) {
            history_.push_back(std::move(cells[r]));
            if (history_.size() > scrollback_) history_pop();
        }
        Rows out;
        out.reserve(rows);
        for (u32 r = drop; r < rows_ && out.size() < rows; ++r) out.push_back(std::move(cells[r]));
        while (out.size() < rows) out.emplace_back(cols);
        for (std::vector<TermCell>& r : out) r.resize(cols);
        cells = std::move(out);
        *cur_row -= std::min(*cur_row, drop);
    };
    if (cols != cols_) {  // a new width: the main screen's lines (and the history's) wrapped again
        bool pending = !alt_ && wrap_next_;
        reflow(cols, rows, &main_row, &main_col, &pending);
        if (!alt_) wrap_next_ = pending;
    } else {
        remake(cells_, true, &main_row);
    }
    u32 alt_row = alt_ ? row_ : 0;
    remake(alt_cells_, false, &alt_row);  // (a full-screen program draws its screen again at the new size)
    if (alt_) row_ = alt_row;
    if (cols == cols_ || alt_) wrap_next_ = false;
    row_ = std::min(row_, rows - 1);
    col_ = std::min(col_, cols - 1);
    cols_ = cols;
    rows_ = rows;
    top_ = 0;
    bottom_ = rows_ - 1;
    tabs_.assign(cols_, false);
    for (u32 c = 8; c < cols_; c += 8) tabs_[c] = true;
    saved_.row = std::min(saved_.row, rows_ - 1), saved_.col = std::min(saved_.col, cols_ - 1);
    alt_saved_.row = std::min(alt_saved_.row, rows_ - 1), alt_saved_.col = std::min(alt_saved_.col, cols_ - 1);
    ++generation_;
}

namespace {

bool plain(const TermCell& c) { return c.cp == ' ' && c.fg == kTermDefault && c.bg == kTermDefault && (c.attr & ~(TermCell::kWrapped | TermCell::kPad)) == 0; }

}  // namespace

// The main screen's lines and the history's, wrapped again at a new width
// (and laid out on `rows` rows): rows that a line went on across (kWrapped)
// are one line again, cut into rows of the new width, a wide character
// that would straddle the edge going whole to the next. The row at the top
// of the screen stays there unless the cursor's line would leave the
// screen, so nothing comes back from the history; blank rows below the
// cursor are not kept. The cursor goes with the character it was on.
void Term::reflow(u32 cols, u32 rows, u32* cur_row, u32* cur_col, bool* pending) {
    const size_t hist = history_.size(), cur_src = hist + std::min(*cur_row, rows_ - 1);
    auto src = [&](size_t i) -> const std::vector<TermCell>& { return i < hist ? history_[i] : cells_[i - hist]; };
    size_t last = cur_src;  // (the last row with anything on it, a picture, or the cursor)
    for (size_t i = hist + rows_; i-- > cur_src + 1;)
        if (!std::ranges::all_of(src(i), plain)) {
            last = i;
            break;
        }
    for (const TermPicture& p : pictures_)
        if (!p.alt && p.line >= lines_gone_) last = std::max<size_t>(last, std::min<size_t>(p.line - lines_gone_ + std::max(p.rows, 1u) - 1, hist + rows_ - 1));
    std::vector<u32> lands(pictures_.empty() ? 0 : last + 1);  // (where each row's first cell lands: a picture's row goes there)
    std::deque<std::vector<TermCell>> out;
    std::vector<TermCell> row;
    row.reserve(cols);
    size_t anchor = 0, cr = 0;
    u32 cc = 0;
    auto end_row = [&](bool wrapped) {
        if (wrapped && row.size() < cols) {  // (a wide character that did not fit: a blank of the row's own)
            TermCell pad;
            pad.attr = TermCell::kPad;
            row.push_back(pad);
        }
        row.resize(cols);
        if (wrapped) row[cols - 1].attr |= TermCell::kWrapped;
        out.push_back(std::move(row));
        row = {};
        row.reserve(cols);
    };
    for (size_t i = 0; i <= last; ++i) {
        const std::vector<TermCell>& r = src(i);
        const u32 n = static_cast<u32>(r.size());
        const bool wrapped = n && (r[n - 1].attr & TermCell::kWrapped) && i < last;
        // What of the row is the line's: all of it but a pad where it went on; up to its
        // last character where it ends (and to the cursor, on the cursor's row).
        u32 len = n;
        if (wrapped) {
            if (r[n - 1].attr & TermCell::kPad) --len;
        } else {
            while (len > 0 && plain(r[len - 1])) --len;
        }
        const bool at_cursor = i == cur_src;
        if (at_cursor) len = std::max(len, std::min(*cur_col, n ? n - 1 : 0) + 1);
        if (i == hist) anchor = out.size() + (row.size() >= cols ? 1 : 0);
        if (!lands.empty()) lands[i] = static_cast<u32>(out.size() + (row.size() >= cols ? 1 : 0));
        for (u32 c = 0; c < len; ++c) {
            TermCell cell = c < n ? r[c] : TermCell{};
            cell.attr &= static_cast<u16>(~(TermCell::kWrapped | TermCell::kPad));
            if (cell.attr & TermCell::kWideTail) continue;  // (with its head)
            u32 w = cell.attr & TermCell::kWide ? 2 : 1;
            if (w == 2 && cols < 2) cell.attr &= static_cast<u16>(~TermCell::kWide), w = 1;
            if (row.size() + w > cols) end_row(true);
            if (at_cursor && (c == *cur_col || (w == 2 && c + 1 == *cur_col))) cr = out.size(), cc = static_cast<u32>(row.size()) + (c + 1 == *cur_col && w == 2 ? 1 : 0);
            row.push_back(cell);
            if (w == 2) {
                TermCell tail = c + 1 < n && (r[c + 1].attr & TermCell::kWideTail) ? r[c + 1] : cell;
                tail.attr = static_cast<u16>((tail.attr & ~(TermCell::kWide | TermCell::kWrapped | TermCell::kPad)) | TermCell::kWideTail);
                row.push_back(tail);
            }
        }
        if (!wrapped) end_row(false);
    }
    // (A cursor waiting to wrap stays after its character, if there is room there now.)
    if (*pending && cc + 1 < cols) ++cc, *pending = false;
    const size_t n_out = out.size();
    size_t top = std::max(anchor, n_out > rows ? n_out - rows : 0);
    top = std::min(top, cr);
    if (cr + 1 > rows) top = std::max(top, cr + 1 - rows);
    const size_t from = top > scrollback_ ? top - scrollback_ : 0;  // (what is past what the history keeps goes)
    if (!pictures_.empty()) {
        for (TermPicture& p : pictures_)
            if (!p.alt && p.line >= lines_gone_ && p.line - lines_gone_ <= last) p.line = lines_gone_ + lands[p.line - lines_gone_];
        ++pictures_gen_;
    }
    lines_gone_ += from;
    if (!pictures_.empty()) forget_pictures(false, 0, lines_gone_);
    history_.clear();
    for (size_t k = from; k < top; ++k) history_.push_back(std::move(out[k]));
    Rows screen;
    screen.reserve(rows);
    for (size_t k = top; k < n_out && screen.size() < rows; ++k) screen.push_back(std::move(out[k]));
    while (screen.size() < rows) screen.emplace_back(cols);
    cells_ = std::move(screen);
    *cur_row = static_cast<u32>(cr - top);
    *cur_col = std::min(cc, cols - 1);
}

std::string Term::take_reply() {
    std::string r;
    r.swap(reply_);
    return r;
}

// ---------------------------------------------------------------- reading what the program writes

void Term::feed(std::string_view bytes) {
    ++generation_;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const u8 b = static_cast<u8>(bytes[i]);
        // A run of plain characters (most of what programs write): straight
        // onto the line, without the parser.
        if (state_ == kGround && utf8_left_ == 0 && b >= 0x20 && b < 0x7F && !graphics_ && !insert_) {
            size_t j = i;
            while (j < bytes.size() && static_cast<u8>(bytes[j]) >= 0x20 && static_cast<u8>(bytes[j]) < 0x7F) ++j;
            TermCell c = pen_;
            c.attr = static_cast<u16>(pen_.attr & ~(TermCell::kWide | TermCell::kWideTail));
            for (; i < j; ++i) {
                if (wrap_next_) {
                    if (autowrap_) {
                        line(row_)[cols_ - 1].attr |= TermCell::kWrapped;
                        col_ = 0;
                        index();
                    }
                    wrap_next_ = false;
                }
                TermCell* l = line(row_);
                if (l[col_].attr & (TermCell::kWide | TermCell::kWideTail)) {  // (half of a wide one: the careful way)
                    print(static_cast<u8>(bytes[i]));
                    continue;
                }
                c.cp = static_cast<u8>(bytes[i]);
                l[col_] = c;
                if (++col_ >= cols_) {
                    col_ = cols_ - 1;
                    wrap_next_ = true;
                }
            }
            --i;
            continue;
        }
        if (state_ >= kOsc) {
            // A picture's string (sixel, kitty's): taken a run at a time, to the ESC that ends it.
            if (state_ == kDcs || state_ == kApc) {
                const void* esc = std::memchr(bytes.data() + i, 0x1B, bytes.size() - i);
                const size_t end = esc ? static_cast<size_t>(static_cast<const char*>(esc) - bytes.data()) : bytes.size();
                if (dcs_.size() <= kStringMost) dcs_.append(bytes.data() + i, std::min(end - i, kStringMost + 1 - dcs_.size()));
                if (esc) state_ = state_ == kDcs ? kDcsEscape : kApcEscape;
                i = end;  // (the ESC, or the end: the loop goes past it)
                continue;
            }
            if (state_ == kDcsEscape || state_ == kApcEscape) {
                const bool device = state_ == kDcsEscape;
                state_ = kGround;
                if (b == '\\') {  // (ST)
                    if (dcs_.size() <= kStringMost) device ? dcs() : apc();
                    if (dcs_.capacity() > (1u << 20)) std::string().swap(dcs_);  // (a big picture's: let go of)
                    dcs_.clear();
                    continue;
                }
                dcs_.clear();  // (another sequence began: the string is dropped, and the ESC is that one's)
                state_ = kEscape;
                intermediate_ = 0;
            } else if (state_ == kOsc || state_ == kString) {
                // Strings (a title, and what is not understood) run to BEL or ESC \.
                if (b == 0x07) {
                    if (state_ == kOsc) osc();
                    state_ = kGround;
                } else if (b == 0x1B) {
                    state_ = state_ == kOsc ? kOscEscape : kStringEscape;
                } else if (state_ == kOsc && osc_.size() < 4096) {
                    osc_ += static_cast<char>(b);
                }
                continue;
            } else {
                if (state_ == kOscEscape) osc();
                state_ = kGround;
                if (b == '\\') continue;  // (ST)
            }
        }
        // UTF-8 in the middle of a character.
        if (utf8_left_ > 0) {
            if ((b & 0xC0) == 0x80) {
                utf8_cp_ = utf8_cp_ << 6 | (b & 0x3F);
                if (--utf8_left_ == 0) print(utf8_cp_);
                continue;
            }
            utf8_left_ = 0;
            print(0xFFFD);
        }
        if (b == 0x1B) {  // a sequence starts (and any under way is dropped)
            state_ = kEscape;
            intermediate_ = 0;
            continue;
        }
        if (b < 0x20 || b == 0x7F) {
            if (b != 0x7F) control(b);  // (inside a sequence too, as a VT does)
            continue;
        }
        switch (state_) {
            case kGround:
                if (b < 0x80) print(graphics_ ? graphics_char(b) : b);
                else if ((b & 0xE0) == 0xC0) utf8_cp_ = b & 0x1F, utf8_left_ = 1;
                else if ((b & 0xF0) == 0xE0) utf8_cp_ = b & 0x0F, utf8_left_ = 2;
                else if ((b & 0xF8) == 0xF0) utf8_cp_ = b & 0x07, utf8_left_ = 3;
                else print(0xFFFD);
                break;
            case kEscape:
                if (b == '[') {
                    state_ = kCsi;
                    params_.clear();
                    sub_.clear();
                    param_open_ = false;
                    private_ = intermediate_ = 0;
                } else if (b == ']') {
                    state_ = kOsc;
                    osc_.clear();
                } else if (b == 'P' || b == '_') {
                    state_ = b == 'P' ? kDcs : kApc;  // DCS, APC: a sixel picture, kitty's (anything else is passed over when it ends)
                    dcs_.clear();
                } else if (b == 'X' || b == '^') {
                    state_ = kString;  // SOS, PM: not understood, passed over
                } else if (b >= 0x20 && b <= 0x2F) {
                    designate_ = b;
                    state_ = kEscapeMore;
                } else {
                    state_ = kGround;
                    escape(b);
                }
                break;
            case kEscapeMore:
                if (b >= 0x20 && b <= 0x2F) break;
                if (designate_ == '(') graphics_ = b == '0';  // G0: line drawing, or not
                state_ = kGround;
                break;
            case kCsi:
            case kCsiIgnore:
                if (b >= '0' && b <= '9') {
                    if (!param_open_) {
                        params_.push_back(0);
                        sub_.push_back(0);
                        param_open_ = true;
                    }
                    int& p = params_.back();
                    if (p < 0) p = 0;
                    p = std::min(p * 10 + (b - '0'), 100000);
                } else if (b == ';' || b == ':') {
                    if (!param_open_) {
                        params_.push_back(-1);
                        sub_.push_back(0);
                    }
                    param_open_ = false;
                    if (b == ':') {  // the next is a sub-parameter of this one
                        params_.push_back(-1);
                        sub_.push_back(1);
                        param_open_ = true;
                        params_.back() = -1;
                    }
                } else if (b >= 0x3C && b <= 0x3F) {
                    if (params_.empty()) private_ = b;
                    else state_ = kCsiIgnore;
                } else if (b >= 0x20 && b <= 0x2F) {
                    intermediate_ = b;
                } else if (b >= 0x40 && b <= 0x7E) {
                    if (state_ == kCsi) csi(b);
                    state_ = kGround;
                } else {
                    state_ = kCsiIgnore;
                }
                break;
            default: state_ = kGround; break;
        }
    }
}

int Term::param(size_t i, int otherwise) const {
    // (Sub-parameters are not counted as parameters.)
    size_t k = 0;
    for (size_t j = 0; j < params_.size(); ++j) {
        if (sub_[j]) continue;
        if (k++ == i) return params_[j] < 0 ? otherwise : params_[j];
    }
    return otherwise;
}

void Term::control(u8 c) {
    switch (c) {
        case 0x07: ++bells_; break;
        case 0x08:
            if (col_ > 0) --col_;
            wrap_next_ = false;
            break;
        case 0x09: {
            u32 c2 = std::min(col_ + 1, cols_ - 1);
            while (c2 < cols_ - 1 && !tabs_[c2]) ++c2;
            col_ = c2;
            wrap_next_ = false;
            break;
        }
        case 0x0A:
        case 0x0B:
        case 0x0C: index(); break;
        case 0x0D:
            col_ = 0;
            wrap_next_ = false;
            break;
        case 0x0E:
        case 0x0F: break;  // (G1 is not used)
        case 0x18:
        case 0x1A: state_ = kGround; break;
        default: break;
    }
}

void Term::print(u32 cp) {
    int w = term_width(cp);
    if (w == 0) return;  // (a combining mark: dropped; the character before it stands)
    if (w == 2 && cols_ < 2) w = 1;  // (a screen one column wide has no room for both halves)
    if (wrap_next_) {
        if (autowrap_) {
            line(row_)[cols_ - 1].attr |= TermCell::kWrapped;
            col_ = 0;
            index();
        }
        wrap_next_ = false;
    }
    if (w == 2 && col_ + 1 >= cols_) {  // (no room for both halves: on the next line)
        if (autowrap_) {
            erase(row_, col_, cols_);
            line(row_)[cols_ - 1].attr |= TermCell::kWrapped | TermCell::kPad;
            col_ = 0;
            index();
        } else {
            col_ = cols_ >= 2 ? cols_ - 2 : 0;
        }
    }
    TermCell* l = line(row_);
    if (insert_) {
        std::move_backward(l + col_, l + cols_ - w, l + cols_);
    }
    // Writing over half of a wide character leaves the other half blank.
    if (l[col_].attr & TermCell::kWideTail && col_ > 0) l[col_ - 1] = blank();
    if (l[col_].attr & TermCell::kWide && col_ + 1 < cols_) l[col_ + 1] = blank();
    TermCell c = pen_;
    c.cp = cp;
    c.attr = static_cast<u16>(pen_.attr & ~(TermCell::kWide | TermCell::kWideTail));
    if (w == 2) {
        c.attr |= TermCell::kWide;
        l[col_] = c;
        TermCell tail = c;
        tail.cp = ' ';
        tail.attr = static_cast<u16>((c.attr & ~TermCell::kWide) | TermCell::kWideTail);
        if (col_ + 2 < cols_ && (l[col_ + 2].attr & TermCell::kWideTail)) l[col_ + 2] = blank();
        l[col_ + 1] = tail;
    } else {
        l[col_] = c;
    }
    col_ += static_cast<u32>(w);
    if (col_ >= cols_) {
        col_ = cols_ - 1;
        wrap_next_ = true;
    }
}

void Term::index() {
    if (row_ == bottom_) scroll_up(top_, bottom_, 1);
    else if (row_ + 1 < rows_) ++row_;
    wrap_next_ = false;  // (a line filled to its end and then a new line is one line, not two)
}

void Term::reverse_index() {
    if (row_ == top_) scroll_down(top_, bottom_, 1);
    else if (row_ > 0) --row_;
    wrap_next_ = false;
}

void Term::scroll_up(u32 top, u32 bottom, u32 n) {
    n = std::min(n, bottom - top + 1);
    Rows& s = screen();
    const TermCell b = blank();
    for (u32 k = 0; k < n; ++k) {
        std::vector<TermCell> gone = std::move(s[top]);
        std::move(s.begin() + top + 1, s.begin() + bottom + 1, s.begin() + top);
        // A line off the top of the whole main screen is kept; the oldest
        // kept, when there are as many as are kept, makes the new blank line.
        if (!alt_ && top == 0) {
            history_.push_back(std::move(gone));
            gone = {};
            if (history_.size() > scrollback_) {
                gone = std::move(history_.front());
                history_pop();
            }
        }
        gone.assign(cols_, b);
        s[bottom] = std::move(gone);
    }
}

void Term::scroll_down(u32 top, u32 bottom, u32 n) {
    n = std::min(n, bottom - top + 1);
    Rows& s = screen();
    const TermCell b = blank();
    for (u32 k = 0; k < n; ++k) {
        std::vector<TermCell> gone = std::move(s[bottom]);
        std::move_backward(s.begin() + top, s.begin() + bottom, s.begin() + bottom + 1);
        gone.assign(cols_, b);
        s[top] = std::move(gone);
    }
}

void Term::erase(u32 r, u32 c0, u32 c1) {
    TermCell* l = line(r);
    c1 = std::min(c1, cols_);
    if (c0 > 0 && c0 < c1 && (l[c0].attr & TermCell::kWideTail)) l[c0 - 1] = blank();  // (half a wide character goes whole)
    if (c1 < cols_ && c1 > c0 && (l[c1].attr & TermCell::kWideTail)) l[c1] = blank();
    std::fill(l + c0, l + std::max(c0, c1), blank());
}

void Term::move_to(i64 r, i64 c) {
    const i64 lo = origin_ ? top_ : 0, hi = origin_ ? bottom_ : rows_ - 1;
    if (origin_) r += top_;
    row_ = static_cast<u32>(std::clamp<i64>(r, lo, hi));
    col_ = static_cast<u32>(std::clamp<i64>(c, 0, cols_ - 1));
    wrap_next_ = false;
}

void Term::escape(u8 final) {
    switch (final) {
        case 'D': index(); break;
        case 'E':
            col_ = 0;
            index();
            break;
        case 'M': reverse_index(); break;
        case '7': saved_ = {row_, col_, pen_, graphics_}; break;
        case '8':
            row_ = std::min(saved_.row, rows_ - 1);
            col_ = std::min(saved_.col, cols_ - 1);
            pen_ = saved_.pen;
            graphics_ = saved_.graphics;
            wrap_next_ = false;
            break;
        case 'H':
            if (col_ < cols_) tabs_[col_] = true;
            break;
        case 'c': reset(); break;
        default: break;  // (= and > keypad modes, and the rest)
    }
}

void Term::csi(u8 final) {
    const int n1 = std::max(1, param(0, 1));
    if (private_ == '?' && (final == 'h' || final == 'l')) return set_mode(final == 'h');
    if (private_) {  // (a marked sequence is not the plain one of the same final: what is not handled here is dropped)
        if (final == 'c' && (private_ == '>' || private_ == '=')) reply_ += "\x1b[>0;0;0c";  // (secondary device attributes)
        if (final == 'S' && private_ == '?' && param(1, 0) == 1) {  // XTSMGRAPHICS: sixel's colours, and the most a picture may be
            char buf[64];
            if (param(0, 0) == 1) std::snprintf(buf, sizeof buf, "\x1b[?1;0;256S");
            else if (param(0, 0) == 2) std::snprintf(buf, sizeof buf, "\x1b[?2;0;%u;%uS", std::min(cols_ * cell_w_, kPictureSideMost), std::min(rows_ * cell_h_, kPictureSideMost));
            else std::snprintf(buf, sizeof buf, "\x1b[?%d;1;0S", param(0, 0));
            reply_ += buf;
        }
        return;
    }
    switch (intermediate_) {
        case 0: break;
        case ' ': return;  // (DECSCUSR, the cursor's shape: drawn as it is)
        default: return;
    }
    TermCell* l = line(row_);
    switch (final) {
        case 'A': move_to(row_ - static_cast<i64>(n1) - (origin_ ? top_ : 0), col_); break;
        case 'B':
        case 'e': move_to(row_ + static_cast<i64>(n1) - (origin_ ? top_ : 0), col_); break;
        case 'C':
        case 'a': move_to(row_ - (origin_ ? top_ : 0), col_ + static_cast<i64>(n1)); break;
        case 'D': move_to(row_ - (origin_ ? top_ : 0), static_cast<i64>(std::min(col_, cols_ - 1)) - n1); break;
        case 'E': move_to(row_ + static_cast<i64>(n1) - (origin_ ? top_ : 0), 0); break;
        case 'F': move_to(row_ - static_cast<i64>(n1) - (origin_ ? top_ : 0), 0); break;
        case 'G':
        case '`': move_to(row_ - (origin_ ? top_ : 0), n1 - 1); break;
        case 'H':
        case 'f': move_to(param(0, 1) - 1, param(1, 1) - 1); break;
        case 'd': move_to(n1 - 1, col_); break;
        case 'J': {
            const int how = param(0, 0);
            if (how == 0) {
                erase(row_, std::min(col_, cols_ - 1), cols_);
                for (u32 r = row_ + 1; r < rows_; ++r) erase(r, 0, cols_);
            } else if (how == 1) {
                for (u32 r = 0; r < row_; ++r) erase(r, 0, cols_);
                erase(row_, 0, std::min(col_, cols_ - 1) + 1);
            } else if (how == 2) {
                for (u32 r = 0; r < rows_; ++r) erase(r, 0, cols_);
                if (!pictures_.empty()) forget_pictures(alt_, lines_gone_ + history_.size(), ~0ull);
            } else if (how == 3) {
                lines_gone_ += history_.size();
                history_.clear();
                if (!pictures_.empty()) forget_pictures(false, 0, lines_gone_);
            }
            break;
        }
        case 'K': {
            const int how = param(0, 0);
            const u32 c = std::min(col_, cols_ - 1);
            if (how == 0) erase(row_, c, cols_);
            else if (how == 1) erase(row_, 0, c + 1);
            else if (how == 2) erase(row_, 0, cols_);
            break;
        }
        case 'L':
            if (row_ >= top_ && row_ <= bottom_) scroll_down(row_, bottom_, static_cast<u32>(n1));
            col_ = 0;
            break;
        case 'M':
            if (row_ >= top_ && row_ <= bottom_) {
                // (Deleting lines inside the region never feeds the history.)
                const u32 n = std::min<u32>(static_cast<u32>(n1), bottom_ - row_ + 1);
                Rows& s = screen();
                const TermCell b = blank();
                for (u32 k = 0; k < n; ++k) {
                    std::vector<TermCell> gone = std::move(s[row_]);
                    std::move(s.begin() + row_ + 1, s.begin() + bottom_ + 1, s.begin() + row_);
                    gone.assign(cols_, b);
                    s[bottom_] = std::move(gone);
                }
            }
            col_ = 0;
            break;
        case 'P': {
            const u32 c = std::min(col_, cols_ - 1), n = std::min<u32>(static_cast<u32>(n1), cols_ - c);
            std::move(l + c + n, l + cols_, l + c);
            std::fill(l + cols_ - n, l + cols_, blank());
            break;
        }
        case '@': {
            const u32 c = std::min(col_, cols_ - 1), n = std::min<u32>(static_cast<u32>(n1), cols_ - c);
            std::move_backward(l + c, l + cols_ - n, l + cols_);
            std::fill(l + c, l + c + n, blank());
            break;
        }
        case 'X': erase(row_, std::min(col_, cols_ - 1), std::min(col_, cols_ - 1) + static_cast<u32>(n1)); break;
        case 'S': scroll_up(top_, bottom_, static_cast<u32>(n1)); break;
        case 'T': scroll_down(top_, bottom_, static_cast<u32>(n1)); break;
        case 'b': {  // the last character, again
            const u32 c = col_ > 0 ? col_ - 1 : 0;
            const u32 cp = line(row_)[c].cp;
            for (int k = 0; k < std::min(n1, 10000); ++k) print(cp);
            break;
        }
        case 'm': sgr(); break;
        case 'r': {
            const u32 t = static_cast<u32>(std::max(1, param(0, 1))) - 1, b = static_cast<u32>(std::max(1, param(1, static_cast<int>(rows_)))) - 1;
            if (t < b && b < rows_) {
                top_ = t;
                bottom_ = b;
                move_to(0, 0);
            }
            break;
        }
        case 's': saved_ = {row_, col_, pen_, graphics_}; break;
        case 'u':
            row_ = std::min(saved_.row, rows_ - 1);
            col_ = std::min(saved_.col, cols_ - 1);
            pen_ = saved_.pen;
            wrap_next_ = false;
            break;
        case 'g':
            if (param(0, 0) == 0 && col_ < cols_) tabs_[col_] = false;
            else if (param(0, 0) == 3) std::ranges::fill(tabs_, false);
            break;
        case 'h':
        case 'l':
            if (param(0, 0) == 4) insert_ = final == 'h';
            break;
        case 'n':
            if (param(0, 0) == 5) reply_ += "\x1b[0n";
            else if (param(0, 0) == 6) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "\x1b[%u;%uR", row_ + 1 - (origin_ ? top_ : 0), std::min(col_, cols_ - 1) + 1);
                reply_ += buf;
            }
            break;
        case 'c':
            if (param(0, 0) == 0) reply_ += "\x1b[?62;4;22c";  // a VT220 with sixel pictures and colour
            break;
        case 't': {  // the size of the text area in characters (18) or pixels (14), and of a cell (16)
            char buf[48];
            const int what = param(0, 0);
            if (what == 18) std::snprintf(buf, sizeof buf, "\x1b[8;%u;%ut", rows_, cols_);
            else if (what == 14) std::snprintf(buf, sizeof buf, "\x1b[4;%u;%ut", rows_ * cell_h_, cols_ * cell_w_);
            else if (what == 16) std::snprintf(buf, sizeof buf, "\x1b[6;%u;%ut", cell_h_, cell_w_);
            else break;  // (moving and sizing the window: not the program's to do)
            reply_ += buf;
            break;
        }
        default: break;
    }
}

void Term::set_mode(bool on) {
    for (size_t i = 0; i < params_.size(); ++i) {
        if (sub_[i]) continue;
        switch (params_[i]) {
            case 1: app_cursor_ = on; break;
            case 6:
                origin_ = on;
                move_to(0, 0);
                break;
            case 7: autowrap_ = on; break;
            case 25: cursor_visible_ = on; break;
            case 47:
            case 1047:
            case 1049:
                if (on == alt_) break;
                if (params_[i] == 1049 && on) alt_saved_ = {row_, col_, pen_, graphics_};
                alt_ = on;
                if (!pictures_.empty()) forget_pictures(true, 0, 0);
                if (on)
                    for (std::vector<TermCell>& r : alt_cells_) std::ranges::fill(r, TermCell{});
                if (params_[i] == 1049 && !on) {
                    row_ = std::min(alt_saved_.row, rows_ - 1);
                    col_ = std::min(alt_saved_.col, cols_ - 1);
                    pen_ = alt_saved_.pen;
                    graphics_ = alt_saved_.graphics;
                }
                top_ = 0;
                bottom_ = rows_ - 1;
                wrap_next_ = false;
                break;
            case 2004: bracketed_paste_ = on; break;
            case 9:
            case 1000:
            case 1002:
            case 1003:
                if (on) mouse_mode_ = static_cast<u32>(params_[i]);
                else if (mouse_mode_ == static_cast<u32>(params_[i])) mouse_mode_ = 0;
                break;
            case 1006: mouse_sgr_ = on; break;
            default: break;  // (focus events, blinking, other ways of telling the mouse: not done)
        }
    }
}

void Term::sgr() {
    if (params_.empty()) {
        pen_ = TermCell{};
        return;
    }
    for (size_t i = 0; i < params_.size(); ++i) {
        if (sub_[i]) continue;
        const int p = params_[i] < 0 ? 0 : params_[i];
        // The parameters after this one: its sub-parameters (38:2::r:g:b), or the next ones (38;2;r;g;b).
        auto colour = [&](u32* into) {
            std::vector<int> rest;
            size_t j = i + 1;
            const bool colons = j < params_.size() && sub_[j];
            for (; j < params_.size() && (colons ? sub_[j] != 0 : true) && rest.size() < 6; ++j) rest.push_back(params_[j]);
            if (rest.empty()) return;
            if (rest[0] == 5 && rest.size() >= 2) {
                *into = kTermIndexed | static_cast<u32>(std::clamp(rest[1], 0, 255));
                i += colons ? rest.size() : 2;
            } else if (rest[0] == 2) {
                // 38;2;r;g;b, or 38:2:id:r:g:b (a colour space first), or 38:2:r:g:b.
                const size_t at = colons && rest.size() >= 5 ? rest.size() - 3 : 1;
                if (rest.size() < at + 3) return;
                auto v = [&](size_t k) { return static_cast<u32>(std::clamp(rest[k] < 0 ? 0 : rest[k], 0, 255)); };
                *into = kTermRgb | v(at) << 16 | v(at + 1) << 8 | v(at + 2);
                i += colons ? rest.size() : 4;
            }
        };
        if (p == 0) pen_ = TermCell{};
        else if (p == 1) pen_.attr |= TermCell::kBold;
        else if (p == 2) pen_.attr |= TermCell::kDim;
        else if (p == 3) pen_.attr |= TermCell::kItalic;
        else if (p == 4 || p == 21) pen_.attr |= TermCell::kUnderline;
        else if (p == 7) pen_.attr |= TermCell::kInverse;
        else if (p == 8) pen_.attr |= TermCell::kHidden;
        else if (p == 9) pen_.attr |= TermCell::kStrike;
        else if (p == 22) pen_.attr &= static_cast<u16>(~(TermCell::kBold | TermCell::kDim));
        else if (p == 23) pen_.attr &= static_cast<u16>(~TermCell::kItalic);
        else if (p == 24) pen_.attr &= static_cast<u16>(~TermCell::kUnderline);
        else if (p == 27) pen_.attr &= static_cast<u16>(~TermCell::kInverse);
        else if (p == 28) pen_.attr &= static_cast<u16>(~TermCell::kHidden);
        else if (p == 29) pen_.attr &= static_cast<u16>(~TermCell::kStrike);
        else if (p >= 30 && p <= 37) pen_.fg = kTermIndexed | static_cast<u32>(p - 30);
        else if (p == 38) colour(&pen_.fg);
        else if (p == 39) pen_.fg = kTermDefault;
        else if (p >= 40 && p <= 47) pen_.bg = kTermIndexed | static_cast<u32>(p - 40);
        else if (p == 48) colour(&pen_.bg);
        else if (p == 49) pen_.bg = kTermDefault;
        else if (p >= 90 && p <= 97) pen_.fg = kTermIndexed | static_cast<u32>(p - 90 + 8);
        else if (p >= 100 && p <= 107) pen_.bg = kTermIndexed | static_cast<u32>(p - 100 + 8);
    }
}

void Term::osc() {
    const size_t semi = osc_.find(';');
    const std::string what = osc_.substr(0, semi), text = semi == std::string::npos ? std::string() : osc_.substr(semi + 1);
    if (what == "0" || what == "2") {
        title_ = text;
    } else if (what == "7") {  // file://host/path, the shell's working directory
        std::string path = text;
        if (path.starts_with("file://")) {
            const size_t slash = path.find('/', 7);
            path = slash == std::string::npos ? std::string() : path.substr(slash);
        }
        std::string decoded;  // (%-escapes)
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) && std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
                decoded += static_cast<char>(std::stoi(path.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                decoded += path[i];
            }
        }
        if (!decoded.empty()) cwd_ = decoded;
    }
}

// ---------------------------------------------------------------- pictures

namespace {

// Bytes zlib compressed (kitty's o=z), no more than `most` of them out.
bool inflate_all(const std::string& in, size_t most, std::string* out) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    out->clear();
    int rc = Z_OK;
    char buf[65536];
    while (rc == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        out->append(buf, sizeof buf - z.avail_out);
        if (out->size() > most) rc = Z_MEM_ERROR;
    }
    inflateEnd(&z);
    return rc == Z_STREAM_END;
}

// A sixel picture's colour from DEC's hue (0 blue, 120 red, 240 green), lightness and saturation, in percent.
u32 from_hls(int h, int l, int s) {
    const double hue = std::fmod((h + 240) % 360, 360.0) / 360.0, light = std::clamp(l, 0, 100) / 100.0, sat = std::clamp(s, 0, 100) / 100.0;
    auto part = [](double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    double r = light, g = light, b = light;
    if (sat > 0) {
        const double q = light < 0.5 ? light * (1 + sat) : light + sat - light * sat, p = 2 * light - q;
        r = part(p, q, hue + 1.0 / 3), g = part(p, q, hue), b = part(p, q, hue - 1.0 / 3);
    }
    auto v = [](double x) { return static_cast<u32>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); };
    return v(r) | v(g) << 8 | v(b) << 16 | 0xFF000000u;
}

}  // namespace

// A device control string: a sixel picture ("P1;P2;P3 q" and its data) is
// read and put at the cursor; the rest is passed over.
void Term::dcs() {
    size_t q = 0;
    while (q < dcs_.size() && ((dcs_[q] >= '0' && dcs_[q] <= '9') || dcs_[q] == ';')) ++q;
    if (q < dcs_.size() && dcs_[q] == 'q') sixel(std::string_view(dcs_).substr(q + 1));
}

// An application program command: kitty's pictures ("G" keys ";" data).
void Term::apc() {
    if (dcs_.empty() || dcs_[0] != 'G') return;
    const size_t semi = dcs_.find(';');
    kitty(std::string_view(dcs_).substr(1, semi == std::string::npos ? std::string::npos : semi - 1), semi == std::string::npos ? std::string() : dcs_.substr(semi + 1));
}

// Sixel: bands of six rows of pixels, a character a column of six (its
// bits, from the top), in the colour chosen (#n, or #n;2;r;g;b to set it,
// in percent; #n;1;h;l;s in DEC's hue, lightness and saturation); "!n" the
// next n times, "$" back to the band's start, "-" the next band. Pixels no
// character set are clear.
void Term::sixel(std::string_view d) {
    static const u8 kVt340[16][3] = {{0, 0, 0},    {20, 20, 80}, {80, 13, 13}, {20, 80, 20}, {80, 20, 80}, {20, 80, 80}, {80, 80, 20}, {53, 53, 53},
                                     {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33}, {60, 33, 60}, {33, 60, 60}, {60, 60, 33}, {80, 80, 80}};
    u32 palette[256];
    for (u32 k = 0; k < 256; ++k) {
        const u8* c = kVt340[k % 16];
        auto v = [](u8 pc) { return static_cast<u32>(pc * 255 / 100); };
        palette[k] = v(c[0]) | v(c[1]) << 8 | v(c[2]) << 16 | 0xFF000000u;
    }
    u32 w = 0, h = 0, stride = 0, rows_had = 0;  // (its pixels so far: rows_had of stride)
    std::vector<u32> px;
    auto room = [&](u32 x, u32 y) {  // (the picture made big enough for (x, y))
        if (x >= kPictureSideMost || y >= kPictureSideMost) return false;
        if (x >= stride || y >= rows_had) {
            const u32 s2 = x >= stride ? std::min(std::max(x + 1, stride * 2), kPictureSideMost) : stride;
            const u32 r2 = y >= rows_had ? std::min(std::max(y + 1, rows_had * 2), kPictureSideMost) : rows_had;
            std::vector<u32> bigger(static_cast<size_t>(s2) * r2, 0);
            for (u32 r = 0; r < rows_had; ++r) std::copy_n(px.data() + static_cast<size_t>(r) * stride, stride, bigger.data() + static_cast<size_t>(r) * s2);
            px.swap(bigger);
            stride = s2, rows_had = r2;
        }
        return true;
    };
    u32 x = 0, y = 0, colour = 0;
    size_t at = 0;
    auto number = [&](int otherwise) {
        if (at >= d.size() || d[at] < '0' || d[at] > '9') return otherwise;
        int n = 0;
        while (at < d.size() && d[at] >= '0' && d[at] <= '9') n = std::min(n * 10 + (d[at++] - '0'), 1000000);
        return n;
    };
    auto numbers = [&](int* out, int most) {  // (n;n;n...: how many)
        int k = 0;
        for (;;) {
            const int n = number(0);
            if (k < most) out[k++] = n;
            if (at < d.size() && d[at] == ';') ++at;
            else break;
        }
        return k;
    };
    while (at < d.size()) {
        const char c = d[at++];
        if (c == '"') {  // the size it says it has
            int ra[4] = {};
            if (numbers(ra, 4) == 4 && ra[2] > 0 && ra[3] > 0) {
                w = std::max(w, std::min<u32>(static_cast<u32>(ra[2]), kPictureSideMost));
                h = std::max(h, std::min<u32>(static_cast<u32>(ra[3]), kPictureSideMost));
            }
        } else if (c == '#') {
            int cd[5] = {};
            const int k = numbers(cd, 5);
            colour = static_cast<u32>(cd[0]) % 256;
            if (k == 5 && cd[1] == 2) {
                auto v = [](int pc) { return static_cast<u32>(std::clamp(pc, 0, 100) * 255 / 100); };
                palette[colour] = v(cd[2]) | v(cd[3]) << 8 | v(cd[4]) << 16 | 0xFF000000u;
            } else if (k == 5 && cd[1] == 1) {
                palette[colour] = from_hls(cd[2], cd[3], cd[4]);
            }
        } else if (c == '$') {
            x = 0;
        } else if (c == '-') {
            x = 0;
            y += 6;
        } else if (c == '!' || (c >= '?' && c <= '~')) {
            u32 times = 1;
            u8 bits = static_cast<u8>(c - '?');
            if (c == '!') {
                times = static_cast<u32>(std::max(1, number(1)));
                if (at >= d.size() || d[at] < '?' || d[at] > '~') continue;
                bits = static_cast<u8>(d[at++] - '?');
            }
            times = std::min(times, kPictureSideMost - std::min(x, kPictureSideMost));
            if (bits && times && room(x + times - 1, y + 5)) {
                const u32 rgba = palette[colour];
                for (u32 b = 0; b < 6; ++b)
                    if (bits >> b & 1) std::fill_n(px.data() + static_cast<size_t>(y + b) * stride + x, times, rgba);
            }
            x += times;
            w = std::max(w, std::min(x, kPictureSideMost));
            if (bits) h = std::max(h, std::min(y + static_cast<u32>(std::bit_width(bits)), kPictureSideMost));  // (to the lowest pixel set)
        }
    }
    if (!w || !h) return;
    auto rgba = std::make_shared<std::vector<u8>>(static_cast<size_t>(w) * h * 4, 0);
    for (u32 r = 0; r < h && r < rows_had; ++r)
        for (u32 k = 0; k < w && k < stride; ++k) {
            const u32 v = px[static_cast<size_t>(r) * stride + k];
            std::memcpy(rgba->data() + (static_cast<size_t>(r) * w + k) * 4, &v, 4);  // (its alpha whole or none: already multiplied)
        }
    TermPicture p;
    p.rgba = std::move(rgba);
    p.w = w, p.h = h;
    place(std::move(p), true, false);
}

// A picture put at the cursor (its cells counted from its size, unless
// set), the oldest going when there are too many. The cursor, if it moves:
// for sixel, to the row under the picture, at the column it started in; for
// kitty, to the cell after it on its last row.
void Term::place(TermPicture p, bool move_cursor, bool kitty_cursor) {
    if (!p.cols) p.cols = std::max(1u, (p.w + p.off_x + cell_w_ - 1) / cell_w_);
    if (!p.rows) p.rows = std::max(1u, (p.h + p.off_y + cell_h_ - 1) / cell_h_);
    p.serial = next_serial_++;
    p.alt = alt_;
    p.col = std::min(col_, cols_ - 1);
    p.line = alt_ ? row_ : lines_gone_ + history_.size() + row_;
    if (p.placement)  // (kitty's: the same image and placement again is a move)
        std::erase_if(pictures_, [&](const TermPicture& q) { return q.image == p.image && q.placement == p.placement; });
    const u32 col = p.col, rows = p.rows, cols = p.cols;
    pictures_.push_back(std::move(p));
    if (pictures_.size() > kPicturesMost) pictures_.erase(pictures_.begin());
    ++pictures_gen_;
    if (!move_cursor) return;
    for (u32 k = 1; k < rows; ++k) index();  // (scrolling: the picture goes up with the text)
    if (kitty_cursor) {
        col_ = std::min(col + cols, cols_ - 1);
    } else {
        index();
        col_ = col;
    }
    wrap_next_ = false;
}

// Kitty's graphics protocol: keys ("a=T,f=100,i=2,m=1": what to do, the
// data's form, the image's number, more to come) and the data, base64.
// Images are sent (a=t), put on the screen (a=p), both (a=T), taken away
// (a=d), or asked about (a=q); in parts (m=1 on all but the last); as RGB
// (f=24), RGBA (f=32) or PNG (f=100), zlib compressed (o=z) or not; only
// within the escape codes (t=d). The program is told how it went when it
// gave a number (i or I), unless it asked not to be (q).
void Term::kitty(std::string_view control, std::string payload) {
    auto keys_of = [](std::string_view c) {
        std::map<char, std::string> k;
        for (size_t at = 0; at < c.size();) {
            const size_t comma = std::min(c.find(',', at), c.size());
            const std::string_view kv = c.substr(at, comma - at);
            if (kv.size() >= 2 && kv[1] == '=') k[kv[0]] = std::string(kv.substr(2));
            at = comma + 1;
        }
        return k;
    };
    std::map<char, std::string> keys = keys_of(control);
    // In parts: the first part's keys stand for all of them.
    if (!kitty_control_.empty()) {
        if (kitty_payload_.size() + payload.size() <= kStringMost) kitty_payload_ += payload;
        if (keys['m'] == "1") return;
        keys = keys_of(kitty_control_);
        payload = std::move(kitty_payload_);
        kitty_control_.clear(), kitty_payload_.clear();
    } else if (keys['m'] == "1") {
        kitty_control_ = control.empty() ? std::string("a=t") : std::string(control);
        kitty_payload_ = std::move(payload);
        return;
    }
    auto num = [&](char k, u32 otherwise = 0) -> u32 {
        const auto it = keys.find(k);
        return it == keys.end() || it->second.empty() ? otherwise : static_cast<u32>(std::strtoul(it->second.c_str(), nullptr, 10));
    };
    auto ch = [&](char k, char otherwise) {
        const auto it = keys.find(k);
        return it == keys.end() || it->second.empty() ? otherwise : it->second[0];
    };
    const char action = ch('a', 't');
    u32 id = num('i');
    const u32 number = num('I'), placement = num('p'), quiet = num('q');
    auto tell = [&](const std::string& what) {
        const bool ok = what == "OK";
        if ((!id && !number) || quiet >= (ok ? 1u : 2u)) return;
        std::string r = "\x1b_G";
        if (id) r += "i=" + std::to_string(id);
        if (number) r += std::string(id ? "," : "") + "I=" + std::to_string(number);
        if (placement) r += ",p=" + std::to_string(placement);
        reply_ += r + ";" + what + "\x1b\\";
    };
    if (action == 'd') {  // taken away: all of them (a, A), or one image's (i, I)
        const char what = ch('d', 'a');
        const size_t had = pictures_.size();
        if (what == 'i' || what == 'I') {
            std::erase_if(pictures_, [&](const TermPicture& p) { return p.image == id && (!placement || p.placement == placement); });
            if (what == 'I' && !placement) kitty_images_.erase(id);
        } else {
            std::erase_if(pictures_, [&](const TermPicture& p) { return p.image != 0 && p.alt == alt_; });
            if (what == 'A') kitty_images_.clear();
        }
        if (pictures_.size() != had) ++pictures_gen_;
        return;
    }
    KittyImage img;
    if (action == 'p') {
        const auto it = kitty_images_.find(id);
        if (it == kitty_images_.end()) return tell("ENOENT:no such image");
        img = it->second;
    } else {
        if (ch('t', 'd') != 'd') return tell("EINVAL:only data sent in the escape codes is taken");
        const u32 f = num('f', 32), w = num('s'), h = num('v');
        // (The format and the size before the data: compressed, it is let
        // grow to what the size needs and no more, and a size past the
        // most a picture may be once let it grow to gigabytes.)
        if (f != 24 && f != 32 && f != 100) return tell("EINVAL:no such format");
        if (f != 100 && (!w || !h || w > kPictureSideMost || h > kPictureSideMost)) return tell("ENODATA:not enough data for its size");
        std::string data = from_base64(payload);
        if (ch('o', 0) == 'z') {
            std::string raw;
            const size_t most = f == 100 ? kStringMost : static_cast<size_t>(w) * h * (f / 8);
            if (!inflate_all(data, most, &raw)) return tell("EINVAL:the data would not decompress");
            data = std::move(raw);
        }
        auto rgba = std::make_shared<std::vector<u8>>();
        if (f == 100) {
            if (!g_picture_reader || !g_picture_reader(data, rgba.get(), &img.w, &img.h)) return tell("EBADPNG:the picture could not be read");
        } else {
            const size_t bpp = f / 8;
            if (data.size() < static_cast<size_t>(w) * h * bpp) return tell("ENODATA:not enough data for its size");
            img.w = w, img.h = h;
            rgba->resize(static_cast<size_t>(w) * h * 4);
            const u8* in = reinterpret_cast<const u8*>(data.data());
            u8* out = rgba->data();
            for (size_t k = 0, n = static_cast<size_t>(w) * h; k < n; ++k, in += bpp, out += 4) {
                const u32 a = bpp == 4 ? in[3] : 255;
                out[0] = static_cast<u8>(in[0] * a / 255), out[1] = static_cast<u8>(in[1] * a / 255), out[2] = static_cast<u8>(in[2] * a / 255), out[3] = static_cast<u8>(a);
            }
        }
        if (action == 'q') return tell("OK");
        img.rgba = std::move(rgba);
        if (!id && number) {  // (a number of the program's: the terminal's own for it)
            id = 0x80000000u;
            while (kitty_images_.contains(id)) ++id;
        }
        if (id) {
            kitty_images_[id] = img;
            // (The least used go when they come to more than is kept.)
            size_t bytes = 0;
            for (const auto& [k, v] : kitty_images_) bytes += v.rgba ? v.rgba->size() : 0;
            while (bytes > kKittyBytesMost && kitty_images_.size() > 1) {
                auto oldest = kitty_images_.begin();
                for (auto it = kitty_images_.begin(); it != kitty_images_.end(); ++it)
                    if (it->first != id && (oldest->first == id || it->second.used < oldest->second.used)) oldest = it;
                bytes -= oldest->second.rgba ? oldest->second.rgba->size() : 0;
                kitty_images_.erase(oldest);
            }
        }
        if (action != 'T') return tell("OK");
    }
    if (id)
        if (auto it = kitty_images_.find(id); it != kitty_images_.end()) it->second.used = ++kitty_used_;
    TermPicture p;
    p.rgba = img.rgba;
    p.w = img.w, p.h = img.h;
    p.image = id, p.placement = placement;
    p.off_x = std::min(num('X'), cell_w_ - 1), p.off_y = std::min(num('Y'), cell_h_ - 1);
    const u32 c = num('c'), r = num('r');
    if (c || r) {  // the cells it fills (one given: the other as its shape has it)
        p.fit = true;
        p.cols = c ? c : std::max(1u, static_cast<u32>(std::lround(static_cast<double>(r) * cell_h_ * img.w / std::max(1u, img.h) / cell_w_)));
        p.rows = r ? r : std::max(1u, static_cast<u32>(std::lround(static_cast<double>(c) * cell_w_ * img.h / std::max(1u, img.w) / cell_h_)));
    }
    place(std::move(p), num('C') != 1, true);
    tell("OK");
}

// ---------------------------------------------------------------- text

std::string Term::line_text(const TermCell* cells, u32 n) {
    std::string out;
    size_t keep = 0;
    for (u32 c = 0; c < n; ++c) {
        if (cells[c].attr & TermCell::kWideTail) continue;
        u32 cp = cells[c].cp;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | cp >> 6), out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | cp >> 12), out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | cp >> 18), out += static_cast<char>(0x80 | (cp >> 12 & 0x3F)), out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)),
                out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        if (cp != ' ') keep = out.size();
    }
    out.resize(keep);
    return out;
}

std::string Term::row_text(u32 row) const { return line_text(screen()[row].data(), cols_); }

}  // namespace blit
