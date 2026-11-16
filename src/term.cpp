#include "term.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace blit {

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
    graphics_ = insert_ = origin_ = app_cursor_ = bracketed_paste_ = false;
    autowrap_ = cursor_visible_ = true;
    if (alt_) alt_ = false;
    for (std::vector<TermCell>& r : cells_) std::ranges::fill(r, TermCell{});
    for (std::vector<TermCell>& r : alt_cells_) std::ranges::fill(r, TermCell{});
    tabs_.assign(cols_, false);
    for (u32 c = 8; c < cols_; c += 8) tabs_[c] = true;
    ++generation_;
}

void Term::resize(u32 cols, u32 rows) {
    cols = std::max(cols, 1u);
    rows = std::max(rows, 1u);
    if (cols == cols_ && rows == rows_) return;
    auto remake = [&](Rows& cells, bool keep_history) {
        // Fewer rows: the top ones go (into the history on the main
        // screen), so that the cursor's line stays on show.
        const u32 drop = rows < rows_ && row_ >= rows ? row_ - rows + 1 : 0;
        for (u32 r = 0; r < drop && keep_history; ++r) {
            history_.push_back(std::move(cells[r]));
            if (history_.size() > scrollback_) history_.pop_front();
        }
        Rows out;
        out.reserve(rows);
        for (u32 r = drop; r < rows_ && out.size() < rows; ++r) out.push_back(std::move(cells[r]));
        while (out.size() < rows) out.emplace_back(cols);
        for (std::vector<TermCell>& r : out) r.resize(cols);
        cells = std::move(out);
        return drop;
    };
    const u32 drop_main = remake(cells_, !alt_);
    const u32 drop_alt = remake(alt_cells_, false);
    const u32 drop = alt_ ? drop_alt : drop_main;
    row_ = std::min(row_ - std::min(row_, drop), rows - 1);
    col_ = std::min(col_, cols - 1);
    cols_ = cols;
    rows_ = rows;
    top_ = 0;
    bottom_ = rows_ - 1;
    wrap_next_ = false;
    tabs_.assign(cols_, false);
    for (u32 c = 8; c < cols_; c += 8) tabs_[c] = true;
    saved_.row = std::min(saved_.row, rows_ - 1), saved_.col = std::min(saved_.col, cols_ - 1);
    ++generation_;
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
        // Strings (a title, and what is not understood) run to BEL or ESC \.
        if (state_ == kOsc || state_ == kString) {
            if (b == 0x07) {
                if (state_ == kOsc) osc();
                state_ = kGround;
            } else if (b == 0x1B) {
                state_ = state_ == kOsc ? kOscEscape : kStringEscape;
            } else if (state_ == kOsc && osc_.size() < 4096) {
                osc_ += static_cast<char>(b);
            }
            continue;
        }
        if (state_ == kOscEscape || state_ == kStringEscape) {
            if (state_ == kOscEscape) osc();
            state_ = kGround;
            if (b == '\\') continue;  // (ST)
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
                } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
                    state_ = kString;  // DCS, SOS, PM, APC: not understood, passed over
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
            col_ = 0;
            index();
        }
        wrap_next_ = false;
    }
    if (w == 2 && col_ + 1 >= cols_) {  // (no room for both halves: on the next line)
        if (autowrap_) {
            erase(row_, col_, cols_);
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
                history_.pop_front();
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
    if (private_ == '>' || private_ == '=') {
        if (final == 'c') reply_ += "\x1b[>0;0;0c";  // (secondary device attributes)
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
            } else if (how == 3) {
                history_.clear();
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
            if (param(0, 0) == 0) reply_ += "\x1b[?62;22c";  // a VT220 with colour
            break;
        default: break;  // (t: window operations, and the rest)
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
            default: break;  // (mouse reporting, focus events, blinking: not done)
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
