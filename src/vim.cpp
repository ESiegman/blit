#include "vim.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <type_traits>

#include "casemap.hpp"
#include "fuzzy.hpp"
#include "threads.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

constexpr u64 kNone = ~0ull;
constexpr u32 kG = 0x10000;  // a key that came after "g": g + e is kG | 'e'
constexpr u32 kLB = 0x20000, kRB = 0x40000;  // and after "[" or "]"
constexpr u32 kEndCol = 0x7FFFFFFF;  // the column wanted after "$": the end of whatever line
constexpr char kOpComment = 1;       // the operator "gc" (no key of its own)
constexpr char kOpFormat = 2;        // the operator "gq": the language server formats the lines (asked of the application)
constexpr u32 kBack = 8;             // the Backspace key, as a motion

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Bytes of the document by offset, read a window at a time.
struct Peek {
    const Buffer& b;
    u64 base = 0, len = 0;
    char win[512];
    explicit Peek(const Buffer& buf) : b(buf) {}
    u8 at(u64 off) {
        if (off < base || off >= base + len) {
            base = off >= 128 ? off - 128 : 0;
            len = b.read(base, win, sizeof win);
        }
        return off - base < len ? static_cast<u8>(win[off - base]) : 0;
    }
};

// 0 white space, 1 punctuation, 2 a word's characters. For W, B and E
// everything that is not white space is a word.
int kind_of(u8 c, bool big) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0) return 0;
    if (big) return 2;
    return std::isalnum(c) || c == '_' || c >= 0x80 ? 2 : 1;
}

std::string utf8_of(u32 cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// The display columns text takes from column `col` on: a tab to the next
// stop, a wide character two, a mark or what is joined to an emoji none
// (as Editor::col_between counts them in the document).
u32 columns_in(std::string_view s, u32 col, u32 tab_width) {
    const u32 from = col;
    Joiner join;
    for (size_t i = 0; i < s.size();) {
        const u8 b = static_cast<u8>(s[i]);
        if (b == '\t') {
            col = (col / std::max<u32>(1, tab_width) + 1) * std::max<u32>(1, tab_width);
            join.reset();
            ++i;
        } else if (b < 0x80) {
            ++col;
            join.reset();
            ++i;
        } else {
            const Decoded d = decode_utf8(reinterpret_cast<const u8*>(s.data()) + i, s.size() - i);
            col += join.width(d.cp);
            i += d.len;
        }
    }
    return col - from;
}

// White space taken off up to k in s: one fewer if a mark is on the last
// space (it stays, with its mark, a character of its own).
size_t keep_marked(std::string_view s, size_t k) {
    if (k == 0 || k >= s.size() || static_cast<u8>(s[k]) < 0x80) return k;
    const Decoded d = decode_utf8(reinterpret_cast<const u8*>(s.data()) + k, s.size() - k);
    return cp_extends(d.cp) || d.cp == 0x200D ? k - 1 : k;
}

// Where the character (a grapheme cluster, as Editor::next_char has them)
// that starts at i in a piece of text ends: "\r\n" is one, any other
// control character ends one, and nothing in ASCII goes on with one.
size_t char_end_in(std::string_view s, size_t i) {
    const u8* q = reinterpret_cast<const u8*>(s.data());
    if (i >= s.size()) return s.size();
    if (q[i] == '\r' && i + 1 < s.size() && q[i + 1] == '\n') return i + 2;
    if (q[i] < 0x20 || q[i] == 0x7F) return i + 1;
    Decoded d = decode_utf8(q + i, s.size() - i);
    Joiner join;
    join.joins(d.cp);
    for (i += d.len; i < s.size() && q[i] >= 0x80; i += d.len) {
        d = decode_utf8(q + i, s.size() - i);
        if (!join.extends(d.cp)) break;
    }
    return i;
}

// Where the last character (a grapheme cluster, as Editor has them) of a
// piece of text starts.
size_t last_char_start(std::string_view s) {
    size_t last = 0;
    Joiner join;
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(reinterpret_cast<const u8*>(s.data()) + i, s.size() - i);
        const bool goes_on = i > 0 && ((d.cp == '\n' && s[i - 1] == '\r') || (d.cp >= 0x80 && join.extends(d.cp)));
        if (d.cp < 0x80) join.reset();
        if (!goes_on) last = i;
        i += d.len;
    }
    return last;
}

// ~ u U and g?: letters to the other case, to lower, to upper, or thirteen along the alphabet.
std::string recase(std::string text, char op) {
    size_t i = 0;
    for (; i < text.size() && (op == '?' || static_cast<u8>(text[i]) < 0x80); ++i) {  // (g? turns ASCII letters only, as in Vim)
        char& ch = text[i];
        const bool up = ch >= 'A' && ch <= 'Z', low = ch >= 'a' && ch <= 'z';
        if (op == '?') {
            if (up || low) ch = static_cast<char>((up ? 'A' : 'a') + (ch - (up ? 'A' : 'a') + 13) % 26);
        } else if (low && (op == 'U' || op == '~')) {
            ch = static_cast<char>(ch - 32);
        } else if (up && (op == 'u' || op == '~')) {
            ch = static_cast<char>(ch + 32);
        }
    }
    if (i == text.size()) return text;
    // Past ASCII, a character at a time, by Unicode's case, as Vim's
    // swapchar() has it: one that has an upper case goes up (gU, ~) and is
    // otherwise left as it is ("ǅ"), else one that has a lower case goes
    // down (gu, ~). What becomes another length is put in afresh.
    std::string out(text, 0, i);
    out.reserve(text.size());
    while (i < text.size()) {
        const Decoded d = decode_utf8(reinterpret_cast<const u8*>(text.data()) + i, text.size() - i);
        const u32 up = case_upper(d.cp);
        u32 to = up != d.cp ? (op == 'u' ? d.cp : up) : (op == 'U' ? d.cp : case_lower(d.cp));
        if (cp_extends(d.cp)) to = d.cp;  // (a mark goes with the letter it is on, and keeps its own case: U+0345)
        if (to == d.cp) out.append(text, i, d.len);
        else out += utf8_of(to);
        i += d.len;
    }
    return out;
}

// Lines as Vim counts them: the nothing after a final newline is not one.
u64 count_lines(const Editor& ed) {
    const u64 n = ed.buf.line_count(), size = ed.buf.size();
    if (n > 1 && size > 0) {
        char last = 0;
        ed.buf.read(size - 1, &last, 1);
        if (last == '\n') return n - 1;
    }
    return n;
}

u64 line_last(const Editor& ed, u64 line) {  // the last character of a line; its start if it is empty
    const u64 s = ed.buf.line_start(line), e = ed.line_end(line);
    return e > s ? ed.prev_char(e) : s;
}

// A character (a grapheme cluster) starts at `at`: not in the middle of
// one, as a mark on a space, the second half of a flag, or an emoji joined
// to the one before are. (ASCII stands alone, but for the "\n" of "\r\n".)
bool at_char(const Editor& ed, Peek& p, u64 at) {
    if (at == 0 || at >= ed.buf.size()) return true;
    const u8 b = p.at(at);
    if (b < 0x80) return b != '\n' || p.at(at - 1) != '\r';
    return ed.prev_char(ed.next_char(at)) == at;
}

// The character after the one at `at`, and where the one before `at`
// starts: a byte at a time through ASCII, where they are bytes, as Vim's
// own loops through white space went; Editor's clusters elsewhere (so that
// a mark on a space goes with it).
u64 char_after(const Editor& ed, Peek& p, u64 at) { return p.at(at) < 0x80 && p.at(at + 1) < 0x80 ? at + 1 : ed.next_char(at); }
u64 char_before(const Editor& ed, Peek& p, u64 at) { return p.at(at - 1) < 0x80 ? at - 1 : ed.prev_char(at); }

u64 first_nonblank(const Editor& ed, u64 line) {
    Peek p(ed.buf);
    u64 at = ed.buf.line_start(line);
    const u64 e = ed.line_end(line);
    while (at < e && (p.at(at) == ' ' || p.at(at) == '\t')) ++at;
    // (A mark on the last space makes it a character that is not blank: the cursor goes on to it, not into it.)
    if (at < e && p.at(at) >= 0x80) at = ed.prev_char(ed.next_char(at));
    return at;
}

// The rule for an exclusive motion that ends in the first column of a later
// line (:help exclusive): it ends at the end of the line before instead; and
// if it began at or before that line's first non-blank, it is for whole lines
// (true: those from lo's line to hi's). This is why "d}" leaves the empty line
// the paragraph ends at.
bool exclusive_trim(const Editor& ed, u64* lo, u64* hi) {
    if (*hi <= *lo || *hi >= ed.buf.size()) return false;
    const u64 end_line = ed.buf.line_of(*hi), start_line = ed.buf.line_of(*lo);
    if (end_line <= start_line || *hi != ed.buf.line_start(end_line)) return false;
    *hi = ed.line_end(end_line - 1);
    return *lo <= first_nonblank(ed, start_line);
}

// Vim's rule in op_delete(): a delete of characters over more than one
// line, from at or before the first non-blank of its first line, that
// leaves only blanks on its last, is of the lines whole. (`lines`: how
// many the text has; past them there is no line to keep.)
bool deletes_lines(const Editor& ed, u64 lo, u64 hi, u64 lines) {
    if (hi <= lo || hi > ed.buf.size()) return false;
    const u64 first = ed.buf.line_of(lo), last = ed.buf.line_of(hi);
    if (last <= first || last >= lines || lo > first_nonblank(ed, first)) return false;
    Peek p(ed.buf);
    for (u64 at = hi, e = ed.line_end(last); at < e; ++at)
        if (p.at(at) != ' ' && p.at(at) != '\t') return false;
    return true;
}

// What a line break typed is: what the text's lines end in.
const char* line_break(const Editor& ed) { return ed.crlf() ? "\r\n" : "\n"; }

std::string indent_of(const Editor& ed, u64 line) {
    const u64 s = ed.buf.line_start(line);
    return ed.buf.text(s, first_nonblank(ed, line) - s);
}

// ---- indentation by the language's rules (Vim::indent_rules)

bool word_byte(char c) { return std::isalnum(static_cast<u8>(c)) || c == '_'; }

// Whether a word is one of those between spaces in `words`.
bool one_of(const char* words, std::string_view w) {
    if (w.empty() || !*words) return false;
    for (const char* at = std::strstr(words, std::string(w).c_str()); at; at = std::strstr(at + 1, std::string(w).c_str()))
        if ((at == words || at[-1] == ' ') && (at[w.size()] == ' ' || at[w.size()] == 0)) return true;
    return false;
}

// The code of a line up to `end`: its blanks at either end and a comment
// after it left out (one whose marker comes first on the line or after a
// blank, so that one in a string mostly stays).
std::string code_of(const Editor& ed, u64 line, u64 end, std::string_view marker) {
    const u64 start = ed.buf.line_start(line);
    std::string t = ed.buf.text(start, std::min<u64>(end - start, 4096));
    while (!marker.empty() && marker.back() == ' ') marker.remove_suffix(1);
    if (!marker.empty())
        for (size_t at = t.find(marker); at != std::string::npos; at = t.find(marker, at + 1))
            if (at == 0 || t[at - 1] == ' ' || t[at - 1] == '\t') {
                t.resize(at);
                break;
            }
    const size_t a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
}

std::string_view first_word(std::string_view t) {
    size_t n = 0;
    while (n < t.size() && word_byte(t[n])) ++n;
    return t.substr(0, n);
}

std::string_view last_word(std::string_view t) {
    size_t n = t.size();
    while (n > 0 && word_byte(t[n - 1])) --n;
    return t.substr(n);
}

// Whether the line after this code is a level further in.
bool opens(const IndentRules& r, std::string_view t) {
    if (t.empty()) return false;
    const char e = t.back();
    if (r.brackets && (e == '(' || e == '[' || e == '{')) return true;
    if (r.colon && e == ':') return true;
    const std::string_view last = last_word(t);
    if (one_of(r.last, last)) return true;
    if (one_of(r.closers, last)) return false;  // (it closed what it opened: "if x then return end")
    if (one_of(r.first, first_word(t))) return true;
    if (!*r.anywhere) return false;
    // One of those words (Lua's function) opens a level unless the line
    // closes it again: each opener on the line counts one in, each closer
    // one out ("f(function() if a then b() end end)" opens none, as "else"
    // and "elseif ... then" open none). Words in a string are text.
    int open = 0;
    bool seen = false;
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '"' || t[i] == '\'') {
            const char q = t[i];
            for (++i; i < t.size() && t[i] != q; ++i)
                if (t[i] == '\\') ++i;
            ++i;
            continue;
        }
        if (t[i] == '[' && i + 1 < t.size() && t[i + 1] == '[') {  // (Lua's [[long string]])
            const size_t close = t.find("]]", i + 2);
            i = close == std::string_view::npos ? t.size() : close + 2;
            continue;
        }
        if (!word_byte(t[i])) {
            ++i;
            continue;
        }
        const std::string_view w = first_word(t.substr(i));
        if (one_of(r.anywhere, w)) {
            seen = true;
            ++open;
        } else if (one_of(r.last, w)) {
            ++open;
        }
        if (one_of(r.closers, w)) --open;
        i += w.size();
    }
    return seen && open > 0;
}

// Whether this code starts by closing a level.
bool closes(const IndentRules& r, std::string_view t) {
    if (t.empty()) return false;
    if (r.brackets && (t[0] == ')' || t[0] == ']' || t[0] == '}')) return true;
    return one_of(r.closers, first_word(t));
}

// An indent a level further out: a tab fewer, or a level's spaces.
std::string outdent(const Editor& ed, std::string indent, std::string_view unit) {
    const size_t width = unit == "\t" ? ed.tab_width : unit.size();
    if (!indent.empty() && indent.back() == '\t') indent.pop_back();
    else
        for (size_t n = 0; n < width && !indent.empty() && indent.back() == ' '; ++n) indent.pop_back();
    return indent;
}

// The bytes of a line that stand in display columns [col0, col1). (A
// character that is only partly in them counts.)
std::pair<u64, u64> columns_of(const Editor& ed, u64 line, u32 col0, u32 col1) {
    const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
    const u32 width = ed.col_between(start, end);
    if (width <= col0) return {end, end};
    u64 a = ed.off_at(line, col0);
    // (A character of no width just before, a mark alone or a zero width
    // space, stands at the same column: it is in the block too.)
    for (Peek p(ed.buf); a > start && p.at(a - 1) >= 0x80;) {
        const u64 before = ed.prev_char(a);
        if (ed.col_between(start, before) != ed.col_between(start, a)) break;
        a = before;
    }
    const u64 b = col1 > width ? end : std::min(end, ed.next_char(ed.off_at(line, col1 - 1)));
    return {a, std::max(a, b)};
}

// A place in the text as Vim's own loops step through it (its inc(),
// dec(), incl() and decl()): a character at a time, with the end of each
// line, where Vim's lines have their NUL, a place of its own. The word and
// sentence objects and ( and ) are Vim's code over this, so that they go
// where Vim's go, counts and ends of the text and all.
struct Walk {
    const Editor& ed;
    Peek& p;
    u64 at;
    bool eol() {  // at the end of a line ("\r\n" is one place)
        if (at >= ed.buf.size()) return true;
        const u8 b = p.at(at);
        return b == '\n' || (b == '\r' && p.at(at + 1) == '\n');
    }
    bool bol() { return at == 0 || p.at(at - 1) == '\n'; }
    bool empty() { return bol() && eol(); }    // on an empty line
    u8 ch() { return eol() ? 0 : p.at(at); }  // (0 at a line's end, as Vim's gchar_pos() has it)
    bool white() { return ch() == ' ' || ch() == '\t'; }
    int cls(bool big) { return kind_of(ch(), big); }
    bool last_line() {
        const u64 e = ed.line_end(ed.buf.line_of(at));
        return e >= ed.buf.size() || e + (p.at(e) == '\r' ? 2 : 1) >= ed.buf.size();
    }
    // Along the line: 0, or 2 onto its end. From the end of a line: 1 onto
    // the next, or -1 (staying) if it was the last.
    int inc() {
        if (!eol()) {
            at = char_after(ed, p, at);
            return eol() ? 2 : 0;
        }
        if (last_line()) return -1;
        at += p.at(at) == '\r' ? 2 : 1;
        return 1;
    }
    // Back along the line: 0. From its start: 1 onto the end of the line
    // before, or -1 (staying) at the start of the text.
    int dec() {
        if (!bol()) {
            at = char_before(ed, p, at);
            return 0;
        }
        if (at == 0) return -1;
        at -= at >= 2 && p.at(at - 2) == '\r' ? 2 : 1;
        return 1;
    }
    int incl() {  // inc(), and past the end of a line it gets to
        const int r = inc();
        return r >= 1 && !bol() ? inc() : r;
    }
    int decl() {  // dec(), and back over the end of a line it gets to
        const int r = dec();
        return r == 1 && !bol() ? dec() : r;
    }
    bool left() {  // Vim's oneleft(): back along the line, if there is any of it
        if (bol()) return false;
        at = char_before(ed, p, at);
        return true;
    }
};

// Vim's back_in_line(): back to where the run (a word, signs or blanks)
// that w is in starts on its line.
void back_in_line(Walk& w, bool big) {
    const int k = w.cls(big);
    while (!w.bol()) {
        w.dec();
        if (w.cls(big) != k) {
            w.inc();
            break;
        }
    }
}

// Vim's fwd_word() for one word, stopping at the end of a line (as "aw"
// uses it): past the word and the blanks after it. False when w was at the
// end of the last line, or got to it.
bool fwd_word(Walk& w, bool big) {
    const int k = w.cls(big);
    const bool last = w.last_line();
    const int i = w.inc();
    if (i == -1 || (i >= 1 && last)) return false;
    if (i >= 1) return true;
    if (k != 0)
        while (w.cls(big) == k)
            if (w.inc() != 0) return true;
    while (w.cls(big) == 0 && !w.empty())  // (an empty line is a word)
        if (w.inc() != 0) return true;
    return true;
}

// Vim's end_word() for one word, stopping at an empty line: onto the last
// character of this word, or (if w is at its end and `stop` is not set,
// or on blanks) of the next. False at the end of the text.
bool end_word(Walk& w, bool big, bool stop) {
    const int k = w.cls(big);
    if (w.inc() == -1) return false;
    if (k != 0 && w.cls(big) == k) {
        while (w.cls(big) == k)
            if (w.inc() == -1) return false;
    } else if (!stop || k == 0) {
        while (w.cls(big) == 0) {
            if (w.empty()) return true;
            if (w.inc() == -1) return false;
        }
        const int next = w.cls(big);
        while (w.cls(big) == next)
            if (w.inc() == -1) return false;
    }
    w.dec();  // (one past it)
    return true;
}

bool sentence_mark(u8 c, const char* set) { return c && std::strchr(set, c); }

// Vim's findsent(): on from w to where the count-th sentence after it
// starts, or back to where this one (then the one before) starts. A
// sentence ends at ".", "!" or "?" (and any ")", "]", '"' and "'" after
// it) with a blank or the end of a line after that; an empty line is one
// of its own. False, w as it was, when the text runs out with sentences
// still to go (the last sentence's end, if it ends with a full stop,
// counts as one more).
bool find_sentence(Walk& w, bool forward, u32 count) {
    constexpr u64 kFar = 1 << 20;  // (a sentence longer than this is not looked through)
    Walk pos = w;
    auto step = [&](Walk& x) { return forward ? x.incl() : x.decl(); };
    while (count--) {
        const u64 prev = pos.at;
        bool noskip = false, found = false;
        if (pos.ch() == 0) {  // on an empty line: on to one that is not (going on, that is where the next starts)
            do {
                if (step(pos) == -1) break;
            } while (pos.ch() == 0);
            found = forward;
        } else if (!forward) {
            pos.decl();
        }
        if (!found) {
            // Back to the last character that is no blank, full stop or the like.
            for (bool dot = false; pos.white() || sentence_mark(pos.ch(), ".!?)]\"'");) {
                Walk t = pos;
                if (t.decl() == -1 || (t.empty() && forward) || dot) break;
                const u8 c = pos.ch();
                if (sentence_mark(c, ".!?")) dot = true;
                if (sentence_mark(c, ")]\"'") && !sentence_mark(t.ch(), ".!?)]\"'")) break;
                pos.decl();
            }
            // Then on (or back) to where the sentence ends.
            const u64 from_line = w.ed.buf.line_of(pos.at);
            for (u64 steps = 0;; ++steps) {
                const u8 c = pos.ch();
                if (c == 0) {  // an empty line (going back: the sentence starts at the line after it)
                    if (!forward && w.ed.buf.line_of(pos.at) != from_line) pos.at += pos.p.at(pos.at) == '\r' ? 2 : 1;
                    break;
                }
                if (sentence_mark(c, ".!?")) {
                    Walk t = pos;
                    int r;
                    do r = t.inc();
                    while (r != -1 && sentence_mark(t.ch(), ")]\"'"));
                    if (r == -1 || t.white() || t.ch() == 0) {
                        pos.at = t.at;
                        if (pos.ch() == 0) pos.inc();  // (past the end of the line)
                        break;
                    }
                }
                if (step(pos) == -1 || steps > kFar) {
                    if (count) return false;
                    noskip = true;
                    break;
                }
            }
        }
        while (!noskip && pos.white())
            if (pos.incl() == -1) break;
        if (pos.at == prev) {  // it did not move: one step, and look again
            if (step(pos) == -1) {
                if (count) return false;
                break;
            }
            ++count;
        }
    }
    w.at = pos.at;
    return true;
}

// Vim's find_first_blank(): back to the first of the blanks before w (or
// it stays).
void first_blank(Walk& w) {
    while (w.decl() != -1) {
        if (!w.white()) {
            w.incl();
            break;
        }
    }
}

// Vim's findsent_forward(): over `count` halves, sentences and the blanks
// between them in turn, from the start of a sentence (`at_start`) or from
// blanks.
void sentences_on(Walk& w, u32 count, bool at_start) {
    while (count--) {
        find_sentence(w, true, 1);
        if (at_start) first_blank(w);
        if (count == 0 || at_start) w.decl();
        at_start = !at_start;
    }
}

// What a text object went over, from `start` to w (and the later of the
// two itself, if inclusive), as the bytes [lo, hi). One that ends at the
// start of a later line without it ends at the end of the line before, as
// an exclusive motion does there, and is of whole lines if it starts at or
// before its first line's first non-blank (exclusive_trim()); not in
// Visual mode.
bool walked(const Editor& ed, Walk& w, u64 start, bool inclusive, bool trim, u64* lo, u64* hi, bool* linewise) {
    if (w.eol()) w.left();  // (the cursor, which the end is, stands on a line's last character, not past it)
    Walk end{ed, w.p, std::max(start, w.at)};
    *lo = std::min(start, w.at);
    *hi = inclusive ? (end.eol() ? end.at : ed.next_char(end.at)) : end.at;
    if (!trim && inclusive && end.empty() && !end.last_line()) *hi = ed.buf.line_start(ed.buf.line_of(end.at) + 1);  // (Visual mode: selected, an empty line is its line break)
    if (!inclusive && trim && exclusive_trim(ed, lo, hi)) {
        *linewise = true;
        const u64 last = ed.buf.line_of(*hi);
        *lo = ed.buf.line_start(ed.buf.line_of(*lo));
        *hi = last + 1 < ed.buf.line_count() ? ed.buf.line_start(last + 1) : ed.buf.size();
    }
    return true;
}

bool wordish(char c) { return kind_of(static_cast<u8>(c), false) == 2; }

// Enter, or Ctrl-J or Ctrl-M (in Insert mode, the same).
bool is_enter(const VimKey& k) { return k.kind == VimKey::kEnter || (k.ctrl && (k.ch == 'j' || k.ch == 'm')); }

// Is the line that starts at `s` empty ("\r" alone counts as empty)? Not
// asked of the nothing after a final newline.
bool blank_at(const Buffer& buf, u64 s) {
    char b[2] = {};
    const u64 got = buf.read(s, b, 2);
    return got > 0 && (b[0] == '\n' || (b[0] == '\r' && got == 2 && b[1] == '\n'));
}

// The start of the line after the one `off` is on (size() if it is the last).
u64 next_line_start(const Buffer& buf, u64 off) {
    u64 cs = off;
    for (Buffer::Reader r = buf.reader(off); !r.at_end();) {
        const std::string_view c = r.chunk();
        if (const void* q = std::memchr(c.data(), '\n', c.size())) return cs + static_cast<u64>(static_cast<const char*>(q) - c.data()) + 1;
        cs += c.size();
        r.advance(static_cast<u32>(c.size()));
    }
    return buf.size();
}

// The starts of the lines after places asked for in order (each one at or
// after where the last answer was): one pass through the text, rather than
// a walk down the tree and a new start for each.
class NextLines {
public:
    explicit NextLines(const Buffer& b) : buf_(b) {}
    u64 after(u64 off) {
        if (!started_ || off < pos_) {
            r_ = buf_.reader(off);
            pos_ = off;
            started_ = true;
        }
        while (pos_ < off && !r_.at_end()) {
            const u64 k = std::min<u64>(r_.chunk().size(), off - pos_);
            r_.advance(static_cast<u32>(k));
            pos_ += k;
        }
        for (; !r_.at_end();) {
            const std::string_view c = r_.chunk();
            if (const void* q = std::memchr(c.data(), '\n', c.size())) {
                const u64 k = static_cast<u64>(static_cast<const char*>(q) - c.data()) + 1;
                pos_ += k;
                if (k < c.size()) r_.advance(static_cast<u32>(k));
                else r_.advance(static_cast<u32>(c.size()));
                return pos_;
            }
            pos_ += c.size();
            r_.advance(static_cast<u32>(c.size()));
        }
        return buf_.size();
    }

private:
    const Buffer& buf_;
    Buffer::Reader r_{};
    u64 pos_ = 0;
    bool started_ = false;
};

// Past so many lines an operation on lines is made as few edits as it takes
// (see substitute(), shift_lines(), join_lines()): a change to every line of
// a large file is otherwise millions of edits, each an undo record and a walk
// down the tree. Below it each line has its own edit, so that marks and the
// like on a line stay on it.
constexpr u64 kManyLines = 2000;
constexpr u64 kEditGap = 4096;      // unchanged text an edit may take in, to join two
constexpr u64 kEditMost = 1 << 20;  // and an edit's most

// Lines line0..line1 made over a megabyte at a time, one edit each (for an
// operation on many lines: see kManyLines). `fn` is given each line without
// its line break (and without a "\r" before it) and adds what it becomes to
// `out`.
void rewrite_lines(Editor& ed, u64 line0, u64 line1, const std::function<void(std::string_view, std::string&)>& fn) {
    u64 a = ed.buf.line_start(line0);
    u64 end = line1 + 1 < ed.buf.line_count() ? ed.buf.line_start(line1 + 1) - 1 : ed.buf.size();  // line1's newline, not taken in
    std::string was, out;
    while (a < end) {
        u64 b = std::min(end, a + kEditMost);
        was = ed.buf.text(a, b - a);
        if (b < end) {
            if (const size_t nl = was.rfind('\n'); nl != std::string::npos) {
                was.resize(nl);  // (up to a line's newline, not taken in)
                b = a + nl;
            } else {  // one line longer than all that: the whole of it
                b = std::min(end, next_line_start(ed.buf, b) - 1);
                was = ed.buf.text(a, b - a);
            }
        }
        out.clear();
        for (size_t from = 0;;) {
            const size_t nl = was.find('\n', from);
            std::string_view line(was.data() + from, (nl == std::string::npos ? was.size() : nl) - from);
            const bool cr = !line.empty() && line.back() == '\r';
            if (cr) line.remove_suffix(1);
            fn(line, out);
            if (cr) out += '\r';
            if (nl == std::string::npos) break;
            out += '\n';
            from = nl + 1;
        }
        if (out != was) ed.replace(a, b, out);
        end = end + out.size() - (b - a);
        a += out.size() + 1;  // (past the newline it stopped at)
    }
}

// The start of the first empty line that starts in (from, to), or ~0. The
// text is gone through a piece at a time with memchr, newline to newline.
u64 next_blank(const Buffer& buf, u64 from, u64 to) {
    to = std::min(to, buf.size());
    u64 cs = from;
    for (Buffer::Reader r = buf.reader(from); cs < to && !r.at_end();) {
        const std::string_view c = r.chunk();
        const char* const end = c.data() + c.size();
        for (const char* q = static_cast<const char*>(std::memchr(c.data(), '\n', c.size())); q; q = static_cast<const char*>(std::memchr(q + 1, '\n', static_cast<size_t>(end - q - 1)))) {
            const u64 s = cs + static_cast<u64>(q - c.data()) + 1;
            if (s >= to) return ~0ull;
            if (q + 1 < end && q[1] != '\n' && q[1] != '\r') continue;  // (the usual case, told without leaving the piece)
            if (blank_at(buf, s)) return s;
            if (q + 1 == end) break;
        }
        cs += c.size();
        r.advance(static_cast<u32>(c.size()));
    }
    return ~0ull;
}

// The start of the last empty line that starts before `to`, or ~0: the
// stretches before it are gone through in turn, nearest first.
u64 prev_blank(const Buffer& buf, u64 to) {
    constexpr u64 kStretch = 1 << 20;
    for (u64 b = to; b > 1;) {
        const u64 a = b > kStretch ? b - kStretch : 0;
        u64 best = ~0ull;
        for (u64 at = a == 0 ? 0 : a - 1, s; (s = next_blank(buf, at, b)) != ~0ull; at = s) best = s;
        if (best != ~0ull) return best;
        b = a;
    }
    return ~0ull;
}

// Where a match of `p` starts in `hay`, at or after `from` and before
// `limit`; npos if nowhere. Bytes outside that stretch are still looked at
// to tell whether a match is a whole word.
size_t find_in(std::string_view hay, const Vim::Pattern& p, size_t from, size_t limit) {
    const size_t n = p.text.size();
    for (size_t at = find_text(hay, p.text, p.ignore_case, from, limit); at != std::string_view::npos; at = find_text(hay, p.text, p.ignore_case, at + 1, limit))
        if (!p.word || !((at > 0 && wordish(hay[at - 1])) || (at + n < hay.size() && wordish(hay[at + n])))) return at;
    return std::string_view::npos;
}

}  // namespace

// (Asked several times a key, and it reads the far end of the file: it is
// kept until the text changes.)
u64 Vim::lines_of(const Editor& ed) {
    if (lines_rev_ != ed.revision() || lines_buf_ != &ed || lines_size_ != ed.buf.size()) {
        lines_rev_ = ed.revision();
        lines_buf_ = &ed;
        lines_size_ = ed.buf.size();
        lines_ = count_lines(ed);
    }
    return lines_;
}

// ---------------------------------------------------------------- searching

namespace {

// A pattern compiled: text as it stands when nothing in it is special.
Vim::Pattern compiled(std::string_view typed, const VimRegex::Options& o) {
    Vim::Pattern p;
    std::string error;
    const std::shared_ptr<const VimRegex> re = VimRegex::compile(typed, o, &error);
    if (!re) {
        p.text = std::string(typed);
        p.error = std::move(error);
        return p;
    }
    p.ignore_case = re->ignore_case();
    if (re->plain()) {  // looked for as text, the fastest way there is
        p.text = re->plain_text();
        p.word = re->plain_word();
        return p;
    }
    p.text = std::string(typed);
    p.re = re;
    return p;
}

// How many searches with p have given up so far (see VimRegex::overruns):
// a search that finds nothing says why by comparing this with what it was.
u32 overruns(const Vim::Pattern& p) { return p.re ? p.re->overruns() : 0; }
std::string not_found(const Vim::Pattern& p, u32 before) { return overruns(p) != before ? std::string(VimRegex::kTooSlow) : "E486: Pattern not found: " + p.text; }

RegexMatch plain_match(u64 at, u64 len) {
    RegexMatch m;
    m.start = m.anchor = at;
    m.end = at + len;
    m.group[0][0] = at;
    m.group[0][1] = at + len;
    for (u32 g = 1; g < 10; ++g) m.group[g][0] = m.group[g][1] = RegexMatch::kUnset;
    return m;
}

// The place after the character at `at` (a regular expression starts only
// where a character does).
u64 after_char(const Buffer& buf, u64 at) {
    char b = 0;
    buf.read(at, &b, 1);
    const u8 c = static_cast<u8>(b);
    return at + (c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4);
}
u64 after_char(std::string_view s, u64 at) {
    const u8 c = at < s.size() ? static_cast<u8>(s[at]) : 0;
    return at + (c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4);
}
// The first place at or after `at` where a character starts.
u64 align(const Buffer& buf, u64 at) {
    char b[4] = {};
    const u64 got = buf.read(at, b, 4);
    u64 k = 0;
    while (k < got && k < 3 && is_cont(static_cast<u8>(b[k]))) ++k;
    return at + k;
}

// Where a regular expression's matches may start: up to the end of the
// text, but not after a final newline (Vim has no line there).
u64 regex_top(const Buffer& buf) {
    const u64 size = buf.size();
    char last = 0;
    if (size > 0) buf.read(size - 1, &last, 1);
    return size + (size > 0 && last == '\n' ? 0 : 1);
}
u64 regex_top(std::string_view s) { return s.size() + (!s.empty() && s.back() == '\n' ? 0 : 1); }

u64 line_after(const Buffer& buf, u64 off) { return next_line_start(buf, off); }
u64 line_after(std::string_view s, u64 off) {
    const size_t nl = s.find('\n', off);
    return nl == std::string_view::npos ? s.size() : nl + 1;
}

}  // namespace

Vim::Pattern Vim::parse_pattern(std::string_view typed, const std::string& last_sub) {
    VimRegex::Options o;
    o.last_sub = last_sub;
    return compiled(typed, o);
}

Vim::Pattern Vim::make_pattern(std::string_view typed, bool regex, bool exact_case) {
    if (regex) {
        VimRegex::Options o;
        o.ignore_case = !exact_case;
        o.smart_case = true;
        return compiled(typed, o);
    }
    // Capitals typed: they count; none: neither do the text's.
    Pattern p;
    p.text = std::string(typed);
    p.ignore_case = !exact_case && std::none_of(p.text.begin(), p.text.end(), [](char ch) { return ch >= 'A' && ch <= 'Z'; });
    return p;
}

size_t Vim::match_in(std::string_view hay, const Pattern& p, size_t from, RegexMatch* m) {
    if (p.bad() || p.empty()) return std::string_view::npos;
    if (!p.re) {
        const size_t at = find_in(hay, p, from, hay.size());
        if (m && at != std::string_view::npos) *m = plain_match(at, p.text.size());
        return at;
    }
    RegexMatch got;
    if (!p.re->search(hay, from, regex_top(hay), &got)) return std::string_view::npos;
    if (m) *m = got;
    return static_cast<size_t>(got.start);
}

// The text is looked at where it lies, a piece of the document at a time;
// only a match that runs from one piece into the next is copied out to be
// compared.
u64 Vim::each_match(const Buffer& buf, const Pattern& p, u64 lo, u64 hi, const std::function<bool(u64)>& hit) {
    if (p.bad()) return kNone;
    if (p.re) {  // (each place a match starts, one after another)
        VimRegex::Searcher s(*p.re);
        RegexMatch m;
        hi = std::min(hi, regex_top(buf));
        for (u64 at = align(buf, lo); at < hi && s.next(buf, at, hi, &m); at = after_char(buf, m.anchor))
            if (hit(m.start)) return m.start;
        return kNone;
    }
    const u64 size = buf.size(), len = p.text.size();
    if (len == 0 || size < len) return kNone;
    hi = std::min(hi, size - len + 1);
    if (lo >= hi) return kNone;
    auto byte = [&](u64 off) {
        char c = 0;
        buf.read(off, &c, 1);
        return c;
    };
    // Whether the match at `i` in `c`, which starts at `cs`, is a whole word.
    auto whole = [&](std::string_view c, u64 cs, size_t i) {
        const u64 at = cs + i;
        if (at > 0 && wordish(i > 0 ? c[i - 1] : byte(at - 1))) return false;
        return !(at + len < size && wordish(i + len < c.size() ? c[i + len] : byte(at + len)));
    };
    std::string seam;
    u64 cs = lo;
    for (Buffer::Reader r = buf.reader(lo); cs < hi && !r.at_end();) {
        const std::string_view c = r.chunk();
        const u64 ce = cs + c.size();
        if (c.size() >= len) {
            const size_t stop = static_cast<size_t>(std::min(hi, ce - len + 1) - cs);
            for (size_t i = find_text(c, p.text, p.ignore_case, 0, stop); i != std::string_view::npos; i = find_text(c, p.text, p.ignore_case, i + 1, stop))
                if ((!p.word || whole(c, cs, i)) && hit(cs + i)) return cs + i;
        }
        // The matches that start in this piece and end in a later one.
        const u64 s0 = std::max(cs, ce + 1 >= len ? ce + 1 - len : 0), s1 = std::min(hi, ce);
        if (len > 1 && s0 < s1) {
            seam.resize(std::min(size - s0, s1 - s0 + len - 1));
            buf.read(s0, seam.data(), seam.size());
            const size_t stop = static_cast<size_t>(s1 - s0);
            for (size_t i = find_text(seam, p.text, p.ignore_case, 0, stop); i != std::string_view::npos; i = find_text(seam, p.text, p.ignore_case, i + 1, stop))
                if ((!p.word || whole(seam, s0, i)) && hit(s0 + i)) return s0 + i;
        }
        cs = ce;
        r.advance(static_cast<u32>(c.size()));
    }
    return kNone;
}

namespace {

// The matches one after another (see each_whole), in a Buffer or a string.
template <class Hay>
void whole_matches(const Hay& hay, const Vim::Pattern& p, u64 lo, u64 hi, bool per_line, const std::function<bool(const RegexMatch&)>& fn) {
    if (p.bad() || p.empty()) return;
    if (!p.re) {
        const u64 len = p.text.size();
        u64 free_from = lo;  // where the next may start: not inside the last, nor (per line) on its line
        if constexpr (std::is_same_v<Hay, Buffer>) {
            NextLines lines_after(hay);
            Vim::each_match(hay, p, lo, hi, [&](u64 at) {
                if (at < free_from) return false;
                if (!fn(plain_match(at, len))) return true;
                free_from = per_line ? std::max(at + len, lines_after.after(at)) : at + len;
                return false;
            });
        } else {
            const u64 top = std::min<u64>(hi, hay.size());
            for (size_t at = find_in(hay, p, lo, top); at != std::string_view::npos && at < top; at = find_in(hay, p, free_from, top)) {
                if (!fn(plain_match(at, len))) return;
                free_from = per_line ? std::max(at + len, line_after(hay, at)) : at + len;
                if (free_from >= top) return;
            }
        }
        return;
    }
    VimRegex::Searcher s(*p.re);
    RegexMatch m;
    hi = std::min(hi, regex_top(hay));
    u64 at = lo, last_end = kNone;
    while (at < hi && s.next(hay, at, hi, &m)) {
        if (m.start == m.end && m.start == last_end) {  // nothing, just where the last one ended: not again
            at = after_char(hay, std::max(at, m.anchor));
            continue;
        }
        if (!fn(m)) return;
        last_end = m.end;
        at = std::max(m.end, m.anchor);
        if (per_line) at = std::max(at, line_after(hay, m.start));
    }
}

}  // namespace

void Vim::each_whole(const Buffer& buf, const Pattern& p, u64 lo, u64 hi, bool per_line, const std::function<bool(const RegexMatch&)>& fn) {
    whole_matches(buf, p, lo, hi, per_line, fn);
}

void Vim::each_whole(std::string_view text, const Pattern& p, u64 lo, u64 hi, bool per_line, const std::function<bool(const RegexMatch&)>& fn) {
    whole_matches(text, p, lo, hi, per_line, fn);
}

u64 Vim::find(const Buffer& buf, const Pattern& p, u64 from, bool forward, bool wrap, bool* wrapped, u64 reach, RegexMatch* out) {
    if (wrapped) *wrapped = false;
    if (p.empty() || p.bad()) return kNone;
    const u64 len = p.re ? 0 : p.text.size();
    if (buf.size() < len) return kNone;
    // (A regular expression may match nothing at the very end.)
    const u64 size = p.re ? regex_top(buf) : buf.size();
    // The first or the last match in [lo, hi), the stretch nearest `from`
    // looked through first. Most searches end in the first few megabytes,
    // looked through on this thread; past them the parts of a long stretch
    // are taken by several threads at once, nearest first, and no part is
    // started once a nearer one has a match.
    constexpr u64 kAlone = 8 << 20, kPart = 16 << 20, kStretch = 1 << 20;
    auto in_part = [&](u64 a, u64 b, bool want_first, RegexMatch* got) -> bool {
        if (!p.re) {
            u64 best = kNone;
            if (want_first) {
                best = each_match(buf, p, a, b, [](u64) { return true; });
            } else {
                for (u64 top = b; top > a && best == kNone;) {  // (backward: the stretches before b in turn)
                    const u64 bottom = top - a > kStretch ? top - kStretch : a;
                    each_match(buf, p, bottom, top, [&](u64 at) {
                        best = at;
                        return false;
                    });
                    top = bottom;
                }
            }
            if (best == kNone) return false;
            *got = plain_match(best, len);
            return true;
        }
        VimRegex::Searcher s(*p.re);
        RegexMatch m;
        a = align(buf, a);
        if (want_first) {
            // (A match that \zs says starts here may have begun earlier on the line.)
            u64 at = p.re->has_zs() && a > 0 ? buf.line_start(buf.line_of(a)) : a;
            for (; at < b && s.next(buf, at, b, &m); at = after_char(buf, m.anchor))
                if (m.start >= a) return *got = m, true;
            return false;
        }
        for (u64 top = b; top > a;) {
            const u64 bottom = top - a > kStretch ? align(buf, top - kStretch) : a;
            bool any = false;
            for (u64 at = bottom; at < top && s.next(buf, at, top, &m); at = after_char(buf, m.anchor))
                if (m.start < b) *got = m, any = true;
            if (any) return true;
            top = bottom;
        }
        return false;
    };
    auto look = [&](u64 lo, u64 hi, bool want_first, RegexMatch* got) -> bool {
        if (lo >= hi) return false;
        const u64 near = hi - lo <= kAlone ? (want_first ? hi : lo) : want_first ? lo + kAlone : hi - kAlone;
        const bool hit = want_first ? in_part(lo, near, true, got) : in_part(near, hi, false, got);
        if (hit || near == (want_first ? hi : lo)) return hit;
        const u64 far_lo = want_first ? near : lo, far_hi = want_first ? hi : near;
        const size_t parts = static_cast<size_t>((far_hi - far_lo + kPart - 1) / kPart);
        std::vector<RegexMatch> found(parts);
        std::vector<char> any(parts, 0);
        std::atomic<size_t> next{0}, nearest{parts};
        auto work = [&] {
            for (size_t k; (k = next.fetch_add(1)) < parts && k < nearest.load();) {
                const u64 a = want_first ? far_lo + k * kPart : (far_hi - far_lo > (k + 1) * kPart ? far_hi - (k + 1) * kPart : far_lo);
                const u64 b = want_first ? std::min(far_hi, a + kPart) : far_hi - k * kPart;
                any[k] = in_part(a, b, want_first, &found[k]);
                if (any[k])
                    for (size_t was = nearest.load(); k < was && !nearest.compare_exchange_weak(was, k);) {
                    }
            }
        };
        on_threads(static_cast<unsigned>(std::min<size_t>(parts, cores())), work);
        if (nearest.load() >= parts) return false;
        *got = found[nearest.load()];
        return true;
    };
    auto first = [&](u64 lo, u64 hi, RegexMatch* got) { return look(lo, hi, true, got); };
    auto last = [&](u64 lo, u64 hi, RegexMatch* got) { return look(lo, hi, false, got); };
    RegexMatch m;
    bool hit;
    if (forward) {
        const u64 lo = from + 1;  // (from may be ~0: then everything)
        const u64 span = lo < size ? size - lo : 0;
        hit = first(lo, span > reach ? lo + reach : size, &m);
        if (!hit && wrap && reach > span) {
            hit = first(0, std::min(lo, reach - span), &m);
            if (hit && wrapped) *wrapped = true;
        }
    } else {
        const u64 to = std::min(from, size);
        hit = last(to > reach ? to - reach : 0, to, &m);
        if (!hit && wrap && reach > to) {
            const u64 left = reach - to;
            hit = last(size - to > left ? size - left : to, size, &m);
            if (hit && wrapped) *wrapped = true;
        }
    }
    if (!hit) return kNone;
    if (out) *out = m;
    return m.start;
}

void Vim::replacement(const RegexMatch& m, std::string_view with, bool rules, const std::function<std::string(u64, u64)>& text, std::string* out, std::string_view nl) {
    if (!rules) out->append(with);
    else regex_expand(with, m, text, out, nl);
}

u64 Vim::replace_all(Editor& ed, const Pattern& p, std::string_view with, bool rules, u64 lo, u64 hi, bool per_line, u64* lines_out, u64* last_out) {
    // The matches are found first, where the text lies, and what each
    // becomes is made while the text is as it was (a group may be before
    // the match itself, after a \zs). When every match becomes the same
    // there is one copy of it.
    struct Found {
        u64 start, end, off;  // the match, and where what it becomes starts in `made`
    };
    std::vector<Found> found;
    std::string made;
    const bool same = !rules || regex_replacement_plain(with);
    auto text = [&](u64 a, u64 b) { return ed.buf.text(a, b > a ? b - a : 0); };
    const std::string_view nl = line_break(ed);  // (what \r puts in)
    if (same) replacement(plain_match(0, 0), with, rules, text, &made, nl);
    const u32 runs = overruns(p);
    each_whole(ed.buf, p, lo, hi, per_line, [&](const RegexMatch& m) {
        found.push_back({m.start, m.end, same ? 0 : made.size()});
        if (!same) replacement(m, with, rules, text, &made, nl);
        return true;
    });
    if (found.empty() || overruns(p) != runs) return 0;  // (one that gave up part of the way changes nothing)
    auto made_of = [&](size_t i) {
        if (same) return std::string_view(made);
        const u64 to = i + 1 < found.size() ? found[i + 1].off : made.size();
        return std::string_view(made).substr(found[i].off, to - found[i].off);
    };
    // The matches by line (the one each starts on): [begin, end) of `found`.
    struct Run {
        size_t begin, end;
    };
    std::vector<Run> by_line;
    {
        NextLines lines_after(ed.buf);
        for (size_t i = 0, line_end = 0; i < found.size(); ++i) {
            if (i == 0 || found[i].start >= line_end) {
                by_line.push_back({i, i});
                line_end = lines_after.after(found[i].start);
            }
            by_line.back().end = i + 1;
        }
    }
    // The edits: each from the first match of a run of lines to the end of
    // its last. Each line its own, unless there are many: then lines close
    // together go into one edit (see kManyLines).
    std::vector<Run> edits;
    const bool many = by_line.size() > kManyLines;
    for (const Run& ln : by_line) {
        if (many && !edits.empty()) {
            const Run& e = edits.back();
            const u64 gap = found[ln.begin].start - found[e.end - 1].end, span = found[ln.end - 1].end - found[e.begin].start;
            if (gap <= kEditGap && span <= kEditMost) {
                edits.back().end = ln.end;
                continue;
            }
        }
        edits.push_back(ln);
    }
    // (The cursor stays where it was in the text, which the edits before it
    // move; one inside an edit goes to its start. Whoever wants it
    // elsewhere, :s, puts it there.)
    const u64 cursor = ed.cursor();
    u64 cursor_to = cursor;
    ed.begin_group();
    i64 moved = 0;  // what the edits made so far (all before the rest) have added
    u64 last = 0;
    std::string out;
    for (const Run& e : edits) {
        const u64 a = found[e.begin].start, b = found[e.end - 1].end;
        const u64 at = static_cast<u64>(static_cast<i64>(a) + moved);
        const std::string was = ed.buf.text(at, b - a);
        out.clear();
        for (size_t i = e.begin; i < e.end; ++i) {
            if (i > e.begin) out.append(was, found[i - 1].end - a, found[i].start - found[i - 1].end);
            if (i + 1 == found.size()) last = at + out.size();
            out += made_of(i);
        }
        ed.replace(at, at + (b - a), out);
        moved += static_cast<i64>(out.size()) - static_cast<i64>(b - a);
        if (b <= cursor) cursor_to = static_cast<u64>(static_cast<i64>(cursor) + moved);
        else if (a < cursor) cursor_to = at;
    }
    ed.end_group();
    ed.move_to(std::min(cursor_to, ed.buf.size()), false);
    if (lines_out) *lines_out = by_line.size();
    if (last_out) *last_out = last;
    return found.size();
}

std::string Vim::replace_text(std::string_view text, const Pattern& p, std::string_view with, bool rules, u64* count) {
    std::string out;
    u64 from = 0, n = 0;
    auto part = [&](u64 a, u64 b) { return std::string(text.substr(a, b > a ? b - a : 0)); };
    const size_t first_nl = text.find('\n');
    const std::string_view nl = first_nl != std::string_view::npos && first_nl > 0 && text[first_nl - 1] == '\r' ? "\r\n" : "\n";  // (by its first line, as Editor reads a file's)
    each_whole(text, p, 0, regex_top(text), false, [&](const RegexMatch& m) {
        out.append(text.substr(from, m.start - from));
        replacement(m, with, rules, part, &out, nl);
        from = m.end;
        ++n;
        return true;
    });
    out.append(text.substr(std::min<u64>(from, text.size())));
    if (count) *count = n;
    return out;
}

bool Vim::search(Editor& ed, bool forward, u32 count) {
    if (pattern_.empty()) {
        message_ = "E35: No previous regular expression";
        return false;
    }
    if (pattern_.bad()) {
        message_ = pattern_.error;
        return false;
    }
    u64 at = ed.cursor();
    bool wrapped_any = false;
    const u32 runs = overruns(pattern_);
    for (u32 i = 0; i < std::max<u32>(1, count); ++i) {
        bool wrapped = false;
        const u64 hit = find(ed.buf, pattern_, at, forward, true, &wrapped);
        if (hit == kNone) {
            message_ = not_found(pattern_, runs);
            return false;
        }
        wrapped_any |= wrapped;
        at = hit;
    }
    highlight_on_ = true;
    go(ed, at);
    if (wrapped_any) message_ = forward ? "search hit BOTTOM, continuing at TOP" : "search hit TOP, continuing at BOTTOM";
    return true;
}

// ---------------------------------------------------------------- small things

bool Vim::refuse() {
    if (!read_only) return false;
    message_ = "E21: Cannot make changes, 'modifiable' is off";
    return true;
}

void Vim::clear_pending() {
    count_ = op_count_ = 0;
    counted_ = false;
    op_ = 0;
    reg_ = 0;
    await_ = kNothing;
    shown_.clear();
}

// Outside Insert mode the cursor is on a character, not after the last one,
// and never on the nothing after a final newline.
void Vim::clamp(Editor& ed) {
    if (mode_ == kInsert || mode_ == kReplace) return;
    u64 line = ed.buf.line_of(ed.cursor());
    const u64 n = lines_of(ed);
    if (line >= n) line = n - 1;
    const u64 last = line_last(ed, line);
    if (ed.cursor() > last || ed.buf.line_of(ed.cursor()) != line) ed.move_to(last, false);
    else if (ed.has_selection()) ed.move_to(ed.cursor(), false);
}

void Vim::go(Editor& ed, u64 off, bool keep_column) {
    ed.move_to(std::min(off, ed.buf.size()), false);
    clamp(ed);
    if (!keep_column) {
        vis_eol_ = false;
        const u64 line = ed.buf.line_of(ed.cursor());
        want_col_ = ed.col_between(ed.buf.line_start(line), ed.cursor());
        // On a tab Vim's cursor stands at its last column (in Visual mode,
        // only after where the selection began), and that is the column
        // that j and k keep.
        char c = 0;
        if (ed.tab_width > 0 && ed.buf.read(ed.cursor(), &c, 1) == 1 && c == '\t' && !(visual() && ed.cursor() <= vstart_))
            want_col_ = (want_col_ / ed.tab_width + 1) * ed.tab_width - 1;
    }
}

void Vim::store(char name, std::string text, bool linewise, bool yank, const char* nl, bool blockwise) {
    if (name == '_') return;
    auto r = std::make_shared<const Register>(Register{std::move(text), linewise, blockwise});
    if (!yank && !name) {
        // What was deleted: whole lines (or more than one line) go to "1,
        // pushing what was there along to "2 and on up to "9; less than a
        // line goes to "-.
        if (linewise || r->text.find('\n') != std::string::npos) {
            for (char n = '9'; n > '1'; --n) {
                const auto it = regs_.find(static_cast<char>(n - 1));
                if (it != regs_.end()) regs_[n] = it->second;
            }
            regs_['1'] = r;
        } else {
            regs_['-'] = r;
        }
    }
    if (name == '+' || name == '*') {
        if (clipboard_set) clipboard_set(r->text);
    } else if (name >= 'A' && name <= 'Z') {
        std::shared_ptr<const Register>& into = regs_[lower(name)];
        Register more = into ? *into : Register{};
        if (more.text.empty()) {
            more = *r;
        } else if (linewise && !more.linewise) {  // lines added to characters: a line of their own
            more.text += nl;
            more.text += r->text;
        } else if (!linewise && more.linewise) {  // characters added to lines: a line of their own, so the register stays well-formed
            more.text += r->text;
            if (more.text.back() != '\n') more.text += nl;
        } else {
            more.text += r->text;
        }
        more.linewise = more.linewise || linewise;
        into = std::make_shared<const Register>(std::move(more));
        r = into;  // (what "" holds is the whole register)
    } else if (name) {
        regs_[name] = r;
    }
    if (yank && !name) regs_['0'] = r;
    regs_['"'] = std::move(r);
}

std::shared_ptr<const Vim::Register> Vim::fetch(char name) {
    auto made = [](std::string text) { return std::make_shared<const Register>(Register{std::move(text)}); };
    if (name == '+' || name == '*') {
        Register r;
        if (clipboard_get) r.text = clipboard_get();
        r.linewise = !r.text.empty() && r.text.back() == '\n';
        return std::make_shared<const Register>(std::move(r));
    }
    if (name == '.') return made(last_inserted_);
    if (name == '/') return made(pattern_.text);
    if (name == ':') {
        // (The history holds searches too: the last one that is a command.)
        for (auto it = history_.rbegin(); it != history_.rend(); ++it)
            if (!it->empty() && (*it)[0] == ':') return made(it->substr(1));
        return made({});
    }
    const auto it = regs_.find(name ? lower(name) : '"');
    if (it != regs_.end() && it->second) return it->second;
    static const auto none = std::make_shared<const Register>();
    return none;
}

void Vim::visual_range(const Editor& ed, u64* lo, u64* hi) const {
    const u64 a = std::min(vstart_, ed.cursor()), b = std::max(vstart_, ed.cursor()), size = ed.buf.size();
    if (mode_ == kVisualLine) {
        *lo = ed.buf.line_start(ed.buf.line_of(a));
        const u64 line = ed.buf.line_of(std::min(b, size));
        *hi = line + 1 < ed.buf.line_count() ? ed.buf.line_start(line + 1) : size;
    } else {
        *lo = a;
        *hi = b < size ? ed.next_char(b) : size;
        // After "$" the selection takes the end of the line in (as in Vim, where the cursor then stands past the last character).
        if (vis_eol_ && mode_ == kVisual && ed.cursor() >= vstart_) *hi = std::min(ed.line_end(ed.buf.line_of(b)) + 1, size);
    }
}

std::string Vim::status() const {
    if (mode_ == kCommand) return std::string(1, cmd_kind_) + cmd_;
    std::string s = message_.text;
    if (s.empty()) s = mode_ == kInsert ? "-- INSERT --" : mode_ == kReplace ? "-- REPLACE --" : mode_ == kVisual ? "-- VISUAL --" : mode_ == kVisualLine ? "-- VISUAL LINE --" : mode_ == kVisualBlock ? "-- VISUAL BLOCK --" : "";
    if (macro_into_) s += std::string(s.empty() ? "" : "  ") + "recording @" + macro_into_;
    return s;
}

std::string Vim::showcmd() const { return shown_; }

int Vim::status_cursor() const {
    if (mode_ != kCommand) return -1;
    return 1 + static_cast<int>(columns_in(cmd_, 1, 8));  // (in cells: a wide character two, a mark none)
}

void Vim::suspend(Editor& ed) {
    confirm_end(ed);
    if (mode_ == kInsert || mode_ == kReplace || insert_once_) leave_insert(ed);
    if (visual()) leave_visual(ed);
    shelved_[buffer_id] = Places{std::move(jumps_), jump_at_, prev_jump_, last_change_, last_insert_at_, last_vstart_, last_vend_, last_vmode_};
    forget();
}

void Vim::forget() {
    confirm_.on = false;  // (its undo step ends with the document, which has gone)
    mode_ = kNormal;
    cmd_return_ = kNormal;
    cmd_op_ = 0;
    clear_pending();
    insert_once_ = 0;
    block_insert_.on = false;
    jumps_.clear();
    jump_at_ = 0;
    prev_jump_ = 0;
    last_change_ = last_insert_at_ = last_vstart_ = last_vend_ = kNone;
    last_vmode_ = kVisual;
    lines_buf_ = nullptr;
    lines_rev_ = lines_size_ = kNone;
}

// A place at the end of the jump list. A line is in the list once: one
// that was there for the same line goes.
void Vim::list_jump(const Editor& ed, u64 off) {
    const u64 size = ed.buf.size(), line = ed.buf.line_of(std::min(off, size));
    // (Its line as a stretch of bytes, so that the others need no looking up.)
    const u64 from = ed.buf.line_start(line), to = line + 1 >= ed.buf.line_count() ? ~0ull : ed.buf.line_start(line + 1);
    std::erase_if(jumps_, [&](u64 at) { return std::min(at, size) >= from && std::min(at, size) < to; });
    jumps_.push_back(off);
    if (jumps_.size() > 100) jumps_.erase(jumps_.begin());
}

// A long move is about to start from `off`: <C-o> and '' come back to it.
void Vim::remember_jump(const Editor& ed, u64 off) {
    jumped_ = true;
    if (on_jump) on_jump(off);
    prev_jump_ = off;
    list_jump(ed, off);
    jump_at_ = jumps_.size();
}

void Vim::jump(Editor& ed, u64 off) {
    if (mode_ == kInsert || mode_ == kReplace || mode_ == kCommand) return;
    remember_jump(ed, ed.cursor());
    go(ed, off);
}

void Vim::resume(Editor& ed) {
    mode_ = kNormal;
    clear_pending();
    if (const auto it = shelved_.find(buffer_id); it != shelved_.end()) {
        Places& p = it->second;
        const u64 size = ed.buf.size();
        jumps_ = std::move(p.jumps);
        for (u64& j : jumps_) j = std::min(j, size);
        jump_at_ = std::min(p.jump_at, jumps_.size());
        prev_jump_ = p.prev_jump, last_change_ = p.last_change, last_insert_at_ = p.last_insert_at;
        last_vstart_ = p.last_vstart, last_vend_ = p.last_vend, last_vmode_ = p.last_vmode;
        shelved_.erase(it);
    }
    if (ed.has_selection()) ed.move_to(ed.cursor(), false);
    go(ed, ed.cursor());
}

void Vim::edited(u64 start, u64 old_end, u64 new_end) {
    auto fix = [&](u64& at) {
        if (at == kNone) return;
        if (at >= old_end) at = at - old_end + new_end;
        else if (at > start) at = start;
    };
    for (auto& [key, at] : marks_)
        if (key >> 8 == buffer_id) fix(at);
    for (u64& at : jumps_) fix(at);
    fix(prev_jump_), fix(last_change_), fix(last_insert_at_), fix(last_vstart_), fix(last_vend_);
}

// ---------------------------------------------------------------- Visual Block mode

bool Vim::block(const Editor& ed, u64* line0, u64* line1, u32* col0, u32* col1) const {
    if (mode_ != kVisualBlock) return false;
    const u64 size = ed.buf.size(), a = std::min(vstart_, size), b = std::min(ed.cursor(), size);
    const u64 la = ed.buf.line_of(a), lb = ed.buf.line_of(b);
    // The columns a character takes up: a tab or a wide one, more than one.
    auto span = [&](u64 off, u64 line, u32* from, u32* to) {
        const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
        *from = ed.col_between(start, off);
        *to = off < end ? std::max(ed.col_between(start, ed.next_char(off)), *from + 1) : *from + 1;  // (a mark alone, no wider than nothing, still has its column)
    };
    u32 a0, a1, b0, b1;
    span(a, la, &a0, &a1);
    span(b, lb, &b0, &b1);
    *line0 = std::min(la, lb);
    *line1 = std::max(la, lb);
    *col0 = std::min(a0, b0);
    *col1 = want_col_ == kEndCol ? kEndCol : std::max(a1, b1);  // after "$": to the end of every line
    return true;
}

// Visual Block mode's commands, on the rectangle: d y c; D and C (from its
// left edge to the end of every line); I and A; ~ u U ? (the letters' case,
// and rot13); r (every character becomes `arg`); < and > (by `arg`
// levels); p (the register in its place). Visual mode ends.
void Vim::block_do(Editor& ed, char what, u32 arg) {
    u64 line0, line1;
    u32 col0, col1;
    if (!block(ed, &line0, &line1, &col0, &col1)) return;
    leave_visual(ed);
    if (what != 'y' && refuse()) return;
    if (what == 'D' || what == 'C') col1 = kEndCol;
    const bool to_end = col1 == kEndCol;
    std::vector<std::pair<u64, u64>> rows;
    std::string text;  // the rows, one to a line
    for (u64 l = line0; l <= line1; ++l) {
        rows.push_back(columns_of(ed, l, col0, col1));
        if (l > line0) text += '\n';
        text += ed.buf.text(rows.back().first, rows.back().second - rows.back().first);
    }
    const u64 corner = rows[0].first;
    auto remove = [&] {
        for (size_t i = rows.size(); i-- > 0;)
            if (rows[i].second > rows[i].first) ed.replace(rows[i].first, rows[i].second, "");
    };
    switch (what) {
        case 'y':
            store(reg_, std::move(text), false, true, line_break(ed), true);
            go(ed, corner);
            break;
        case 'd':
        case 'D':
            store(reg_, std::move(text), false, false, line_break(ed), true);
            ed.begin_group();
            remove();
            ed.end_group();
            changing_ = true;
            go(ed, corner);
            break;
        case 'c':
        case 'C':
        case 'I':
            // Typed on the first line; Esc puts the same on the others (leave_insert()).
            ed.begin_group();
            if (what != 'I') {
                store(reg_, std::move(text), false, false, line_break(ed), true);
                remove();
            }
            ed.move_to(corner, false);
            enter_insert(ed, 1);
            ed.end_group();
            block_insert_ = {true, line0, line1, col0, false, false, what == 'I' ? static_cast<i64>(col0) : -1};
            break;
        case 'A': {
            ed.begin_group();
            const u64 end = ed.line_end(line0);
            const u32 width = ed.col_between(ed.buf.line_start(line0), end);
            if (to_end) {
                ed.move_to(end, false);
            } else if (width < col1) {  // a line that stops short of the block's right edge is filled out to it
                ed.replace(end, end, std::string(col1 - width, ' '));
                ed.move_to(end + (col1 - width), false);
            } else {
                ed.move_to(rows[0].second, false);
            }
            enter_insert(ed, 1);
            ed.end_group();
            block_insert_ = {true, line0, line1, col1, true, to_end, static_cast<i64>(col0)};
            break;
        }
        case '~':
        case 'u':
        case 'U':
        case '?':
        case 'r': {
            ed.begin_group();
            for (size_t i = rows.size(); i-- > 0;) {
                const std::string old = ed.buf.text(rows[i].first, rows[i].second - rows[i].first);
                std::string now;
                if (what == 'r') {
                    // As in Vim, every column of the block: a wide character
                    // gives two, a tab as many as it takes up (and a wide
                    // one put in fills two, a space making up an odd one).
                    const std::string with = utf8_of(arg);
                    const u64 start = ed.buf.line_start(line0 + i);
                    const u32 cols = ed.col_between(start, rows[i].second) - ed.col_between(start, rows[i].first);
                    const u32 w = std::max<u32>(1, columns_in(with, 0, ed.tab_width));
                    for (u32 k = cols / w; k > 0; --k) now += with;
                    now.append(cols % w, ' ');
                } else {
                    now = recase(old, what);
                }
                if (now != old) ed.replace(rows[i].first, rows[i].second, now);
            }
            ed.end_group();
            changing_ = true;
            go(ed, corner);
            break;
        }
        case '<':
        case '>': {
            // The text from the block's left edge on moves right by a level, or left over the white space there.
            const u32 levels = std::max<u32>(1, arg);
            ed.begin_group();
            for (size_t i = rows.size(); i-- > 0;) {
                const u64 at = rows[i].first, end = ed.line_end(line0 + i);
                if (at >= end) continue;  // the line stops short of the block
                if (what == '>') {
                    std::string by;
                    for (u32 k = 0; k < levels; ++k) by += shift;
                    ed.replace(at, at, by);
                } else {
                    Peek p(ed.buf);
                    const u32 width = (shift == "\t" ? ed.tab_width : static_cast<u32>(shift.size())) * levels;
                    u64 b = at;
                    for (u32 cols = 0; b < end && cols < width && (p.at(b) == ' ' || p.at(b) == '\t'); ++b) cols += p.at(b) == '\t' ? ed.tab_width : 1;
                    if (b > at && b < end && p.at(b) >= 0x80) b = ed.prev_char(ed.next_char(b));  // (a space with a mark on it stays, with its mark)
                    if (b > at) ed.replace(at, b, "");
                }
            }
            ed.end_group();
            changing_ = true;
            go(ed, corner);
            break;
        }
        case 'p': {
            if (fetch(reg_)->text.empty()) {
                message_ = "E353: Nothing in register";
                break;
            }
            ed.begin_group();
            remove();
            ed.move_to(corner, false);
            put(ed, true, 1);  // (it reads the register before anything is stored below)
            ed.end_group();
            regs_['"'] = std::make_shared<const Register>(Register{std::move(text), false, true});
            break;
        }
        default: break;
    }
}

void Vim::adopt_selection(Editor& ed) {
    if (mode_ == kInsert || mode_ == kReplace || mode_ == kCommand) return;
    if (ed.has_selection()) {
        const u64 lo = ed.sel_min(), hi = ed.sel_max();
        const bool forward = ed.cursor() == hi;
        mode_ = kVisual;
        vstart_ = forward ? lo : ed.prev_char(hi);
        ed.move_to(forward ? ed.prev_char(hi) : lo, false);
    } else if (visual()) {
        mode_ = kNormal;
    }
    go(ed, ed.cursor());
}

// ---------------------------------------------------------------- motions

// Where a motion key takes the cursor. False if the key is no motion.
bool Vim::motion(Editor& ed, u32 c, bool ctrl, u32 count, bool counted, Motion* out) {
    Motion& m = *out;
    const u64 cur = ed.cursor(), size = ed.buf.size(), line = ed.buf.line_of(cur), n = lines_of(ed);
    Peek p(ed.buf);
    auto to_line = [&](u64 l, bool column) {  // onto another line: the column wanted, or its first character that is not blank
        l = std::min(l, n - 1);
        m.linewise = true;
        m.keep_column = column;
        m.to = column ? std::min(ed.off_at(l, want_col_), line_last(ed, l)) : first_nonblank(ed, l);
    };
    auto word = [&](bool big, int how) {  // 0 w, 1 e, 2 b, 3 ge
        u64 at = cur;
        for (u32 i = 0; i < count; ++i) {
            if (how == 0) {
                const u64 from = at;
                const int k = kind_of(p.at(at), big);
                if (k != 0)
                    while (at < size && kind_of(p.at(at), big) == k) at = ed.next_char(at);
                while (at < size && kind_of(p.at(at), big) == 0) {
                    if (p.at(at) == '\n' && at > from && (at == 0 || p.at(at - 1) == '\n')) break;  // an empty line is a word
                    at = char_after(ed, p, at);
                }
            } else if (how == 1) {
                if (at < size) at = ed.next_char(at);
                while (at < size && kind_of(p.at(at), big) == 0) at = char_after(ed, p, at);
                const int k = kind_of(p.at(at), big);
                while (at < size && ed.next_char(at) < size && kind_of(p.at(ed.next_char(at)), big) == k) at = ed.next_char(at);
            } else if (how == 2) {
                if (at > 0) at = ed.prev_char(at);
                while (at > 0 && kind_of(p.at(at), big) == 0) {
                    if (p.at(at) == '\n' && p.at(at - 1) == '\n') break;
                    at = char_before(ed, p, at);
                }
                const int k = kind_of(p.at(at), big);
                while (k != 0 && at > 0 && kind_of(p.at(ed.prev_char(at)), big) == k) at = ed.prev_char(at);
            } else {
                const int k = kind_of(p.at(at), big);
                if (k != 0)
                    while (at > 0 && kind_of(p.at(at), big) == k) at = ed.prev_char(at);
                while (at > 0 && kind_of(p.at(at), big) == 0) {
                    if (p.at(at) == '\n' && p.at(at - 1) == '\n') break;
                    at = char_before(ed, p, at);
                }
            }
        }
        m.to = std::min(at, size);
        m.inclusive = how == 1 || how == 3;
    };
    if (ctrl) {
        if (c == 'h') c = kBack;
        else if (c == 'j' || c == 'n') c = 'j';
        else if (c == 'p') c = 'k';
        else if (c == 'm') c = '\r';
        else return false;
    }
    switch (c) {
        case 'h': {
            u64 at = cur;
            const u64 start = ed.buf.line_start(line);
            // (In Visual mode after "$" the cursor stands for the end of the line: the first step back is onto the last character.)
            const u32 steps = vis_eol_ && mode_ == kVisual && !op_ ? count - 1 : count;
            for (u32 i = 0; i < steps && at > start; ++i) at = ed.prev_char(at);
            m.ok = at != cur || steps != count;
            m.to = at;
            break;
        }
        case ' ':
        case kBack: {
            // Space and Backspace are l and h that go on to the next line and
            // back to the one before ('whichwrap' is "b,s" in Neovim), but
            // not with an operator.
            u64 at = cur;
            for (u32 i = 0; i < count; ++i) {
                const u64 l = ed.buf.line_of(at);
                if (c == ' ') {
                    if (at < (op_ ? ed.line_end(l) : line_last(ed, l))) at = ed.next_char(at);
                    else if (!op_ && l + 1 < n) at = ed.buf.line_start(l + 1);
                    else break;
                } else {
                    if (i == 0 && vis_eol_ && mode_ == kVisual && !op_) continue;  // (as for "h")
                    if (at > ed.buf.line_start(l)) at = ed.prev_char(at);
                    else if (!op_ && l > 0) at = line_last(ed, l - 1);
                    else break;
                }
            }
            m.ok = at != cur || (c == kBack && vis_eol_ && mode_ == kVisual && !op_);
            m.to = at;
            break;
        }
        case 'l': {
            // (With an operator the move may end just past the last character: "dl" there deletes it.)
            const u64 limit = op_ ? ed.line_end(line) : line_last(ed, line);
            u64 at = cur;
            for (u32 i = 0; i < count && at < limit; ++i) at = ed.next_char(at);
            m.ok = at != cur;
            m.to = at;
            break;
        }
        case 'j':
        case 'k':
        case kG | 'j':
        case kG | 'k': {
            const bool down = (c & 0xFF) == 'j';
            const LineSkip* s = ed.skip;
            if (s && (c & kG)) {  // gj, gk: by rows (a wrapped line's parts each one), along the row
                const u32 col = ed.col_of(cur);
                const u64 row = s->skip_row_at(line, col), rows = s->skip_rows();
                if (down ? row + 1 >= rows : row == 0) m.ok = false;
                const u64 to = down ? std::min(rows - 1, row + count) : (row >= count ? row - count : 0);
                const u32 start = s->skip_col(to), end = s->skip_end(to);
                u32 at = start + (col - std::min(col, s->skip_col(row)));
                if (end != ~0u && at >= end) at = end > start ? end - 1 : start;
                const u64 l = std::min<u64>(s->skip_line(to), n - 1);
                m.to = std::min(ed.off_at(l, at), line_last(ed, l));
                break;
            }
            if (s) {  // (by lines on show: a fold's are passed over, a wrapped line's later rows too)
                u64 l = line;
                for (u32 i = 0; i < count; ++i) {
                    const u64 r = down ? s->skip_row_at(l, ~0u) : s->skip_row(l);
                    if (down ? r + 1 >= s->skip_rows() : r == 0) {
                        if (i == 0) m.ok = false;
                        break;
                    }
                    l = s->skip_line(down ? r + 1 : r - 1);
                }
                to_line(l, true);
                break;
            }
            if (down ? line + 1 >= n : line == 0) m.ok = false;
            to_line(down ? line + count : (line >= count ? line - count : 0), true);
            break;
        }
        case '+':
        case '\r':
            if (line + 1 >= n) m.ok = false;
            to_line(line + count, false);
            break;
        case '-':
            if (line == 0) m.ok = false;
            to_line(line >= count ? line - count : 0, false);
            break;
        // (_ $ and g_ with a count go down count - 1 lines, and on the last line fail.)
        case '_':
            if (count > 1 && line + 1 >= n) m.ok = false;
            to_line(line + count - 1, false);
            break;
        case '0': m.to = ed.buf.line_start(line); break;
        case '^': m.to = first_nonblank(ed, line); break;
        case '$': {
            if (count > 1 && line + 1 >= n) m.ok = false;
            const u64 l = std::min(line + count - 1, n - 1);
            m.to = line_last(ed, l);
            m.inclusive = ed.line_end(l) > ed.buf.line_start(l);  // (an empty line has nothing to include)
            want_col_ = kEndCol;
            m.keep_column = true;
            break;
        }
        case kG | '_': {
            if (count > 1 && line + 1 >= n) m.ok = false;
            const u64 l = std::min(line + count - 1, n - 1);
            u64 at = line_last(ed, l);
            const u64 start = ed.buf.line_start(l);
            while (at > start && (p.at(at) == ' ' || p.at(at) == '\t')) at = ed.prev_char(at);
            m.to = at;
            m.inclusive = true;
            break;
        }
        case '|': m.to = std::min(ed.off_at(line, count - 1), line_last(ed, line)); break;
        case 'w': word(false, 0); break;
        case 'W': word(true, 0); break;
        case 'e': word(false, 1); break;
        case 'E': word(true, 1); break;
        case 'b': word(false, 2); break;
        case 'B': word(true, 2); break;
        case kG | 'e': word(false, 3); break;
        case kG | 'E': word(true, 3); break;
        // (These keep the cursor's column, as Neovim does: its 'startofline' is off.)
        case 'G': to_line(counted ? count - 1 : n - 1, true), m.jump = true; break;
        case kG | 'g': to_line(counted ? count - 1 : 0, true), m.jump = true; break;
        case 'H': to_line(view_top + count - 1, true), m.jump = true; break;
        case 'L': to_line(view_top + (view_rows > count ? view_rows - count : 0), true), m.jump = true; break;
        case 'M': to_line(view_top + view_rows / 2, true), m.jump = true; break;
        case kG | '0': m.to = ed.buf.line_start(line); break;
        case kG | '^': m.to = first_nonblank(ed, line); break;
        case kG | '$': m.to = line_last(ed, line), m.inclusive = ed.line_end(line) > ed.buf.line_start(line); break;
        case ';':
        case ',': {
            if (!find_key_) return m.ok = false, true;
            // , goes the other way round
            const bool flip = c == ',';
            const bool forward = (find_key_ == 'f' || find_key_ == 't') != flip, till = find_key_ == 't' || find_key_ == 'T';
            const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
            const std::string want = utf8_of(find_char_);
            // The line is looked through a piece at a time, from the cursor outward: on a very long line a key must not cost the whole of it.
            constexpr u64 kPiece = 4096;
            // (Only where a character starts: the second emoji of a family or
            // a mark on a letter is no character of its own to stop on. A
            // letter with marks on it is found by the letter, as in Vim.)
            auto scan_forward = [&](u64 from) {  // where the character first is at or after `from`
                for (u64 a = from; a < end; a += kPiece) {
                    const u64 b = std::min(end, a + kPiece + want.size() - 1);
                    const std::string piece = ed.buf.text(a, b - a);
                    for (size_t hit = piece.find(want); hit != std::string::npos; hit = piece.find(want, hit + 1))
                        if (a + hit < std::min(end, a + kPiece) && at_char(ed, p, a + hit)) return a + hit;
                    if (b == end) break;
                }
                return kNone;
            };
            auto scan_back = [&](u64 upto) {  // where it last is at or before `upto`
                for (u64 b = std::min(end, upto + want.size()); b > start;) {
                    const u64 a = b - start > kPiece + want.size() - 1 ? b - (kPiece + want.size() - 1) : start;
                    const std::string piece = ed.buf.text(a, b - a);
                    for (size_t hit = piece.rfind(want); hit != std::string::npos; hit = hit > 0 ? piece.rfind(want, hit - 1) : std::string::npos)
                        if (a + hit <= upto && at_char(ed, p, a + hit)) return a + hit;
                    if (a == start) break;
                    b = a + want.size() - 1;
                }
                return kNone;
            };
            u64 at = cur;
            for (u32 i = 0; i < count; ++i) {
                // (A repeated "t" already stands before one: look past it.)
                u64 from = at;
                if (till && repeat_find_ && i == 0) from = forward ? std::min(end, ed.next_char(at)) : (at > start ? ed.prev_char(at) : start);
                u64 hit = kNone;
                if (forward) hit = scan_forward(std::min(end, from + 1));
                else if (from > start) hit = scan_back(from - 1);
                if (hit == kNone) return m.ok = false, true;
                at = hit;
            }
            u64 to = at;
            if (till) to = forward ? ed.prev_char(to) : ed.next_char(to);
            m.to = to;
            m.inclusive = forward;
            break;
        }
        case '%': {
            if (counted) {  // with a count: that far through the text, in lines ("50%" the middle), the column kept
                if (count > 100) return m.ok = false, true;
                to_line(std::max<u64>(1, (static_cast<u64>(count) * n + 99) / 100) - 1, true);
                m.jump = true;
                break;
            }
            const u64 end = ed.line_end(line);
            u64 at = cur;
            static const char pairs[] = "()[]{}";
            const char* which = nullptr;
            for (; at < end; ++at)
                if (p.at(at) && (which = std::strchr(pairs, p.at(at)))) break;
            if (!which || !*which) return m.ok = false, true;
            const bool open = (which - pairs) % 2 == 0;
            const char self = *which, mate = open ? which[1] : which[-1];
            int depth = 0;
            constexpr u64 kFar = 4 << 20;  // give up looking after this many bytes
            m.ok = false;
            for (u64 i = at, steps = 0; steps < kFar && (open ? i < size : true); ++steps) {
                const char ch = static_cast<char>(p.at(i));
                if (ch == self) ++depth;
                else if (ch == mate && --depth == 0) {
                    m.to = i;
                    m.ok = true;
                    break;
                }
                if (!open && i == 0) break;
                open ? ++i : --i;
            }
            m.inclusive = true;
            m.jump = true;
            break;
        }
        case '}':
        case '{': {
            const bool down = c == '}';
            // Line starts, not line numbers: the empty lines are found with
            // memchr rather than a line at a time.
            u64 s = ed.buf.line_start(line);
            bool to_end = false;  // no empty line further down: to the end of the last line
            for (u32 i = 0; i < count && !to_end; ++i) {
                // Off the blank lines the cursor may be on, then on to the next one.
                if (down) {
                    while (s < size && blank_at(ed.buf, s)) {
                        const u64 next = s + (p.at(s) == '\r' ? 2 : 1);
                        if (next >= size) break;  // (the nothing after a final newline is not a line)
                        s = next;
                    }
                    const u64 b = next_blank(ed.buf, s, size);
                    if (b == kNone) to_end = true;
                    else s = b;
                } else {
                    while (s > 0 && blank_at(ed.buf, s)) {
                        u64 j = s - 1;
                        while (j > 0 && p.at(j - 1) != '\n') --j;
                        s = j;
                    }
                    const u64 b = s > 0 ? prev_blank(ed.buf, s) : kNone;
                    s = b == kNone ? 0 : b;
                    if (b == kNone && i + 1 < count) m.ok = false;
                }
                if (to_end && i + 1 < count) m.ok = false;  // (an end of the text with paragraphs still to go: Vim does nothing)
            }
            if (to_end) {
                const u64 l = n - 1;
                const bool empty = ed.line_end(l) == ed.buf.line_start(l);
                m.to = empty ? ed.buf.line_start(l) : line_last(ed, l);
                m.inclusive = !empty;
            } else {
                m.to = s;
            }
            m.jump = true;
            break;
        }
        case '(':
        case ')': {
            // To where a sentence starts: the next one, or this one (and then the one before).
            Walk w{ed, p, cur};
            m.ok = find_sentence(w, c == ')', count);
            m.to = w.at;  // (at the end of the last line: with an operator, up to it; without, the cursor stops on its last character)
            m.jump = true;
            break;
        }
        case kLB | '(':
        case kLB | '{':
        case kRB | ')':
        case kRB | '}': {
            // Out to the bracket the cursor is inside of.
            const bool back = (c & kLB) != 0;
            const char want = static_cast<char>(c & 0xFF);
            const char mate = want == '(' ? ')' : want == '{' ? '}' : want == ')' ? '(' : '{';
            constexpr u64 kFar = 4 << 20;
            u64 at = cur;
            for (u32 i = 0; i < count; ++i) {
                int depth = 0;
                u64 hit = kNone;
                for (u64 j = at, steps = 0; steps < kFar; ++steps) {
                    if (back ? j == 0 : j + 1 >= size) break;
                    back ? --j : ++j;
                    const char ch = static_cast<char>(p.at(j));
                    if (ch == mate) {
                        ++depth;
                    } else if (ch == want && depth-- == 0) {
                        hit = j;
                        break;
                    }
                }
                if (hit == kNone) break;
                at = hit;
            }
            m.to = at;
            m.ok = at != cur;
            break;
        }
        case kLB | '[':
        case kRB | ']':
        case kLB | ']':
        case kRB | '[': {
            // To a "{" at the start of a line ("[[" back, "]]" on), or a "}" there ("[]" and "][").
            const bool back = (c & kLB) != 0;
            const char want = (c & 0xFF) == (back ? '[' : ']') ? '{' : '}';
            // (Vi's way, which Vim keeps: "]]" after an operator stops at a "}" there too, and takes its line in.)
            const bool both = op_ && !back && want == '{';
            // A newline with the bracket after it, looked for as text.
            Pattern nl_want, nl_close;
            nl_want.text = {'\n', want};
            nl_close.text = {'\n', '}'};
            const u64 here = ed.buf.line_start(line);
            u64 s = here;
            for (u32 i = 0; i < count; ++i) {
                u64 hit = s == 0 && back ? kNone : find(ed.buf, nl_want, s - 1, !back, false);
                if (both) hit = std::min(hit, find(ed.buf, nl_close, s - 1, true, false));
                if (hit == kNone || (!back && hit + 1 >= ed.buf.line_start(n))) {
                    s = back ? 0 : ed.buf.line_start(n - 1);  // (to the first or last line; with more to go, nowhere)
                    if (i + 1 < count) m.ok = false;
                    break;
                }
                s = hit + 1;
            }
            // The line with the "}" is taken in: to the start of the next
            // (if it was the last line, to the start of that, as Vim's
            // cursor ends up there).
            bool past = false;
            if (both && p.at(s) == '}') {
                if (ed.buf.line_of(s) + 1 < n) s = ed.buf.line_start(ed.buf.line_of(s) + 1);
                else past = true;
                if (past && s == here) m.ok = false;  // (from that line itself: nowhere, which Neovim gets wrong and stops at, E292)
            }
            const u64 sl = ed.buf.line_of(s);
            m.to = s;
            if (!op_) m.to = first_nonblank(ed, sl);
            else if (!back && want == '{' && !past && sl + 1 == n && ed.line_end(sl) > s) m.to = line_last(ed, sl), m.inclusive = true;  // (the last line: all of it, as Vim has it)
            m.jump = true;
            break;
        }
        case 'n':
        case 'N':
        case '*':
        case '#':
        case kG | '*':
        case kG | '#': {
            const u32 key = c & 0xFF;
            if (key == '*' || key == '#') {
                // The word under the cursor (or the next one on the line), as
                // a whole word; with no word from there on, the signs there
                // (or next), to the next blank, as they stand (as Vim does).
                u64 at = cur;
                const u64 end = ed.line_end(line);
                while (at < end && kind_of(p.at(at), false) != 2) ++at;
                const bool keyword = at < end;
                if (!keyword)
                    for (at = cur; at < end && kind_of(p.at(at), false) == 0;) ++at;
                if (at >= end) {
                    message_ = "E348: No string under cursor";
                    return m.ok = false, true;
                }
                const int kind = kind_of(p.at(at), false);
                u64 lo = at, hi = at;
                while (lo > ed.buf.line_start(line) && kind_of(p.at(lo - 1), false) == kind) --lo;
                while (hi < end && (keyword ? kind_of(p.at(hi), false) == 2 : kind_of(p.at(hi), false) != 0)) ++hi;
                // (A mark on the sign or space before the word is that character's, not the word's.)
                if (!at_char(ed, p, lo)) lo = ed.next_char(ed.prev_char(lo));
                if (lo >= hi) return m.ok = false, true;
                pattern_ = Pattern{ed.buf.text(lo, hi - lo), false, keyword && c == key, nullptr, {}};  // (g* and g#: also as part of a longer word)
                search_forward_ = key == '*';
                ed.move_to(lo, false);
            }
            const bool forward = (key == 'n' || key == '*') ? search_forward_ : (key == '#' ? false : !search_forward_);
            const u64 before = ed.cursor();
            m.ok = search(ed, key == '#' ? false : forward, count);
            m.to = ed.cursor();
            m.jump = true;
            ed.move_to(key == '*' || key == '#' ? cur : before, false);
            break;
        }
        default: return false;
    }
    return true;
}

// The text an object key names around the cursor ("iw", "a(", "ip").
bool Vim::text_object(Editor& ed, u32 c, bool around, u32 count, u64* lo, u64* hi, bool* linewise) {
    count = std::max<u32>(1, count);
    const u64 cur = ed.cursor(), size = ed.buf.size(), line = ed.buf.line_of(cur);
    const u64 ls = ed.buf.line_start(line), le = ed.line_end(line);
    Peek p(ed.buf);
    *linewise = false;
    switch (c) {
        case 'w':
        case 'W': {
            // Vim's current_word(): the word (or the blanks) the cursor is
            // in, for "aw" with the blanks after it (or, if there are none,
            // those before it), and with a count that many more, going on
            // past the ends of lines; with fewer left, nothing.
            const bool big = c == 'W';
            Walk w{ed, p, cur};
            u64 start = cur;
            bool begun = false;  // the first is found
            auto fail = [&] {
                // (The cursor goes where it got to, as in Vim; in Visual
                // mode the selection with it, from where it started.)
                if (visual()) return walked(ed, w, begun ? start : cur, true, false, lo, hi, linewise);
                go(ed, w.at);
                return false;
            };
            back_in_line(w, big);
            start = w.at;
            bool inclusive = true, blanks_before = false;
            if ((w.cls(big) == 0) == around) {
                if (!end_word(w, big, true)) return fail();
            } else {
                fwd_word(w, big);
                if (w.bol()) w.decl();
                else w.left();
                blanks_before = around;
            }
            begun = true;
            for (u32 i = 1; i < count; ++i) {
                inclusive = true;
                if (w.incl() == -1) return fail();
                if (around != (w.cls(big) == 0)) {
                    if (!fwd_word(w, big) && i + 1 < count) return fail();
                    if (!w.left()) inclusive = false;  // (on the next line: up to it)
                } else if (!end_word(w, big, true)) {
                    return fail();
                }
            }
            if (blanks_before && (w.cls(big) != 0 || (w.bol() && !inclusive))) {
                // No blanks after it: the ones before it (not an indent).
                Walk b{ed, p, start};
                if (b.left()) {
                    back_in_line(b, big);
                    if (b.cls(big) == 0 && !b.bol()) start = b.at;
                }
            }
            return walked(ed, w, start, inclusive, !visual(), lo, hi, linewise);
        }
        case 's': {
            // Vim's current_sent(): the sentence the cursor is in (or the
            // blanks before the next), for "as" with the blanks after it
            // (or before it); with a count, that many more, sentences and
            // blanks taken in turn.
            Walk w{ed, p, cur};
            u64 start = cur;
            find_sentence(w, true, 1);
            Walk b{ed, p, cur};
            while (b.white()) b.incl();
            const bool on_blanks = b.at == w.at;  // the cursor was on the blanks before the next sentence
            if (on_blanks) {
                Walk s{ed, p, start};
                first_blank(s);
                start = s.at;
            } else {
                find_sentence(w, false, 1);
                start = w.at;
            }
            const u32 halves = around ? count * 2 : count - (on_blanks ? 1 : 0);
            if (halves > 0) sentences_on(w, halves, true);
            else w.decl();
            if (around) {
                if (on_blanks) {
                    first_blank(w);
                    if (w.white()) w.decl();
                } else if (!w.white()) {
                    Walk s{ed, p, start};
                    first_blank(s);
                    start = s.at;
                }
            }
            if (visual() && start != w.at) return walked(ed, w, start, true, false, lo, hi, linewise);  // (Visual mode: up to the cursor, with it)
            if (visual()) {
                // Stuck where it began ("is" on a blank before a sentence):
                // as Vim does, on (or back) from where Visual mode began instead.
                Walk to{ed, p, start};
                u32 times = around ? count * 2 : count;
                if (start < cur) {
                    bool at_start = true;
                    while (b.at < to.at) {
                        if (!b.white()) {
                            at_start = false;
                            break;
                        }
                        if (b.incl() == -1) break;
                    }
                    if (!at_start) {
                        find_sentence(to, false, 1);
                        if (to.at == start) at_start = true;
                        else find_sentence(to, true, 1);
                    }
                    while (times--) {
                        if (at_start) first_blank(to);
                        if (!at_start || (!around && !to.white())) find_sentence(to, false, 1);
                        at_start = !at_start;
                    }
                } else {
                    b.incl();
                    bool at_start = true;
                    if (b.at != to.at) {
                        at_start = false;
                        while (b.at < to.at) {
                            if (!b.white()) {
                                at_start = true;
                                break;
                            }
                            if (b.incl() == -1) break;
                        }
                        if (at_start) find_sentence(to, false, 1);
                        else to.at = start;
                    }
                    sentences_on(to, times, at_start);
                }
                return walked(ed, to, cur, true, false, lo, hi, linewise);
            }
            const bool inclusive = w.incl() == -1;  // (the line break after it, if there is one)
            return walked(ed, w, start, inclusive, true, lo, hi, linewise);
        }
        case 't': {
            // The tags around the cursor, "<a ...>" to "</a>". Looked for in
            // the text near by: tags are paired off as they close, and the
            // innermost pair the cursor is in is the one (the count-th, going out).
            constexpr u64 kSpan = 256 << 10;
            const u64 from = cur > kSpan ? cur - kSpan : 0;
            const std::string text = ed.buf.text(from, std::min(size, cur + kSpan) - from);
            const size_t at = static_cast<size_t>(cur - from);
            struct Tag {
                size_t a, b;
                std::string_view name;
            };
            std::vector<Tag> open;
            u32 left = count;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '<') continue;
                const bool closing = i + 1 < text.size() && text[i + 1] == '/';
                const size_t n0 = i + (closing ? 2 : 1);
                size_t n1 = n0;
                while (n1 < text.size() && (std::isalnum(static_cast<u8>(text[n1])) || std::strchr("-_:.", text[n1]))) ++n1;
                if (n1 == n0 || !text[n1 - 1]) continue;  // a "<" that starts no tag
                const size_t gt = text.find('>', n1);
                if (gt == std::string::npos) break;
                const std::string_view name(text.data() + n0, n1 - n0);
                if (closing) {
                    // It closes the nearest one open of that name. (Those opened after that one had no end.)
                    for (size_t k = open.size(); k-- > 0;) {
                        if (open[k].name != name) continue;
                        if (open[k].a <= at && at <= gt && --left == 0) {
                            *lo = from + (around ? open[k].a : open[k].b);
                            *hi = from + (around ? gt + 1 : i);
                            return true;
                        }
                        open.resize(k);
                        break;
                    }
                } else if (text[gt - 1] != '/') {  // ("<br/>" stands alone)
                    open.push_back({i, gt + 1, name});
                }
                i = gt;
            }
            return false;
        }
        case '(': case ')': case 'b':
        case '[': case ']':
        case '{': case '}': case 'B':
        case '<': case '>': {
            const char open = c == '(' || c == ')' || c == 'b' ? '(' : c == '[' || c == ']' ? '[' : c == '<' || c == '>' ? '<' : '{';
            const char close = open == '(' ? ')' : open == '[' ? ']' : open == '<' ? '>' : '}';
            constexpr u64 kFar = 4 << 20;
            // Back to the bracket that opens around the cursor (with a count: around that, and so on), forward to the one that closes it.
            u64 a = kNone;
            int depth = 0;
            u64 from = cur;
            for (u32 round = 0; round < count; ++round) {
                a = kNone;
                depth = 0;
                for (u64 i = from, steps = 0; steps < kFar; ++steps) {
                    const char ch = static_cast<char>(p.at(i));
                    if (ch == close && (i != cur || round > 0)) ++depth;
                    else if (ch == open && depth-- == 0) {
                        a = i;
                        break;
                    }
                    if (i == 0) break;
                    --i;
                }
                if (a == kNone || (round + 1 < count && a == 0)) return false;
                from = a - 1;
            }
            u64 b = kNone;
            depth = 0;
            for (u64 i = a + 1, steps = 0; i < size && steps < kFar; ++i, ++steps) {
                const char ch = static_cast<char>(p.at(i));
                if (ch == open) ++depth;
                else if (ch == close && depth-- == 0) {
                    b = i;
                    break;
                }
            }
            if (b == kNone) return false;
            if (around) {
                *lo = a;
                *hi = b + 1;
            } else {
                *lo = a + 1;
                *hi = b;
                // A block on lines of its own: what is inside is those lines, whole.
                if (p.at(a + 1) == '\n' && first_nonblank(ed, ed.buf.line_of(b)) == b && ed.buf.line_start(ed.buf.line_of(b)) > a + 2) {
                    *lo = a + 2;
                    *hi = ed.buf.line_start(ed.buf.line_of(b));
                    *linewise = true;
                }
            }
            return true;
        }
        case '"':
        case '\'':
        case '`': {
            // Quotes pair off from the start of the line: the pair around the
            // cursor, or the next pair after it.
            const std::string text = ed.buf.text(ls, le - ls);
            const size_t at = static_cast<size_t>(cur - ls);
            size_t a = std::string::npos, b = std::string::npos;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\\') {
                    ++i;
                    continue;
                }
                if (text[i] != static_cast<char>(c)) continue;
                if (a == std::string::npos) {
                    a = i;
                } else {
                    b = i;
                    if (b >= at) break;
                    a = b = std::string::npos;
                }
            }
            if (a == std::string::npos || b == std::string::npos) return false;
            if (around) {
                size_t e = b + 1;
                while (e < text.size() && (text[e] == ' ' || text[e] == '\t')) ++e;
                e = std::max(b + 1, keep_marked(text, e));
                if (e == b + 1)
                    while (a > 0 && (text[a - 1] == ' ' || text[a - 1] == '\t')) --a;
                *lo = ls + a;
                *hi = ls + e;
            } else {
                *lo = ls + a + 1;
                *hi = ls + b;
            }
            return true;
        }
        case 'p': {
            const u64 n = lines_of(ed);
            auto blank = [&](u64 l) { return first_nonblank(ed, l) == ed.line_end(l); };  // (only blanks, or nothing: Vim's linewhite())
            // Vim's current_par(): the paragraph (or the empty lines) the
            // cursor is in, for "ap" with the empty lines after it (or, if
            // there are none, those before it); with a count, that many
            // more, a paragraph and its empty lines at a time for "ap"; with
            // fewer left, nothing.
            const bool on_blank = blank(line);
            u64 a = line;
            while (a > 0 && blank(a - 1) == on_blank) --a;
            i64 b = static_cast<i64>(a);
            while (static_cast<u64>(b) < n && blank(static_cast<u64>(b))) ++b;
            --b;
            auto empty_after = [&](i64 l) { return static_cast<u64>(l + 1) < n && blank(static_cast<u64>(l + 1)); };
            for (u32 i = count - (!around && on_blank ? 1 : 0); i-- > 0;) {
                if (static_cast<u64>(b + 1) == n) return false;
                const bool to_blanks = !around && empty_after(b);  // ("2ip" on the last line of one: the empty lines are the second)
                if (around || !to_blanks) {
                    ++b;
                    while (static_cast<u64>(b + 1) < n && !blank(static_cast<u64>(b + 1))) ++b;
                }
                if (i == 0 && on_blank && around) break;
                if (around || to_blanks)
                    while (empty_after(b)) ++b;
            }
            const u64 last = static_cast<u64>(b);
            if (!on_blank && around && !blank(last))
                while (a > 0 && blank(a - 1)) --a;
            *lo = ed.buf.line_start(a);
            *hi = last + 1 < ed.buf.line_count() ? ed.buf.line_start(last + 1) : size;
            *linewise = true;
            return true;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------- operators

// Does an operator to the bytes [lo, hi). Linewise: they are whole lines.
void Vim::operate(Editor& ed, char op, u64 lo, u64 hi, bool linewise) {
    const u64 from = std::exchange(op_from_, kNone);
    if (hi < lo) std::swap(lo, hi);
    const u64 size = ed.buf.size();
    hi = std::min(hi, size);
    if (op != 'y' && refuse()) return;
    if (op == '<' || op == '>' || op == kOpComment || op == kOpFormat) {
        const u64 first = ed.buf.line_of(lo), last = hi > lo ? ed.buf.line_of(hi - 1) : first;
        if (op == kOpFormat) requests_.push_back({VimRequest::kFormat, {}, 0, false, false, {}, first, last, true});
        else if (op == kOpComment) toggle_comment(ed, first, last);
        else operate_lines(ed, op, first, last, true);
        return;
    }
    std::string text = ed.buf.text(lo, hi - lo);
    const bool open_end = linewise && (text.empty() || text.back() != '\n');  // the file's last line, with no newline of its own
    if (open_end) text += line_break(ed);
    const u64 cur = ed.cursor();
    switch (op) {
        case 'y':
            store(reg_, std::move(text), linewise, true, line_break(ed));
            // The cursor goes to where the text starts; over whole lines it keeps its column.
            if (!linewise) go(ed, lo);
            else if (ed.buf.line_of(cur) != ed.buf.line_of(lo)) go(ed, std::min(ed.off_at(ed.buf.line_of(lo), want_col_), line_last(ed, ed.buf.line_of(lo))), true);
            break;
        case 'd': {
            store(reg_, std::move(text), linewise, false, line_break(ed));
            // (Whole lines at the end of the file take the line break before them along.)
            if (open_end && lo > 0) lo = ed.line_end(ed.buf.line_of(lo) - 1);
            if (hi > lo) ed.replace(lo, hi, "");
            changing_ = true;
            if (linewise) {
                // On the line that takes their place, in the column the cursor was in ('startofline' is off).
                const u64 l = std::min(ed.buf.line_of(std::min(lo + (open_end && lo > 0 ? 1 : 0), ed.buf.size())), lines_of(ed) - 1);
                go(ed, std::min(ed.off_at(l, want_col_), line_last(ed, l)), true);
            } else {
                go(ed, lo);
            }
            break;
        }
        case 'c': {
            store(reg_, std::move(text), linewise, false, line_break(ed));
            ed.begin_group();
            if (linewise) {
                // The lines go, one empty line stays, with the indentation the first had.
                const std::string indent = indent_of(ed, ed.buf.line_of(lo));
                const u64 end = open_end ? hi : ed.line_end(ed.buf.line_of(hi - 1));  // (the last line's break stays, "\r" and all)
                ed.replace(lo, end, indent);
                ed.move_to(lo + indent.size(), false);
            } else {
                if (hi > lo) ed.replace(lo, hi, "");
                ed.move_to(lo, false);
            }
            enter_insert(ed, 1);
            auto_indent_ = linewise;
            ed.end_group();  // (the one opened here: Insert mode has its own, around all of it)
            break;
        }
        case '~':
        case 'u':
        case 'U':
        case '?': {
            if (open_end) text.resize(text.size() - std::strlen(line_break(ed)));  // (the line break put on the last line, "\r\n" in a CRLF text)
            const std::string changed = recase(text, op);
            if (changed != text) ed.replace(lo, lo + text.size(), changed);
            changing_ = true;
            // (On whole lines the cursor goes to the start of what the motion
            // went over, the column with it: "g~~" stays where it is.)
            go(ed, linewise && from >= lo && from < ed.buf.size() ? from : lo);
            break;
        }
        default: break;
    }
}

// > and <: one more level of indentation before each line, or one fewer.
void Vim::operate_lines(Editor& ed, char op, u64 line0, u64 line1, bool keep_column) {
    if (refuse()) return;
    line1 = std::min(line1, lines_of(ed) - 1);
    ed.begin_group();
    const u32 width = shift == "\t" ? ed.tab_width : static_cast<u32>(shift.size());
    if (line1 - line0 + 1 > kManyLines && (op == '>' || op == '<')) {
        rewrite_lines(ed, line0, line1, [&](std::string_view line, std::string& out) {
            if (line.empty()) return;  // an empty line stays empty
            size_t k = 0;
            if (op == '>') out += shift;
            else if (line[0] == '\t') k = 1;  // one tab, or as many spaces as a level is wide
            else
                while (k < line.size() && k < width && line[k] == ' ') ++k;
            k = keep_marked(line, k);
            out += line.substr(k);
        });
    }
    for (u64 l = line1 - line0 + 1 > kManyLines ? line0 : line1 + 1; l-- > line0;) {
        const u64 start = ed.buf.line_start(l), end = ed.line_end(l);
        if (end == start) continue;  // an empty line stays empty
        if (op == '>') {
            ed.replace(start, start, shift);
        } else {
            // One tab, or as many spaces as a level is wide.
            Peek p(ed.buf);
            u64 at = start;
            if (p.at(at) == '\t') ++at;
            else
                while (at < end && at - start < width && p.at(at) == ' ') ++at;
            if (at > start && at < end && p.at(at) >= 0x80) at = ed.prev_char(ed.next_char(at));  // (a space with a mark on it stays, with its mark)
            if (at > start) ed.replace(start, at, "");
        }
    }
    ed.end_group();
    changing_ = true;
    // (As an operator the cursor stays in its column on the first line; ":>" puts it on the text.)
    if (keep_column) go(ed, std::min(ed.off_at(line0, want_col_), line_last(ed, line0)), true);
    else go(ed, first_nonblank(ed, line0));
}

// Is the line a comment from its first character that is not blank? (Told
// by the markers without the spaces that go with them, so "//x" counts.)
bool Vim::commented(const Editor& ed, u64 line) const {
    const u64 start = ed.buf.line_start(line);
    const std::string text = ed.buf.text(start, ed.line_end(line) - start);
    const std::string_view body = trimmed(text), left = trimmed(comment_left), right = trimmed(comment_right);
    return !body.empty() && body.size() >= left.size() + right.size() && body.starts_with(left) && body.ends_with(right);
}

// gc: makes comments of lines line0..line1, or takes the markers off again
// if every one of them (blank lines aside) is a comment already. The way
// Neovim's own "gc" does it: the markers go in at the smallest indentation
// among the lines, so that they line up, and a blank line gets them without
// the spaces.
void Vim::toggle_comment(Editor& ed, u64 line0, u64 line1) {
    if (comment_left.empty()) {
        message_ = "Option 'commentstring' is empty.";
        return;
    }
    if (refuse()) return;
    line1 = std::min(line1, lines_of(ed) - 1);
    const u64 here = ed.buf.line_of(ed.cursor());
    const std::string_view left = comment_left, right = comment_right, left_bare = trimmed(left), right_bare = trimmed(right);
    auto line_text = [&](u64 l) {
        const u64 start = ed.buf.line_start(l);
        return ed.buf.text(start, ed.line_end(l) - start);
    };
    bool all = true;
    size_t indent = std::string::npos;  // of the least indented line that is not blank
    for (u64 l = line0; l <= line1; ++l) {
        const std::string text = line_text(l);
        const size_t in = text.find_first_not_of(" \t");
        if (in == std::string::npos) continue;
        indent = std::min(indent, in);
        if (all) all = commented(ed, l);
    }
    if (indent == std::string::npos) indent = 0;
    ed.begin_group();
    for (u64 l = line1 + 1; l-- > line0;) {
        const std::string text = line_text(l);
        const size_t in = text.find_first_not_of(" \t");
        std::string now;
        if (!all) {
            if (in == std::string::npos) now = text.substr(0, std::min(indent, text.size())) + std::string(left_bare) + std::string(right_bare);
            else now = text.substr(0, indent) + std::string(left) + text.substr(indent) + std::string(right);
        } else {
            if (in == std::string::npos) continue;
            // The markers as they are put in; failing that, without their spaces.
            std::string_view body = std::string_view(text).substr(in);
            size_t tail = body.size();
            while (tail > 0 && (body[tail - 1] == ' ' || body[tail - 1] == '\t')) --tail;
            const std::string_view trail = body.substr(tail);
            body = body.substr(0, tail);
            std::string_view inner;
            if (body.size() >= left.size() + right.size() && body.starts_with(left) && body.ends_with(right)) inner = body.substr(left.size(), body.size() - left.size() - right.size());
            else if (body.size() >= left_bare.size() + right_bare.size() && body.starts_with(left_bare) && body.ends_with(right_bare)) inner = body.substr(left_bare.size(), body.size() - left_bare.size() - right_bare.size());
            else continue;
            if (trimmed(inner).empty()) now.clear();  // nothing but the markers: an empty line is left
            else now = text.substr(0, in) + std::string(inner) + std::string(trail);
        }
        if (now != text) ed.replace(ed.buf.line_start(l), ed.line_end(l), now);
    }
    ed.end_group();
    changing_ = true;
    // The cursor stays in its column, on the first of the lines (where an operator leaves it).
    (void)here;
    const u64 l = std::min(line0, lines_of(ed) - 1);
    go(ed, std::min(ed.off_at(l, want_col_), line_last(ed, l)), true);
}

void Vim::put(Editor& ed, bool before, u32 count, bool then_after) {
    const auto held = fetch(reg_);
    const Register& r = *held;
    if (r.text.empty()) {
        message_ = "E353: Nothing in register";
        return;
    }
    if (refuse()) return;
    count = std::max<u32>(1, count);
    std::string many;  // (the register's text as it is, unless it goes in more than once)
    if (count > 1) {
        many.reserve(r.text.size() * count);
        for (u32 i = 0; i < count; ++i) many += r.text;
    }
    std::string_view text = count > 1 ? std::string_view(many) : std::string_view(r.text);
    const u64 cur = ed.cursor(), size = ed.buf.size(), line = ed.buf.line_of(cur);
    ed.begin_group();
    if (r.blockwise) {
        // A rectangle: each of its rows goes into a line of its own, from
        // the cursor's line down, at the cursor's column. Lines too short
        // for that are filled out with spaces, and past the last line new
        // ones are made.
        std::vector<std::string_view> rows;
        for (size_t i = 0;;) {
            const size_t e = r.text.find('\n', i);
            rows.push_back(std::string_view(r.text).substr(i, e == std::string::npos ? e : e - i));
            if (e == std::string::npos) break;
            i = e + 1;
        }
        const u64 start0 = ed.buf.line_start(line);
        const u32 col = ed.col_between(start0, before || ed.line_end(line) == start0 ? cur : ed.next_char(cur));
        // (The rows are made as wide as the widest on the screen: a wide character counts two, a mark nothing.)
        u32 width = 0;
        for (const std::string_view row : rows) width = std::max(width, columns_in(row, col, ed.tab_width));
        u64 first = cur;
        for (size_t i = 0; i < rows.size(); ++i) {
            const u64 l = line + i;
            if (l >= lines_of(ed)) ed.replace(ed.buf.size(), ed.buf.size(), line_break(ed));
            const u64 start = ed.buf.line_start(l), end = ed.line_end(l);
            const u32 have = ed.col_between(start, end);
            const u64 at = have < col ? end : ed.off_at(l, col);
            const size_t pad = have < col ? col - have : 0;
            std::string piece(pad, ' ');
            for (u32 k = 0; k < count; ++k) {
                piece += rows[i];
                if (k + 1 < count || at < end) piece.append(width - columns_in(rows[i], col, ed.tab_width), ' ');  // (nothing after it: no need to fill it out)
            }
            ed.replace(at, at, piece);
            if (i == 0) first = at + pad;
        }
        go(ed, first);
    } else if (r.linewise) {
        const u64 rows = static_cast<u64>(std::count(text.begin(), text.end(), '\n'));
        u64 at = before ? ed.buf.line_start(line) : (line + 1 < ed.buf.line_count() ? ed.buf.line_start(line + 1) : size);
        if (!before && at == size) {
            char last = size > 0 ? '\n' : 0;  // (nothing in the buffer: its one empty line comes first, as if it had no newline)
            if (size > 0) ed.buf.read(size - 1, &last, 1);
            if (last != '\n') {  // after a last line that has no newline: one before the text instead of after it
                const size_t cut = text.ends_with("\r\n") ? 2 : 1;
                std::string moved;
                moved.reserve(text.size() + 1);
                moved += line_break(ed);
                moved += text.substr(0, text.size() - cut);
                ed.replace(size, size, moved);
                at = size + std::strlen(line_break(ed));
                text = {};
            }
        }
        if (!text.empty()) ed.replace(at, at, text);
        const u64 top = ed.buf.line_of(std::min(at, ed.buf.size()));
        if (then_after) go(ed, ed.buf.line_start(std::min(top + rows, lines_of(ed) - 1)));  // gp: on the line after what went in
        else go(ed, first_nonblank(ed, top));
    } else {
        const u64 end = ed.line_end(line);
        const u64 at = before || end == ed.buf.line_start(line) ? cur : std::min(ed.next_char(cur), end);
        ed.replace(at, at, text);
        // On the last character of what went in (at its start, if it spans lines); gp: just after it.
        if (then_after) go(ed, at + text.size());
        else go(ed, text.find('\n') != std::string::npos ? at : ed.prev_char(at + text.size()));
    }
    ed.end_group();
    changing_ = true;
}

// ---------------------------------------------------------------- Insert mode

void Vim::enter_insert(Editor& ed, u32 count, Mode mode) {
    mode_ = mode;
    insert_count_ = std::max<u32>(1, count);
    insert_from_ = ed.cursor();
    insert_plain_ = true;
    insert_prefix_.clear();
    auto_indent_ = false;
    replaced_.clear();
    insert_keys_.clear();
    changing_ = true;
    // Everything typed until Esc is one step to undo. (After <C-o> in
    // Insert mode that step is open already, and this is still part of it.)
    if (!insert_once_) ed.begin_group();
    insert_once_ = 0;
}

// An indent that o, O, cc, S or Enter put in, with nothing typed after it,
// goes again (as Vim's 'autoindent' has it) when Insert mode ends, Enter is
// pressed, <C-o> or an arrow takes the cursor off the line: no line is left
// holding only blanks. `at` is where the cursor was on that line; true if
// there were blanks to take out.
bool Vim::drop_indent(Editor& ed, u64 at) {
    if (!auto_indent_) return false;
    auto_indent_ = false;
    const u64 line = ed.buf.line_of(at), start = ed.buf.line_start(line);
    if (at != ed.line_end(line)) return false;
    Peek p(ed.buf);
    u64 from = at;
    while (from > start && (p.at(from - 1) == ' ' || p.at(from - 1) == '\t')) --from;
    if (from == at) return false;
    const u64 cur = ed.cursor();
    ed.replace(from, at, "");
    ed.move_to(cur >= at ? cur - (at - from) : cur, false);
    return true;
}

// ---- indentation by the language's rules, with Vim and without it

std::string indent_after(const IndentRules& r, const Editor& ed, u64 line, u64 end, std::string indent, std::string_view unit, std::string_view marker) {
    const std::string code = code_of(ed, line, end, marker);
    if (opens(r, code)) return indent.append(unit);
    if (one_of(r.enders, first_word(code))) return outdent(ed, std::move(indent), unit);
    return indent;
}

bool closer_typed(const IndentRules& r, const Editor& ed, u64 sign, u32 ch) {
    if (ch >= 0x80 || word_byte(static_cast<char>(ch))) return false;
    // (Looked for in the few bytes before it, read at once: most signs are typed far from a line's start.)
    char w[64];
    const size_t n = static_cast<size_t>(std::min<u64>(sign, sizeof w));
    ed.buf.read(sign - n, w, n);
    size_t word = n, lead = n;
    while (word > 0 && n - word <= 16 && word_byte(w[word - 1])) --word;
    if (n - word > 16) return false;
    for (lead = word; lead > 0 && (w[lead - 1] == ' ' || w[lead - 1] == '\t');) --lead;
    const bool starts = lead > 0 ? w[lead - 1] == '\n' : sign == n || first_nonblank(ed, ed.buf.line_of(sign)) == sign - (n - word);
    if (!starts) return false;
    if (word < n) return closes(r, std::string_view(w + word, n - word));
    const char c = static_cast<char>(ch);
    return closes(r, std::string_view(&c, 1));
}

// A line that starts with a closer ("}", "end", Python's "else") goes a
// level further out than the code line above it, unless that one opened
// the level it closes ("if x then" and "end": as far in as each other).
// (The line above's own end of its level, Python's return, is the same
// level, not one more.) It only ever goes further out.
bool closer_indent(const IndentRules& r, const Editor& ed, u64 line, std::string_view unit, std::string_view marker, std::string* want) {
    if (line == 0) return false;
    const u64 start = ed.buf.line_start(line), text = first_nonblank(ed, line);
    if (!closes(r, code_of(ed, line, ed.line_end(line), marker))) return false;
    u64 above = line - 1;
    while (above > 0 && first_nonblank(ed, above) == ed.line_end(above)) --above;
    const u64 above_start = ed.buf.line_start(above), above_text = first_nonblank(ed, above);
    if (above_text == ed.line_end(above)) return false;  // (no code above it)
    *want = ed.buf.text(above_start, above_text - above_start);
    if (!opens(r, code_of(ed, above, ed.line_end(above), marker))) *want = outdent(ed, std::move(*want), unit);
    u32 cols = 0;
    for (const char c : *want) cols = c == '\t' ? (cols / ed.tab_width + 1) * ed.tab_width : cols + 1;
    return cols < ed.col_between(start, text);
}

bool indent_closer(const IndentRules& r, Editor& ed, u64 line, std::string_view unit, std::string_view marker, u64* follow) {
    std::string want;
    if (!closer_indent(r, ed, line, unit, marker, &want)) return false;
    const u64 start = ed.buf.line_start(line), text = first_nonblank(ed, line), cur = ed.cursor();
    auto moved = [&](u64 at) { return at >= text ? at - (text - start) + want.size() : std::min<u64>(at, start + want.size()); };
    ed.replace(start, text, want);
    ed.move_to(moved(cur), false);
    if (follow && *follow > start) *follow = moved(*follow);
    return true;
}

void Vim::indent_closer(Editor& ed, u64 line) {
    if (indent_rules) blit::indent_closer(*indent_rules, ed, line, shift, comment_left, &insert_from_);
}

void Vim::leave_insert(Editor& ed) {
    // A closer typed just now alone at the end of its line ("end", then
    // Esc): a level out, as Enter or a sign after it would have it.
    if (indent_rules && mode_ == kInsert) {
        const u64 cur = ed.cursor(), line = ed.buf.line_of(cur), text = first_nonblank(ed, line);
        if (cur == ed.line_end(line) && cur > text && cur - text <= 16 && insert_from_ <= text && closes(*indent_rules, ed.buf.text(text, cur - text))) indent_closer(ed, line);
    }
    // "3ix": the text, twice more; "3o": two more lines of it. Typed again
    // key by key, as Vim does, when it was all typing: each Enter puts in
    // its own indent and takes back the one before it, if that is alone on
    // its line ("3i<CR><Esc>" leaves the new lines empty, not indented).
    const bool by_keys = insert_count_ > 1 && mode_ == kInsert && insert_plain_;
    std::string typed;
    if (by_keys) {
        u64 end = ed.cursor();
        if (auto_indent_ && end == ed.line_end(ed.buf.line_of(end)))  // (what was typed, without an indent that Esc would take out)
            for (Peek p(ed.buf); end > insert_from_ && (p.at(end - 1) == ' ' || p.at(end - 1) == '\t');) --end;
        typed = end >= insert_from_ ? ed.buf.text(insert_from_, end - insert_from_) : std::string();
        retyping_ = true;
        for (u32 i = 1; i < insert_count_; ++i) {
            if (!insert_prefix_.empty()) insert_key(ed, VimKey{VimKey::kEnter, 0, false});  // ("3o": the next line, as Enter makes it)
            for (const VimKey& key : insert_keys_)
                if ((insert_key(ed, key) & kTyped) && !is_enter(key)) auto_indent_ = false;
            if (await_ == kInsertLiteral) {  // (a <C-v> number that Esc ended, the first time: ended here too)
                await_ = kNothing;
                if (literal_digits_ && literal_ <= 0x10FFFF) ed.insert(literal_ == 0 ? std::string(1, '\0') : utf8_of(literal_));
            }
        }
        retyping_ = false;
        last_inserted_ = typed;  // (after: a <C-a> or <C-r>. typed again puts in the insert before this one)
    }
    // ("3o<Esc>": the further lines are empty too. The column the cursor
    // keeps for j and k is still the one the indent went to.)
    const u32 indent_col = auto_indent_ ? ed.col_of(ed.cursor()) : 0;
    const bool dropped = drop_indent(ed, ed.cursor());
    if (!by_keys) {
        if (dropped && !insert_prefix_.empty()) insert_prefix_ = line_break(ed);
        typed = insert_plain_ && ed.cursor() >= insert_from_ ? ed.buf.text(insert_from_, ed.cursor() - insert_from_) : std::string();
        if (insert_plain_) last_inserted_ = typed;
        if (insert_count_ > 1 && !(insert_prefix_ + typed).empty()) {
            const std::string text = insert_prefix_ + typed;
            for (u32 i = 1; i < insert_count_; ++i) ed.insert(text);
        }
    }
    u64 block_home = kNone;
    if (block_insert_.on) {
        // Visual Block's I, A and c: what was typed on the first line goes
        // on the others too, at the same column.
        const BlockInsert b = block_insert_;
        block_insert_.on = false;
        if (!typed.empty() && b.home >= 0) block_home = std::min(ed.off_at(b.line0, static_cast<u32>(b.home)), ed.line_end(b.line0));
        if (!typed.empty() && typed.find('\n') == std::string::npos) {
            const u64 back_to = insert_from_;
            for (u64 l = b.line1; l > b.line0; --l) {
                const u64 start = ed.buf.line_start(l), end = ed.line_end(l);
                const u32 width = ed.col_between(start, end);
                if (b.to_end) {
                    ed.replace(end, end, typed);
                } else if (width >= b.col) {
                    const u64 at = ed.off_at(l, b.col);
                    ed.replace(at, at, typed);
                } else if (b.append) {  // a line that stops short of the block is filled out to it
                    ed.replace(end, end, std::string(b.col - width, ' ') + typed);
                }
            }
            ed.move_to(back_to + typed.size(), false);
        }
    }
    last_insert_at_ = ed.cursor();
    ed.end_group();
    mode_ = kNormal;
    await_ = kNothing;
    const u64 cur = ed.cursor();
    if (cur > ed.buf.line_start(ed.buf.line_of(cur))) ed.move_to(ed.prev_char(cur), false);  // Esc steps back onto the last character typed
    go(ed, block_home == kNone ? ed.cursor() : block_home);
    if (dropped) want_col_ = indent_col;
}

// <C-v> in Insert mode, as Vim's get_literal() has it: the next key as it
// is (Esc and Ctrl with a letter too, as control characters), or a
// character by its number: up to three decimal digits, "o" and three
// octal ones, "x" and two hex ones (to 255 at most), "u" and four hex
// ones or "U" and eight. A key that is no digit ends the number early and
// is then a key as usual.
u32 Vim::literal_key(Editor& ed, const VimKey& k) {
    auto keep = [&] {  // (as insert_key() keeps the keys a count types again)
        if (insert_count_ > 1 && !retyping_) insert_keys_.push_back(k);
    };
    auto put = [&](u32 cp) {
        await_ = kNothing;
        typed_ = cp == 0 ? std::string(1, '\0') : cp <= 0x10FFFF ? utf8_of(cp) : std::string();
        if (typed_.empty()) return static_cast<u32>(kHandled);
        ed.insert(typed_);
        return static_cast<u32>(kHandled) | static_cast<u32>(kTyped);
    };
    const u32 c = k.kind == VimKey::kChar && !k.ctrl ? k.ch : 0;
    if (c == 'x' || c == 'X') {
        literal_hex_ = true;
    } else if (c == 'o' || c == 'O') {
        literal_octal_ = true;
    } else if (c == 'u' || c == 'U') {
        literal_unicode_ = static_cast<char>(c);
    } else {
        int d = -1;
        if (literal_hex_ || literal_unicode_) d = c < 128 && std::isxdigit(static_cast<int>(c)) ? (c <= '9' ? static_cast<int>(c - '0') : (static_cast<int>(c) | 0x20) - 'a' + 10) : -1;
        else if (literal_octal_) d = c >= '0' && c <= '7' ? static_cast<int>(c - '0') : -1;
        else d = c >= '0' && c <= '9' ? static_cast<int>(c - '0') : -1;
        if (d < 0) {
            if (literal_digits_ == 0) {  // no number: this key, as it is
                keep();
                if (k.kind == VimKey::kTab) return put('\t');
                if (k.kind == VimKey::kEnter) return put('\n');
                if (k.kind == VimKey::kEsc) return put(0x1B);
                if (k.kind != VimKey::kChar) return put(~0u);
                return put(k.ctrl && k.ch >= '@' && k.ch < 0x80 ? (k.ch & 0x1F) : k.ch);
            }
            const u32 r = put(literal_);
            const std::string number = typed_;
            const u32 then = insert_key(ed, k);
            if (then & kTyped) typed_ = number + typed_;
            else typed_ = number;
            return r | then;
        }
        literal_ = literal_ * (literal_hex_ || literal_unicode_ ? 16 : literal_octal_ ? 8 : 10) + static_cast<u32>(d);
        ++literal_digits_;
    }
    keep();
    if (literal_ > 255 && !literal_unicode_) literal_ = 255;
    const u8 most = literal_hex_ ? 2 : literal_unicode_ == 'u' ? 4 : literal_unicode_ == 'U' ? 8 : 3;
    if (literal_digits_ >= most) return put(literal_);
    return kHandled;
}

u32 Vim::insert_key(Editor& ed, const VimKey& k) {
    if (await_ == kInsertLiteral && !read_only) return literal_key(ed, k);
    if (k.kind == VimKey::kEsc || (k.ctrl && (k.ch == '[' || k.ch == 'c'))) {
        leave_insert(ed);
        return kHandled;
    }
    if (read_only) return kHandled;
    if (insert_count_ > 1 && !retyping_) insert_keys_.push_back(k);  // (only with a count: they are to be typed again)
    if (await_ == kInsertRegister) {  // <C-r> and a register's name: its text, as if typed
        await_ = kNothing;
        if (k.kind != VimKey::kChar) return kHandled;
        typed_ = k.ch < 128 ? fetch(static_cast<char>(k.ch))->text : std::string();
        if (!typed_.empty()) ed.insert(typed_);
        return typed_.empty() ? static_cast<u32>(kHandled) : static_cast<u32>(kHandled) | static_cast<u32>(kTyped);
    }
    auto moved = [&] {  // the cursor went somewhere by other means than typing
        insert_plain_ = false;
        insert_from_ = ed.cursor();
    };
    if (k.ctrl) {
        // (Neovim's <C-u> and <C-w> start a new undo step first, so that "u" brings back what they took.)
        if ((k.ch == 'w' || k.ch == 'u') && ed.cursor() > 0) {
            ed.end_group();
            ed.begin_group();
        }
        switch (k.ch) {
            case 'w':
            case 'u': {
                // <C-w>: the blanks before the cursor and the word (or the
                // run of signs) before them; <C-u>: all of the line before
                // it but the indent. At the start of a line, the line break.
                // Neither goes back past the start of the line, nor past
                // where typing began, unless it began here.
                const u64 cur = ed.cursor(), line = ed.buf.line_of(cur), start = ed.buf.line_start(line);
                if (cur == start) {
                    ed.backspace();
                } else {
                    u64 floor = start;
                    if (k.ch == 'u' && first_nonblank(ed, line) < cur) floor = first_nonblank(ed, line);
                    if (insert_from_ > floor && insert_from_ < cur) floor = insert_from_;
                    u64 at = floor;
                    if (k.ch == 'w') {
                        Peek p(ed.buf);
                        at = cur;
                        while (at > floor && kind_of(p.at(char_before(ed, p, at)), false) == 0) at = char_before(ed, p, at);
                        const int kind = at > floor ? kind_of(p.at(char_before(ed, p, at)), false) : 0;
                        while (at > floor && kind_of(p.at(char_before(ed, p, at)), false) == kind) at = char_before(ed, p, at);
                    }
                    if (at < cur) ed.replace(at, cur, "");
                }
                insert_from_ = std::min(insert_from_, ed.cursor());
                break;
            }
            case 'h':
                ed.backspace();
                if (ed.cursor() < insert_from_) insert_from_ = ed.cursor();
                break;
            case 'r': await_ = kInsertRegister; break;
            case 'o': {
                drop_indent(ed, ed.cursor());
                insert_once_ = 2, mode_ = kNormal;
                insert_plain_ = false;
                once_line_ = ed.buf.line_of(ed.cursor());
                once_eol_ = ed.cursor() == ed.line_end(once_line_) && ed.cursor() > ed.buf.line_start(once_line_);
                break;
            }
            case 'v':
            case 'q':
                await_ = kInsertLiteral;
                literal_ = 0, literal_digits_ = 0, literal_hex_ = literal_octal_ = false, literal_unicode_ = 0;
                break;
            case 'a':
                if (!last_inserted_.empty()) {
                    typed_ = last_inserted_;
                    ed.insert(typed_);
                    return kHandled | kTyped;
                }
                break;
            case 't':
            case 'd': {
                // One level of indentation more, or one fewer, on this line.
                const u64 line = ed.buf.line_of(ed.cursor()), start = ed.buf.line_start(line);
                const u64 cur = ed.cursor();
                if (k.ch == 't') {
                    ed.replace(start, start, shift);
                    ed.move_to(cur + shift.size(), false);
                } else {
                    Peek p(ed.buf);
                    u64 at = start;
                    const u32 width = shift == "\t" ? ed.tab_width : static_cast<u32>(shift.size());
                    if (p.at(at) == '\t') ++at;
                    else
                        while (at < cur && at - start < width && p.at(at) == ' ') ++at;
                    if (at > start && at < cur && p.at(at) >= 0x80) at = ed.prev_char(ed.next_char(at));  // (a space with a mark on it stays)
                    if (at > start) {
                        ed.replace(start, at, "");
                        ed.move_to(cur - (at - start), false);
                    }
                }
                insert_plain_ = false;
                break;
            }
            case 'e':
            case 'y': {
                // The character under the cursor's place in the line below (or above), as if typed.
                const u64 line = ed.buf.line_of(ed.cursor());
                if (k.ch == 'y' ? line == 0 : line + 1 >= lines_of(ed)) break;
                const u64 other = k.ch == 'y' ? line - 1 : line + 1;
                const u32 col = ed.col_between(ed.buf.line_start(line), ed.cursor());
                if (ed.col_between(ed.buf.line_start(other), ed.line_end(other)) <= col) break;
                const u64 at = ed.off_at(other, col);
                typed_ = ed.buf.text(at, ed.next_char(at) - at);
                ed.insert(typed_);
                return kHandled | kTyped;
            }
            case 'j':
            case 'm': return insert_key(ed, VimKey{VimKey::kEnter, 0, false});
            case 'n':
            case 'p': return 0;  // a completion list's keys, if one is open
            default: break;
        }
        return kHandled;
    }
    switch (k.kind) {
        case VimKey::kChar: {
            typed_ = utf8_of(k.ch);
            const u64 cur = ed.cursor();
            if (mode_ == kReplace && cur < ed.line_end(ed.buf.line_of(cur))) {  // over the character there
                replaced_.push_back(ed.buf.text(cur, ed.next_char(cur) - cur));
                ed.replace(cur, ed.next_char(cur), typed_);
            } else {
                if (mode_ == kReplace) replaced_.emplace_back();
                ed.insert(typed_);
                // A closer just typed at the start of the line ("}", or a
                // word such as "end" or "else" with the sign or blank after
                // it): the line goes a level out.
                if (indent_rules && mode_ == kInsert) {
                    const u64 sign = ed.cursor() - typed_.size();
                    if (closer_typed(*indent_rules, ed, sign, k.ch)) indent_closer(ed, ed.buf.line_of(sign));
                }
            }
            return kHandled | kTyped;
        }
        case VimKey::kEnter: {
            // The new line starts as far in as this one does (autoindent),
            // at the text after the cursor: blanks before that go. With the
            // language's rules, a level further after a line that opens one
            // (and this line a level out first if it starts with a closer).
            if (indent_rules && mode_ == kInsert) indent_closer(ed, ed.buf.line_of(ed.cursor()));
            const u64 cur = ed.cursor(), line = ed.buf.line_of(cur), end = ed.line_end(line);
            const u64 start = ed.buf.line_start(line), text = std::min(first_nonblank(ed, line), cur);
            std::string indent = ed.buf.text(start, text - start);
            if (indent_rules && mode_ == kInsert && text < cur) indent = indent_after(*indent_rules, ed, line, cur, std::move(indent), shift, comment_left);
            Peek p(ed.buf);
            u64 skip = cur;
            while (skip < end && (p.at(skip) == ' ' || p.at(skip) == '\t')) ++skip;
            if (skip > cur && skip < end && p.at(skip) >= 0x80) skip = std::max(cur, ed.prev_char(ed.next_char(skip)));  // (a mark on the last blank keeps it)
            // A line that holds only the indent put in for it is left empty.
            const u64 from = mode_ == kInsert && auto_indent_ && text == cur ? start : cur;
            typed_ = line_break(ed);
            // (Replace mode: the line break and each character of the indent go over nothing, so Backspace takes them back out one at a time, the line break putting back the blanks it took.)
            if (mode_ == kReplace) {
                replaced_.push_back(ed.buf.text(cur, skip - cur));
                replaced_.resize(replaced_.size() + indent.size());
            }
            ed.replace(from, skip, typed_ + indent);
            insert_from_ = std::min(insert_from_, from);
            auto_indent_ = mode_ == kInsert;
            return kHandled | kTyped;
        }
        case VimKey::kTab: {
            // The file's indentation (what > puts in): a tab, or spaces to
            // the next level, as with 'expandtab' and 'softtabstop'.
            const u64 cur = ed.cursor();
            const u32 width = static_cast<u32>(shift.size());
            typed_ = shift == "\t" || shift.empty() ? std::string("\t") : std::string(width - ed.col_between(ed.buf.line_start(ed.buf.line_of(cur)), cur) % width, ' ');
            if (mode_ == kReplace) replaced_.resize(replaced_.size() + typed_.size());
            ed.insert(typed_);
            return kHandled | kTyped;
        }
        case VimKey::kBackspace:
            if (mode_ == kReplace) {
                // Back over what was typed, putting back what it went over.
                if (!replaced_.empty() && ed.cursor() > insert_from_) {
                    const u64 at = ed.prev_char(ed.cursor());
                    ed.replace(at, ed.cursor(), replaced_.back());
                    ed.move_to(at, false);
                    replaced_.pop_back();
                } else if (ed.cursor() > ed.buf.line_start(ed.buf.line_of(ed.cursor()))) {
                    ed.left(false);
                    insert_from_ = std::min(insert_from_, ed.cursor());
                }
                break;
            }
            ed.backspace();
            if (ed.cursor() < insert_from_) insert_from_ = ed.cursor();
            break;
        case VimKey::kDelete:
            ed.del();
            insert_plain_ = false;
            auto_indent_ = false;
            break;
        // (The arrows stay on the line, as Vim's 'whichwrap' has them.)
        case VimKey::kLeft:
            if (ed.cursor() == ed.buf.line_start(ed.buf.line_of(ed.cursor()))) return kHandled;
            ed.left(false), moved();
            break;
        case VimKey::kRight:
            if (ed.cursor() >= ed.line_end(ed.buf.line_of(ed.cursor()))) return kHandled;
            ed.right(false), moved();
            break;
        case VimKey::kHome: ed.home(false), moved(); break;
        case VimKey::kEnd: ed.end(false), moved(); break;
        case VimKey::kUp:
        case VimKey::kDown:
        case VimKey::kPageUp:
        case VimKey::kPageDown: {
            const u64 was = ed.cursor();
            const i64 page = std::max<u32>(1, view_rows - 1);
            ed.vertical(k.kind == VimKey::kUp ? -1 : k.kind == VimKey::kDown ? 1 : k.kind == VimKey::kPageUp ? -page : page, false);
            if (ed.buf.line_of(ed.cursor()) != ed.buf.line_of(was)) drop_indent(ed, was);  // (off the line that held only its indent)
            auto_indent_ = false;
            moved();
            break;
        }
        default: break;
    }
    if (k.kind == VimKey::kLeft || k.kind == VimKey::kRight || k.kind == VimKey::kHome || k.kind == VimKey::kEnd) auto_indent_ = false;
    return kHandled;
}

// ---------------------------------------------------------------- Normal and Visual modes

void Vim::leave_visual(Editor& ed) {
    if (visual()) {
        last_vstart_ = vstart_;
        last_vend_ = ed.cursor();
        last_vmode_ = mode_;
    }
    mode_ = kNormal;
    go(ed, ed.cursor());
}

u32 Vim::normal_key(Editor& ed, const VimKey& k) {
    message_.clear();
    const u32 want_before = want_col_;  // (the column wanted before this key's motion: "$" changes it)
    u32 c = k.ch;
    bool ctrl = k.ctrl;
    switch (k.kind) {
        case VimKey::kChar: break;
        case VimKey::kEsc: c = 27; break;
        case VimKey::kEnter: c = '\r'; break;
        case VimKey::kBackspace: c = kBack; break;
        case VimKey::kLeft: c = 'h'; break;
        case VimKey::kRight: c = 'l'; break;
        case VimKey::kUp: c = 'k'; break;
        case VimKey::kDown: c = 'j'; break;
        case VimKey::kHome: c = '0'; break;
        case VimKey::kEnd: c = '$'; break;
        case VimKey::kPageUp: c = 'b', ctrl = true; break;
        case VimKey::kPageDown: c = 'f', ctrl = true; break;
        case VimKey::kDelete: c = 'x'; break;
        case VimKey::kTab: c = 'i', ctrl = true; break;  // (they are the same key to Vim)
        default: return kHandled;
    }
    if (c == 27 || (ctrl && (c == '[' || c == 'c'))) {
        clear_pending();
        if (visual()) leave_visual(ed);
        return kHandled;
    }
    if (c >= 32 && !ctrl && c < 127) shown_ += static_cast<char>(c);
    else if (ctrl) shown_ += "^" + std::string(1, static_cast<char>(std::toupper(static_cast<int>(c))));

    const u64 cur = ed.cursor(), line = ed.buf.line_of(cur), n = lines_of(ed);
    auto done = [&] {
        clear_pending();
        return static_cast<u32>(kHandled);
    };
    auto fail = [&] {  // the command did not work: a macro running it stops here
        failed_ = true;
        return done();
    };
    auto lines_from = [&](u64 l0, u64 count, u64* lo, u64* hi) {  // `count` whole lines from l0
        const u64 l1 = std::min(l0 + count - 1, n - 1);
        *lo = ed.buf.line_start(l0);
        *hi = l1 + 1 < ed.buf.line_count() ? ed.buf.line_start(l1 + 1) : ed.buf.size();
    };
    auto total = [&] { return (op_count_ ? op_count_ : 1) * (counted_ ? count_ : 1); };
    auto insert_at = [&](u64 off, u32 times) {
        if (refuse()) return done();
        ed.move_to(off, false);
        clear_pending();
        enter_insert(ed, times);
        return static_cast<u32>(kHandled);
    };
    // After an operator: that was the command, or (after "c") on into Insert mode.
    auto settle = [&] {
        if (mode_ == kNormal) clear_pending();
        else shown_.clear(), op_ = 0, count_ = op_count_ = 0, counted_ = false, reg_ = 0;
        return static_cast<u32>(kHandled);
    };
    // Carries out a motion: under an operator, on the text it passes over.
    auto apply = [&](const Motion& m) {
        if (!m.ok) return fail();
        if (!op_) {
            if (m.jump && m.to != cur) remember_jump(ed, cur);
            go(ed, m.to, m.keep_column);
            vis_eol_ = mode_ == kVisual && m.keep_column && want_col_ == kEndCol;
            return done();
        }
        u64 lo = std::min(cur, m.to), hi = std::max(cur, m.to);
        bool linewise = m.linewise, adjusted = false;
        if (linewise) lines_from(ed.buf.line_of(lo), ed.buf.line_of(hi) - ed.buf.line_of(lo) + 1, &lo, &hi);
        else if (m.inclusive) hi = std::min(ed.next_char(hi), ed.buf.size());
        else if (exclusive_trim(ed, &lo, &hi)) {
            linewise = adjusted = true;
            lines_from(ed.buf.line_of(lo), ed.buf.line_of(hi) - ed.buf.line_of(lo) + 1, &lo, &hi);
        }
        if (!linewise && op_ == 'd' && deletes_lines(ed, lo, hi, n)) {
            linewise = true;
            lines_from(ed.buf.line_of(lo), ed.buf.line_of(hi) - ed.buf.line_of(lo) + 1, &lo, &hi);
            want_col_ = want_before;  // (the cursor goes to the column it wanted before, as after "dj")
        }
        const char op = op_;
        op_from_ = std::min(cur, m.to);
        operate(ed, op, lo, hi, linewise);
        if (adjusted && op == 'd') go(ed, first_nonblank(ed, ed.buf.line_of(ed.cursor())));  // (made whole lines by that rule, Vim's delete leaves the cursor at the first non-blank, not in the column)
        return settle();
    };
    // The same for an operator in Visual mode: on what is selected (`lines`: on the lines it is in, whole).
    auto on_selection = [&](char op, bool lines = false) {
        if (mode_ == kVisualBlock && !lines && op != kOpComment && op != kOpFormat) {
            block_do(ed, op, total());
            return settle();
        }
        u64 lo, hi;
        visual_range(ed, &lo, &hi);
        const u64 l0 = ed.buf.line_of(lo), l1 = ed.buf.line_of(hi > lo ? hi - 1 : lo);
        if (lines) lines_from(l0, l1 - l0 + 1, &lo, &hi);
        const bool linewise = lines || mode_ == kVisualLine;
        const u32 levels = total();
        const u64 from = std::min(cur, ed.buf.line_start(ed.buf.line_of(vstart_)));
        leave_visual(ed);
        ed.move_to(lo, false);
        if (op == '<' || op == '>') {  // with a count: by so many levels
            ed.begin_group();
            for (u32 i = 0; i < levels; ++i) operate_lines(ed, op, l0, l1, true);
            ed.end_group();
        } else {
            if (linewise) op_from_ = from;
            operate(ed, op, lo, hi, linewise);
        }
        return settle();
    };
    // A macro on each of the lines selected, from the start of the line
    // (Neovim's @ and Q in Visual Line mode). A command a macro leaves
    // unfinished is given up, as by Esc.
    auto play_on_lines = [&](char name) {
        u64 lo, hi;
        visual_range(ed, &lo, &hi);
        const u64 l0 = ed.buf.line_of(lo), l1 = ed.buf.line_of(hi > lo ? hi - 1 : lo);
        leave_visual(ed);
        clear_pending();
        failed_ = false;
        if (macros_.find(name) == macros_.end()) return static_cast<u32>(kHandled);
        ed.begin_group();
        for (u64 l = l0; l <= l1 && l < lines_of(ed); ++l) {
            go(ed, ed.buf.line_start(l));
            ++macro_depth_;  // (its keys are no part of a macro being recorded, nor of ".")
            const std::vector<VimKey> keys_of = macros_[name];
            for (const VimKey& key : keys_of) {
                feed(ed, key);
                if (failed_) break;
            }
            --macro_depth_;
            if (mode_ == kInsert || mode_ == kReplace) leave_insert(ed);
            else if (visual()) leave_visual(ed);
            mode_ = kNormal;
            clear_pending();
            if (failed_) break;  // (the lines after it are left alone, as the rest of a macro is)
        }
        ed.end_group();
        macro_last_ = name;
        return static_cast<u32>(kHandled);
    };
    // The command line, for a command or for a search. (A search can be an
    // operator's motion, and in Visual mode it moves the selection's end.)
    auto open_line = [&](char kind, Mode back) {
        cmd_op_ = kind == ':' ? 0 : op_;
        cmd_reg_ = reg_;
        clear_pending();
        mode_ = kCommand;
        cmd_return_ = back;
        cmd_kind_ = kind;
        cmd_.clear();
        cmd_selected_ = false;
        cmd_origin_ = cur;
        saved_pattern_ = pattern_;
        inc_rev_ = ~0ull;
        history_at_ = history_.size();
        return static_cast<u32>(kHandled);
    };
    auto play = [&](char name, u32 times) {  // a recorded macro, so many times
        clear_pending();
        failed_ = false;
        const auto it = macros_.find(name);
        if (it == macros_.end() || macro_depth_ > 20) return static_cast<u32>(kHandled);
        macro_last_ = name;
        const std::vector<VimKey> keys_of = it->second;
        ++macro_depth_;
        ed.begin_group();
        // A command in it that failed ends it, and so every further time (and an outer macro it is part of).
        for (u32 i = 0; i < times && !failed_; ++i) {
            for (const VimKey& key : keys_of) {
                feed(ed, key);
                if (failed_) break;
            }
        }
        ed.end_group();
        --macro_depth_;
        return static_cast<u32>(kHandled);
    };

    // Something a key before this one was waiting for.
    bool found_now = false;
    if (await_ != kNothing) {
        const Await a = await_;
        await_ = kNothing;
        switch (a) {
            case kFindChar:
                find_key_ = await_key_;
                find_char_ = c;
                c = ';';
                ctrl = false;
                found_now = true;
                break;
            case kReplaceChar: {
                if (c == kBack) return done();
                if (refuse()) return done();
                const std::string with = c == '\r' ? std::string(line_break(ed)) : utf8_of(c);
                if (mode_ == kVisualBlock) {
                    block_do(ed, 'r', c);
                    return done();
                }
                if (visual()) {  // every character selected (a letter and its marks, a flag: one), the line breaks kept
                    u64 lo, hi;
                    visual_range(ed, &lo, &hi);
                    const std::string text = ed.buf.text(lo, hi - lo);
                    std::string out;
                    for (size_t i = 0; i < text.size();) {
                        const size_t next = char_end_in(text, i);
                        if (text[i] == '\n' || (text[i] == '\r' && next == i + 2)) out.append(text, i, next - i);
                        else out += with;
                        i = next;
                    }
                    leave_visual(ed);
                    ed.replace(lo, hi, out);
                    go(ed, lo);
                } else {
                    const u32 times = total();
                    u64 end = cur;
                    const u64 limit = ed.line_end(line);
                    u32 have = 0;
                    for (; have < times && end < limit; ++have) end = ed.next_char(end);
                    if (have < times) return fail();  // not that many characters on the line
                    std::string out;
                    if (c == '\r') out = with;  // (one line break takes the place of all of them)
                    else
                        for (u32 i = 0; i < times; ++i) out += with;
                    ed.replace(cur, end, out);
                    go(ed, c == '\r' ? cur + out.size() : ed.prev_char(cur + out.size()));
                }
                changing_ = true;
                return done();
            }
            case kRegisterName:
                if (c >= 128) return done();  // (a register is named by an ASCII character)
                reg_ = static_cast<char>(c);
                return kHandled;
            case kMarkSet:
                if (c >= 128) return done();  // (a mark is named by an ASCII character)
                if (c == '\'' || c == '`') prev_jump_ = cur;
                else marks_[buffer_id * 256 + (c & 0xFF)] = cur;
                return done();
            case kMarkJump: {
                // A mark set with "m", or one of those Vim keeps: where the last
                // jump started (' and `), the last change (.), where Insert mode
                // was last left (^), the ends of the last selection (< and >).
                u64 at = kNone;
                if (c == '\'' || c == '`') at = prev_jump_;
                else if (c == '.') at = last_change_;
                else if (c == '^') at = last_insert_at_;
                else if (c == '<') at = std::min(last_vstart_, last_vend_);
                else if (c == '>') at = std::max(last_vstart_, last_vend_);
                else if (const auto it = marks_.find(buffer_id * 256 + (c & 0xFF)); c < 128 && it != marks_.end()) at = it->second;
                if (at == kNone || c >= 128) {
                    message_ = "E20: Mark not set";
                    return fail();
                }
                Motion m;
                at = std::min(at, ed.buf.size());
                m.linewise = await_key_ == '\'';
                m.to = m.linewise ? first_nonblank(ed, ed.buf.line_of(at)) : at;
                m.jump = true;
                return apply(m);
            }
            case kMacroStart:
                if (c < 128 && std::isalnum(static_cast<int>(c))) {
                    macro_into_ = lower(static_cast<char>(c));
                    macro_append_ = c >= 'A' && c <= 'Z';  // "qA": on the end of what "qa" recorded
                    macro_keys_.clear();
                }
                return done();
            case kMacroPlay: {
                if (c >= 128) return done();
                const char name = c == '@' ? macro_last_ : lower(static_cast<char>(c));
                return mode_ == kVisualLine ? play_on_lines(name) : play(name, total());
            }
            case kObject: {
                u64 lo, hi;
                bool linewise;
                const bool around = await_key_ == 'a';
                if (visual()) {
                    u64 slo, shi;
                    visual_range(ed, &slo, &shi);
                    const bool grown = vstart_ != cur;  // more is selected than the one character Visual mode starts with
                    bool ok;
                    if (grown && (c == 'w' || c == 'W') && cur > vstart_ && mode_ != kVisualBlock) {
                        // More words, as Vim's current_word() adds them: on
                        // from the cursor at the end of what is selected (and
                        // if they run out, as far as they went).
                        const bool big = c == 'W';
                        Peek pk(ed.buf);
                        Walk w{ed, pk, cur};
                        for (u32 i = 0, more = total(); i < more; ++i) {
                            if (w.incl() == -1) break;
                            if (around != (w.cls(big) == 0)) {
                                if (!fwd_word(w, big) && i + 1 < more) break;
                                w.left();
                            } else if (!end_word(w, big, true)) {
                                break;
                            }
                        }
                        if (mode_ == kVisualLine) mode_ = kVisual;
                        go(ed, w.empty() || !w.eol() ? w.at : ed.prev_char(w.at));
                        return done();
                    }
                    if (grown && (c == 'w' || c == 'W' || c == 'p' || c == 's')) {
                        // More of the same: the next one joins what is selected.
                        if (shi >= ed.buf.size()) return done();
                        ed.move_to(shi, false);
                        ok = text_object(ed, c, around, total(), &lo, &hi, &linewise) && hi > shi;
                        ed.move_to(cur, false);
                        lo = slo;
                    } else {
                        ok = text_object(ed, c, around, total(), &lo, &hi, &linewise);
                        // That is what is selected already: then the one around it.
                        for (u64 back = 1; ok && grown && lo >= slo && hi <= shi && back <= 2; ++back) {
                            ok = slo >= back;
                            if (!ok) break;
                            ed.move_to(slo - back, false);
                            ok = text_object(ed, c, around, total(), &lo, &hi, &linewise);
                            ed.move_to(cur, false);
                        }
                        ok = ok && !(grown && lo >= slo && hi <= shi);
                    }
                    if (!ok) return done();
                    mode_ = linewise ? kVisualLine : kVisual;
                    vstart_ = lo;
                    if (c == 'p' && hi > lo) go(ed, ed.buf.line_start(ed.buf.line_of(hi - 1)));  // (a paragraph: at the start of its last line, as Vim leaves it)
                    else go(ed, hi > lo ? ed.prev_char(hi) : lo);
                    return done();
                }
                if (!text_object(ed, c, around, total(), &lo, &hi, &linewise)) return fail();
                if (hi <= lo && !linewise) {  // (nothing: as in Vim, the cursor goes to it, "c" inserts there, and no register changes)
                    if (op_ == 'c') return insert_at(lo, 1);
                    go(ed, lo);
                    return done();
                }
                const bool adjusted = linewise && (c == 'w' || c == 'W' || c == 's');  // (whole lines only by exclusive_trim()'s rule)
                if (!linewise && op_ == 'd' && deletes_lines(ed, lo, hi, n)) {
                    linewise = true;
                    lines_from(ed.buf.line_of(lo), ed.buf.line_of(hi) - ed.buf.line_of(lo) + 1, &lo, &hi);
                }
                const char op = op_;
                operate(ed, op, lo, hi, linewise);
                if (adjusted && op == 'd') go(ed, first_nonblank(ed, ed.buf.line_of(ed.cursor())));  // (as for a motion)
                return settle();
            }
            case kPrefixG: c |= kG; break;
            case kPrefixBracket: c |= await_key_ == '[' ? kLB : kRB; break;
            case kPrefixZ: {
                if (await_key_ == 'Z') {
                    if (c == 'Z') requests_.push_back({VimRequest::kSave, {}, 0, false, true});
                    else if (c == 'Q') requests_.push_back({VimRequest::kClose, {}, 0, true, false});
                    return done();
                }
                const i64 here = static_cast<i64>(line), rows = static_cast<i64>(view_rows);
                if (c == 'z' || c == '.') requests_.push_back({VimRequest::kScroll, {}, std::max<i64>(0, here - rows / 2)});
                else if (c == 't' || c == '\r') requests_.push_back({VimRequest::kScroll, {}, here});
                else if (c == 'b' || c == '-') requests_.push_back({VimRequest::kScroll, {}, std::max<i64>(0, here - rows + 1)});
                else if (c == 'c' || c == 'C') requests_.push_back({VimRequest::kFold, {}, 0});
                else if (c == 'o' || c == 'O') requests_.push_back({VimRequest::kFold, {}, 1});
                else if (c == 'a' || c == 'A') requests_.push_back({VimRequest::kFold, {}, 2});
                else if (c == 'M') requests_.push_back({VimRequest::kFold, {}, 3});
                else if (c == 'R') requests_.push_back({VimRequest::kFold, {}, 4});
                return done();
            }
            case kPrefixWindow:
                switch (c) {
                    case 'h': requests_.push_back({VimRequest::kFocus, {}, 0}); break;
                    case 'l': requests_.push_back({VimRequest::kFocus, {}, 1}); break;
                    case 'k': requests_.push_back({VimRequest::kFocus, {}, 2}); break;
                    case 'j': requests_.push_back({VimRequest::kFocus, {}, 3}); break;
                    case 'w': requests_.push_back({VimRequest::kFocus, {}, 4}); break;
                    case 'v': requests_.push_back({VimRequest::kSplit, {}, 0}); break;
                    case 's': requests_.push_back({VimRequest::kSplit, {}, 1}); break;
                    case 'c':
                    case 'q': requests_.push_back({VimRequest::kClose, {}}); break;
                    default: break;
                }
                return done();
            default: break;
        }
    }

    // A count. (A lone 0 is the motion to the start of the line.)
    if (!ctrl && !found_now && c >= '0' && c <= '9' && (c != '0' || counted_)) {
        count_ = std::min<u32>(count_ * 10 + (c - '0'), 99999999);
        counted_ = true;
        return kHandled;
    }
    const u32 times = total();
    const bool counted = counted_ || op_count_;

    // Keys that wait for another.
    if (!ctrl) {
        switch (c) {
            case '"': await_ = kRegisterName; return kHandled;
            case 'f': case 'F': case 't': case 'T':
                await_ = kFindChar;
                await_key_ = static_cast<char>(c);
                return kHandled;
            case 'g': await_ = kPrefixG; return kHandled;
            case '[': case ']':
                await_ = kPrefixBracket;
                await_key_ = static_cast<char>(c);
                return kHandled;
            case 'z': case 'Z':
                if (op_) return done();
                await_ = kPrefixZ;
                await_key_ = static_cast<char>(c);
                return kHandled;
            case '\'': case '`':
                await_ = kMarkJump;
                await_key_ = static_cast<char>(c);
                return kHandled;
            case 'i': case 'a':
                if (op_ || visual()) {
                    await_ = kObject;
                    await_key_ = static_cast<char>(c);
                    return kHandled;
                }
                break;
            default: break;
        }
    } else if (c == 'w' && !op_) {
        await_ = kPrefixWindow;
        return kHandled;
    }

    // Operators. Twice the same is "these lines"; in Visual mode it is "the selection".
    if (!ctrl) {
        char op = 0;
        switch (c) {
            case 'd': case 'c': case 'y': case '<': case '>': op = static_cast<char>(c); break;
            case kG | '~': op = '~'; break;
            case kG | 'u': op = 'u'; break;
            case kG | 'U': op = 'U'; break;
            case kG | '?': op = '?'; break;
            case kG | 'q': op = kOpFormat; break;
            case kG | 'c':
                if (!op_) op = kOpComment;  // (after an operator "gc" is the comment the cursor is in: below)
                break;
            default: break;
        }
        if (op_ && (c == static_cast<u32>(op_) || (op && op == op_) || (op_ == kOpComment && c == 'c') || (op_ == kOpFormat && c == 'q'))) {  // dd, cc, yy, >>, g~~, guu, gUU, g??, gcc, and g~g~
            if (times > 1 && line + 1 >= n) return fail();  // (more lines than one asked for on the last: Vim does nothing)
            u64 lo, hi;
            lines_from(line, times, &lo, &hi);
            op_from_ = std::min(cur, first_nonblank(ed, std::min<u64>(line + times - 1, n - 1)));
            const char doubled = op_;
            operate(ed, doubled, lo, hi, true);
            return settle();
        }
        if (op && visual()) return on_selection(op);
        if (op && !op_) {
            op_ = op;
            op_count_ = counted_ ? count_ : 0;
            count_ = 0;
            counted_ = false;
            return kHandled;
        }
    }

    if (visual() && !ctrl && !found_now && (c == '*' || c == '#')) {
        // What is selected, wherever else it is (Neovim's Visual-mode * and #).
        u64 lo, hi;
        visual_range(ed, &lo, &hi);
        if (hi <= lo) return done();
        pattern_ = Pattern{ed.buf.text(lo, std::min<u64>(hi - lo, 4096)), false, false, nullptr, {}};
        search_forward_ = c == '*';
        leave_visual(ed);
        ed.move_to(lo, false);
        if (!search(ed, search_forward_, times)) return fail();
        if (ed.cursor() != cur) remember_jump(ed, cur);
        return done();
    }

    // Motions.
    Motion m;
    repeat_find_ = (c == ';' || c == ',') && !found_now;
    if (op_ == 'c' && (c == 'w' || c == 'W') && kind_of(Peek(ed.buf).at(cur), c == 'W') != 0) {
        // "cw" on a word changes to the end of the word, like "ce" (but on a
        // word's last character the first step stays where it is).
        Peek pk(ed.buf);
        const u64 next = ed.next_char(cur);
        const bool big = c == 'W', last = next >= ed.buf.size() || kind_of(pk.at(next), big) != kind_of(pk.at(cur), big);
        if (last && times == 1) {
            m.to = cur;
            m.inclusive = true;
            return apply(m);
        }
        if (!motion(ed, c == 'w' ? 'e' : 'E', false, last ? times - 1 : times, counted, &m)) return done();
        return apply(m);
    }
    if (motion(ed, c, ctrl, times, counted, &m)) {
        if (op_ && (c == 'w' || c == 'W') && m.ok && ed.buf.line_of(m.to) > line) {
            // "dw" with the last word it passes over at the end of a line stops
            // at the end of that line, not in the next one. (On an empty line
            // there is no word to stop after: the motion is the whole line.)
            Peek pk(ed.buf);
            u64 e = m.to;
            while (e > cur && kind_of(pk.at(e - 1), false) == 0) --e;
            bool crossed = false;
            for (u64 i = e; i < m.to && !crossed; ++i) crossed = pk.at(i) == '\n';
            if (crossed && !(e == cur && ed.line_end(line) == ed.buf.line_start(line))) m.to = ed.line_end(ed.buf.line_of(e));
        }
        return apply(m);
    }
    const bool vis = visual();
    if (c == (kG | 'n') || c == (kG | 'N')) {
        // The match of the last search that the cursor is in, or the next
        // one (gN: the one before): selected, or what the operator works on.
        if (pattern_.empty()) {
            message_ = "E35: No previous regular expression";
            return done();
        }
        if (pattern_.bad()) {
            message_ = pattern_.error;
            return fail();
        }
        RegexMatch mm;
        u64 hit;
        const u32 runs = overruns(pattern_);
        if (c == (kG | 'n')) {
            // The first match from the cursor's line on that ends after the
            // cursor (or is an empty one at it); else from the top.
            const u64 ls = ed.buf.line_start(line);
            hit = find(ed.buf, pattern_, ls == 0 ? kNone : ls - 1, true, false, nullptr, ~0ull, &mm);
            while (hit != kNone && mm.end <= cur && !(mm.end == mm.start && mm.start == cur)) hit = find(ed.buf, pattern_, hit, true, false, nullptr, ~0ull, &mm);
            if (hit == kNone) hit = find(ed.buf, pattern_, kNone, true, false, nullptr, ~0ull, &mm);
        } else {
            hit = find(ed.buf, pattern_, cur + 1, false, true, nullptr, ~0ull, &mm);
        }
        if (hit == kNone) {
            message_ = not_found(pattern_, runs);
            return fail();
        }
        highlight_on_ = true;
        // (A match of nothing is taken as the character it stands before.)
        const u64 end = mm.end > hit ? std::min(mm.end, ed.buf.size()) : std::min(ed.next_char(hit), ed.buf.size());
        if (op_) {
            const char op = op_;
            ed.move_to(hit, false);
            operate(ed, op, hit, end, false);
            return settle();
        }
        if (!vis) mode_ = kVisual, vstart_ = hit;
        go(ed, ed.prev_char(end));
        return done();
    }
    if (op_ && c == (kG | 'c')) {
        // The comment the cursor is in, as whole lines: "dgc", and "gcgc" to take the markers off it.
        if (comment_left.empty() || !commented(ed, line)) return done();
        u64 l0 = line, l1 = line;
        while (l0 > 0 && commented(ed, l0 - 1)) --l0;
        while (l1 + 1 < n && commented(ed, l1 + 1)) ++l1;
        u64 lo, hi;
        lines_from(l0, l1 - l0 + 1, &lo, &hi);
        const char op = op_;
        operate(ed, op, lo, hi, true);
        return settle();
    }
    if (op_ && !ctrl && (c == '/' || c == '?')) return open_line(static_cast<char>(c), kNormal);  // "d/word": up to where it is found
    if (op_) return done();  // an operator and then something that is no motion: nothing

    // Everything else.
    if (ctrl) {
        const i64 rows = static_cast<i64>(view_rows), top = static_cast<i64>(view_top), here = static_cast<i64>(line), last = static_cast<i64>(n) - 1;
        auto page = [&](i64 by) {  // the view and the cursor, both by so many lines
            requests_.push_back({VimRequest::kScroll, {}, std::clamp<i64>(top + by, 0, last)});
            Motion mv;
            mv.to = std::min(ed.off_at(static_cast<u64>(std::clamp<i64>(here + by, 0, last)), want_col_), line_last(ed, static_cast<u64>(std::clamp<i64>(here + by, 0, last))));
            go(ed, mv.to, true);
        };
        switch (c) {
            case 'r':
                if (refuse()) return done();
                // The cursor goes to where the text changed; or, as Vim has
                // it, where it was before the change, if that was on the line
                // the change starts on or the one above it (after "o").
                for (u32 i = 0; i < times && ed.redo(); ++i) {
                    const u64 at = std::min(ed.changed_at(), ed.buf.size()), was = std::min(ed.step_cursor(), ed.buf.size());
                    const u64 l = ed.buf.line_of(at), wl = ed.buf.line_of(was);
                    go(ed, wl == l || wl + 1 == l ? was : at);
                }
                return done();
            case 'd': page(counted ? times : rows / 2); return done();
            case 'u': page(-(counted ? static_cast<i64>(times) : rows / 2)); return done();
            case 'f': page(std::max<i64>(1, rows - 2) * times); return done();
            case 'b': page(-std::max<i64>(1, rows - 2) * static_cast<i64>(times)); return done();
            case 'e':
            case 'y': {
                const i64 to = std::clamp<i64>(top + (c == 'e' ? 1 : -1) * static_cast<i64>(times), 0, last);
                requests_.push_back({VimRequest::kScroll, {}, to});
                // The cursor stays on its line unless that leaves the view.
                const i64 keep = std::clamp<i64>(here, to, to + rows - 1);
                if (keep != here) go(ed, std::min(ed.off_at(static_cast<u64>(std::min(keep, last)), want_col_), line_last(ed, static_cast<u64>(std::min(keep, last)))), true);
                return done();
            }
            case 'o':
            case 'i': {
                // Back and forward through the places long moves started from.
                // (With nowhere to go back to in this document, the application
                // is asked: it knows where a "go to definition" came from.)
                if (on_jump) { requests_.push_back({VimRequest::kBack, {}, c == 'o' ? -static_cast<i64>(times) : static_cast<i64>(times)}); return done(); }
                bool went = false;
                for (u32 i = 0; i < times; ++i) {
                    if (c == 'o') {
                        if (jumps_.empty()) break;
                        if (jump_at_ >= jumps_.size()) {  // setting out: here is where <C-i> comes back to
                            list_jump(ed, ed.cursor());
                            jump_at_ = jumps_.size() - 1;
                        }
                        if (jump_at_ == 0) break;
                        --jump_at_;
                    } else {
                        if (jump_at_ + 1 >= jumps_.size()) break;
                        ++jump_at_;
                    }
                    prev_jump_ = ed.cursor();
                    go(ed, jumps_[jump_at_]);
                    went = true;
                }
                if (!went && c == 'o') requests_.push_back({VimRequest::kBack, {}});
                return done();
            }
            case 'v':
                if (mode_ == kVisualBlock) {
                    leave_visual(ed);
                } else {
                    if (!vis) vstart_ = cur;
                    mode_ = kVisualBlock;
                }
                return done();
            case 'l':
                highlight_on_ = false;
                requests_.push_back({VimRequest::kNoHighlight, {}});
                return done();
            case ']':
                remember_jump(ed, cur);
                requests_.push_back({VimRequest::kDefinition, {}});
                return done();
            case 't': requests_.push_back({VimRequest::kBack, {}}); return done();  // back from where <C-]> went
            case 'a':
            case 'x': {
                // The number under or after the cursor, up or down by the count:
                // decimal (with a "-" before it), or after "0x" hexadecimal and
                // after "0b" binary ('nrformats' is "bin,hex"). Zeros in front stay.
                if (refuse()) return done();
                const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
                const std::string text = ed.buf.text(start, end - start);
                const size_t at = static_cast<size_t>(cur - start);
                auto digit_of = [](char ch, int base) {
                    const int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 99;
                    return v < base ? v : -1;
                };
                size_t a = 0, b = 0, digits = 0;  // the number is text[a, b), its digits from `digits`
                int base = 0;
                for (size_t i = 0; i < text.size() && !base;) {
                    if (!std::isdigit(static_cast<u8>(text[i]))) {
                        ++i;
                        continue;
                    }
                    size_t from = i, to = i;
                    int found = 10;
                    if (text[i] == '0' && i + 2 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && digit_of(text[i + 2], 16) >= 0) found = 16, from = i + 2;
                    else if (text[i] == '0' && i + 2 < text.size() && (text[i + 1] == 'b' || text[i + 1] == 'B') && digit_of(text[i + 2], 2) >= 0) found = 2, from = i + 2;
                    for (to = from; to < text.size() && digit_of(text[to], found) >= 0; ++to) {}
                    if (to > at) a = i, b = to, digits = from, base = found;  // the first one that ends after the cursor
                    i = to;
                }
                if (!base) return done();
                const long long by = (c == 'a' ? 1 : -1) * static_cast<long long>(times);
                const std::string old = text.substr(digits, b - digits);
                std::string out;
                if (base == 10) {
                    const bool minus = a > 0 && text[a - 1] == '-';
                    if (minus) --a;
                    const long long value = (minus ? -1 : 1) * std::strtoll(old.c_str(), nullptr, 10) + by;
                    std::string number = std::to_string(value < 0 ? -(value + 1) + 1ull : static_cast<unsigned long long>(value));
                    if (old.size() > 1 && old[0] == '0' && number.size() < old.size()) number.insert(0, old.size() - number.size(), '0');
                    out = (value < 0 ? "-" : "") + number;
                } else {
                    unsigned long long value = std::strtoull(old.c_str(), nullptr, base) + static_cast<unsigned long long>(by);
                    bool upper = false;
                    for (const char ch : old) upper = upper || (ch >= 'A' && ch <= 'F');
                    std::string number;
                    do {
                        const unsigned d = static_cast<unsigned>(value % static_cast<unsigned>(base));
                        number.insert(number.begin(), static_cast<char>(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10));
                        value /= static_cast<unsigned>(base);
                    } while (value);
                    if (number.size() < old.size()) number.insert(0, old.size() - number.size(), '0');
                    out = text.substr(a, digits - a) + number;
                }
                ed.replace(start + a, start + b, out);
                changing_ = true;
                go(ed, start + a + out.size() - 1);
                return done();
            }
            default: return done();
        }
    }
    if (vis) {
        u64 lo, hi;
        visual_range(ed, &lo, &hi);
        const bool blockwise = mode_ == kVisualBlock;
        switch (c) {
            case 'v':
            case 'V': {
                const Mode want = c == 'v' ? kVisual : kVisualLine;
                if (mode_ == want) leave_visual(ed);
                else mode_ = want;
                return done();
            }
            case 'o':
            case 'O': {
                // The cursor to the other end of what is selected. (O in a
                // block: to the other side, on the same line.)
                if (c == 'O' && blockwise) {
                    const u64 la = ed.buf.line_of(std::min(vstart_, ed.buf.size()));
                    const u32 ca = ed.col_between(ed.buf.line_start(la), std::min(vstart_, ed.buf.size())), cb = ed.col_between(ed.buf.line_start(line), cur);
                    vstart_ = std::min(ed.off_at(la, cb), line_last(ed, la));
                    go(ed, std::min(ed.off_at(line, ca), line_last(ed, line)));
                    return done();
                }
                const u64 other = vstart_;
                vstart_ = cur;
                go(ed, other);
                return done();
            }
            // The capitals are for whole lines; in a block, D and C are to the end of each line.
            case 'x': return on_selection('d');
            case 'X': return on_selection('d', !blockwise);
            case 's': return on_selection('c');
            case 'S': case 'R': return on_selection('c', true);
            case 'Y': return on_selection('y', !blockwise);
            case 'D':
            case 'C':
                if (!blockwise) return on_selection(c == 'D' ? 'd' : 'c', true);
                block_do(ed, static_cast<char>(c));
                return settle();
            case 'I':
            case 'A': {
                if (blockwise) {
                    block_do(ed, static_cast<char>(c));
                    return settle();
                }
                // At the start of the first line selected, or the end of the last.
                const u64 l0 = ed.buf.line_of(lo), l1 = ed.buf.line_of(hi > lo ? hi - 1 : lo);
                leave_visual(ed);
                return insert_at(c == 'I' ? ed.buf.line_start(l0) : ed.line_end(l1), times);
            }
            case '~': return on_selection('~');
            case 'u': return on_selection('u');
            case 'U': return on_selection('U');
            case 'r': await_ = kReplaceChar; return kHandled;
            case 'Q':
                if (mode_ == kVisualLine) return play_on_lines(macro_rec_);
                break;
            case 'J':
            case kG | 'J': {
                const u64 l0 = ed.buf.line_of(lo), l1 = hi > lo ? ed.buf.line_of(hi - 1) : l0;
                leave_visual(ed);
                ed.move_to(ed.buf.line_start(l0), false);
                count_ = static_cast<u32>(std::max<u64>(2, l1 - l0 + 1));
                counted_ = true;
                if (c != 'J') await_ = kPrefixG;
                VimKey join;
                join.ch = 'J';
                return normal_key(ed, join);
            }
            case 'p':
            case 'P': {
                // What is selected goes; what the register holds takes its place.
                if (blockwise) {
                    block_do(ed, 'p');
                    return done();
                }
                if (refuse()) return done();
                const auto held = fetch(reg_);
                const Register& r = *held;
                const bool linewise = mode_ == kVisualLine;
                leave_visual(ed);
                ed.begin_group();
                const std::string old = ed.buf.text(lo, hi - lo);
                ed.replace(lo, hi, linewise && !r.linewise ? r.text + "\n" : !linewise && r.linewise ? "\n" + r.text : r.text);
                ed.end_group();
                if (c == 'p') regs_['"'] = std::make_shared<const Register>(Register{std::move(old), linewise});  // (P keeps the register: it can be put again)
                changing_ = true;
                go(ed, lo);
                return done();
            }
            case ':':
                leave_visual(ed);
                open_line(':', kNormal);
                cmd_ = "'<,'>";
                return kHandled;
            default: break;
        }
    }
    switch (c) {
        case 'i': return insert_at(cur, times);
        case 'a': return insert_at(ed.line_end(line) > cur ? ed.next_char(cur) : cur, times);
        case 'I': return insert_at(first_nonblank(ed, line), times);
        case 'A': return insert_at(ed.line_end(line), times);
        case 'o':
        case 'O': {
            // A new line below (or above), as far in as this one, and Insert mode on it.
            if (refuse()) return done();
            std::string indent = indent_of(ed, line);
            if (indent_rules && c == 'o') indent = indent_after(*indent_rules, ed, line, ed.line_end(line), std::move(indent), shift, comment_left);  // (as Enter at its end would)
            const std::string nl = line_break(ed);
            clear_pending();
            ed.begin_group();
            if (c == 'o') {
                const u64 end = ed.line_end(line);
                ed.replace(end, end, nl + indent);
                ed.move_to(end + nl.size() + indent.size(), false);
            } else {
                const u64 start = ed.buf.line_start(line);
                ed.replace(start, start, indent + nl);
                ed.move_to(start + indent.size(), false);
            }
            enter_insert(ed, times);
            insert_prefix_ = nl + indent;  // "3o": two more lines of what is typed
            auto_indent_ = true;
            ed.end_group();
            return kHandled;
        }
        case kG | 'i': return insert_at(last_insert_at_ == kNone ? cur : std::min(last_insert_at_, ed.buf.size()), times);  // where Insert mode was last left (if it has been)
        case 'x': case 'X': case 's': case 'D': case 'C': case 'Y': case 'S': {
            // Short for an operator and a motion.
            static const struct {
                char key, op;
                const char* move;
            } shorts[] = {{'x', 'd', "l"}, {'X', 'd', "h"}, {'s', 'c', "l"}, {'D', 'd', "$"}, {'C', 'c', "$"}, {'Y', 'y', "$"}, {'S', 'c', nullptr}};
            for (const auto& s : shorts) {
                if (static_cast<u32>(s.key) != c) continue;
                if (ed.line_end(line) == ed.buf.line_start(line) && (c == 'x' || c == 'X')) return done();  // nothing to delete on an empty line
                op_ = s.op;
                if (!s.move) {  // S: the whole line
                    if (times > 1 && line + 1 >= n) return fail();
                    u64 lo, hi;
                    lines_from(line, times, &lo, &hi);
                    operate(ed, 'c', lo, hi, true);
                    return settle();
                }
                Motion mv;
                if (!motion(ed, static_cast<u32>(*s.move), false, times, counted, &mv)) return done();
                if (c == 's' && !mv.ok) {  // "s" on an empty line is "i"
                    mv.ok = true;
                    mv.to = cur;
                }
                return apply(mv);
            }
            return done();
        }
        case 'r': await_ = kReplaceChar; return kHandled;
        case 'R':
            if (refuse()) return done();
            clear_pending();
            enter_insert(ed, 1, kReplace);
            return kHandled;
        case 'J':
        case kG | 'J': {
            // The next line onto the end of this one, with one space between.
            if (refuse()) return done();
            const u32 joins = std::max<u32>(1, times > 1 ? times - 1 : 1);
            ed.begin_group();
            u64 at = cur;
            const u64 last = std::min<u64>(line + joins, lines_of(ed) - 1);
            const bool many = last - line > kManyLines;
            if (many) {
                // Many lines: they are joined a megabyte at a time, an edit for each.
                u64 e = ed.line_end(line), end = ed.line_end(last);  // where the joined line ends, and where the last one does
                char before = ' ';  // the last character of the joined line so far
                if (e > ed.buf.line_start(line)) ed.buf.read(e - 1, &before, 1);
                std::string was, out;
                while (e < end) {
                    u64 b = std::min(end, e + kEditMost);
                    was = ed.buf.text(e, b - e);  // a line break, a line, a line break, a line...
                    const size_t first = was[0] == '\r' ? 1 : 0;  // (the first line break's newline)
                    if (b < end) {
                        size_t cut = was.rfind('\n');
                        if (cut == first) {  // one line longer than all that: the whole of it
                            const u64 next = next_line_start(ed.buf, b);
                            b = std::min(end, next < ed.buf.size() || (next > 0 && Peek(ed.buf).at(next - 1) == '\n') ? next - 1 : next);
                            if (b > e && b < end && Peek(ed.buf).at(b - 1) == '\r') --b;
                            was = ed.buf.text(e, b - e);
                        } else {
                            if (cut > 0 && was[cut - 1] == '\r') --cut;
                            was.resize(cut);
                            b = e + cut;
                        }
                    }
                    out.clear();
                    for (size_t from = 0; from < was.size();) {
                        if (was[from] == '\r') ++from;
                        ++from;  // (the newline)
                        const size_t nl = was.find('\n', from);
                        size_t stop = nl == std::string::npos ? was.size() : nl;
                        if (nl != std::string::npos && stop > from && was[stop - 1] == '\r') --stop;
                        std::string_view next(was.data() + from, stop - from);
                        at = e + out.size();
                        if (c == 'J') {
                            size_t lead = 0;
                            while (lead < next.size() && (next[lead] == ' ' || next[lead] == '\t')) ++lead;
                            next.remove_prefix(keep_marked(next, lead));
                            if (before != ' ' && before != '\t' && !next.empty() && next[0] != ')') out += ' ';
                            if (!next.empty()) before = next.back();
                        }
                        out += next;
                        from = stop;
                    }
                    ed.replace(e, b, out);
                    end = end + out.size() - (b - e);
                    e += out.size();
                }
                ed.move_to(at, false);
            }
            for (u32 i = 0; !many && i < joins && ed.buf.line_of(ed.cursor()) + 1 < lines_of(ed); ++i) {
                const u64 l = ed.buf.line_of(ed.cursor()), end = ed.line_end(l);
                const u64 next = c == 'J' ? first_nonblank(ed, l + 1) : ed.buf.line_start(l + 1);
                char before = ' ', after = 0;
                if (end > ed.buf.line_start(l)) ed.buf.read(end - 1, &before, 1);
                ed.buf.read(next, &after, 1);
                const bool space = c == 'J' && before != ' ' && before != '\t' && after != ')' && next < ed.line_end(l + 1);
                ed.replace(end, next, space ? " " : "");
                at = end;
                ed.move_to(at, false);
            }
            ed.end_group();
            changing_ = true;
            go(ed, at);
            return done();
        }
        case '~': {
            if (refuse()) return done();
            u64 end = cur;
            const u64 limit = ed.line_end(line);
            for (u32 i = 0; i < times && end < limit; ++i) end = ed.next_char(end);
            if (end > cur) {
                const u64 size0 = ed.buf.size();
                operate(ed, '~', cur, end, false);
                end = end + ed.buf.size() - size0;  // (a letter whose other case is longer or shorter: "ſ" is two bytes, "S" one)
            }
            go(ed, std::min(end, line_last(ed, line)));
            return done();
        }
        case 'p': put(ed, false, times); return done();
        case 'P': put(ed, true, times); return done();
        case kG | 'p': put(ed, false, times, true); return done();
        case kG | 'P': put(ed, true, times, true); return done();
        case '&':
        case kG | '&':
            // The last :s again: on this line, or (g&) on every line with its flags.
            if (last_sub_.empty()) {
                message_ = "E35: No previous regular expression";
                return done();
            }
            // (Neovim's & is ":&&": the flags are kept.)
            if (c == '&') substitute(ed, line, line, last_sub_, true);
            else substitute(ed, 0, n - 1, last_sub_, true);
            changing_ = true;
            return done();
        case kG | ';': {
            if (last_change_ == kNone) {
                message_ = "E664: changelist is empty";
                return done();
            }
            Motion mv;
            mv.to = std::min(last_change_, ed.buf.size());
            mv.jump = true;
            return apply(mv);
        }
        case kG | 'f': {
            // The file whose name is under the cursor.
            const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
            const std::string text = ed.buf.text(start, end - start);
            auto in_name = [&](size_t i) { return i < text.size() && !std::strchr(" \t\"'<>()[]{},;|=`", text[i]) && text[i]; };
            size_t a = static_cast<size_t>(cur - start), b = a;
            if (!in_name(a)) return done();
            while (a > 0 && in_name(a - 1)) --a;
            while (in_name(b)) ++b;
            requests_.push_back({VimRequest::kOpen, text.substr(a, b - a), 1});
            return done();
        }
        case 'u':
            if (refuse()) return done();
            for (u32 i = 0; i < times && ed.undo(); ++i) go(ed, ed.cursor());  // (back where it was before the change)
            return done();
        case '.': {
            if (dot_.empty() || replaying_) return done();
            clear_pending();
            std::vector<VimKey> again = dot_;
            u32 repeats = times;
            if (counted) {
                // A count here takes the place of the count the command had (before or after its operator), and is kept for the next ".".
                auto digit = [&](size_t i, char lo) { return i < again.size() && again[i].kind == VimKey::kChar && !again[i].ctrl && again[i].ch >= static_cast<u32>(lo) && again[i].ch <= '9'; };
                auto skip_count = [&](size_t i) {
                    if (!digit(i, '1')) return i;
                    while (digit(i, '0')) ++i;
                    return i;
                };
                size_t at = 0;
                std::vector<VimKey> out;
                if (again.size() > 1 && again[0].kind == VimKey::kChar && again[0].ch == '"') out.assign(again.begin(), again.begin() + 2), at = 2;
                for (const char d : std::to_string(times)) out.push_back(VimKey{VimKey::kChar, static_cast<u32>(d), false});
                at = skip_count(at);
                if (at < again.size() && again[at].kind == VimKey::kChar && !again[at].ctrl && again[at].ch < 128 && again[at].ch && std::strchr("dcy<>", static_cast<int>(again[at].ch))) out.push_back(again[at++]), at = skip_count(at);
                out.insert(out.end(), again.begin() + static_cast<std::ptrdiff_t>(at), again.end());
                again = out;
                dot_ = again;
                repeats = 1;
            }
            replaying_ = true;
            ed.begin_group();
            for (u32 i = 0; i < repeats; ++i)
                for (const VimKey& key : again) feed(ed, key);
            ed.end_group();
            replaying_ = false;
            return kHandled;
        }
        case 'v':
        case 'V':
            vstart_ = cur;
            vis_eol_ = false;
            mode_ = c == 'v' ? kVisual : kVisualLine;
            return done();
        case kG | 'v':
            if (last_vstart_ == kNone) return fail();  // (nothing has been selected yet)
            mode_ = last_vmode_;
            vstart_ = std::min(last_vstart_, ed.buf.size());
            go(ed, last_vend_);
            return done();
        case 'q':
            if (macro_into_) {  // stop recording (this "q" is no part of it)
                if (!macro_keys_.empty()) macro_keys_.pop_back();
                if (macro_append_) macros_[macro_into_].insert(macros_[macro_into_].end(), macro_keys_.begin(), macro_keys_.end());
                else macros_[macro_into_] = macro_keys_;
                macro_last_ = macro_rec_ = macro_into_;
                macro_into_ = 0;
                return done();
            }
            await_ = kMacroStart;
            return kHandled;
        case '@': await_ = kMacroPlay; return kHandled;
        case 'Q': return play(macro_rec_, times);  // the macro recorded last
        case 'm': await_ = kMarkSet; return kHandled;
        case ':':
        case '/':
        case '?': return open_line(static_cast<char>(c), vis && c != ':' ? mode_ : kNormal);
        // Neovim's bracket keys: [b ]b to the tab before and after ([B ]B the
        // first and last), [d ]d to the diagnostic before and after ([D ]D),
        // [<Space> ]<Space> an empty line above or below.
        case kLB | 'b': requests_.push_back({VimRequest::kTab, {}, -static_cast<i64>(times)}); return done();
        case kRB | 'b': requests_.push_back({VimRequest::kTab, {}, static_cast<i64>(times)}); return done();
        case kLB | 'B': requests_.push_back({VimRequest::kTab, "first"}); return done();
        case kRB | 'B': requests_.push_back({VimRequest::kTab, "last"}); return done();
        case kLB | 'd': requests_.push_back({VimRequest::kDiagnostic, {}, -static_cast<i64>(times)}); return done();
        case kRB | 'd': requests_.push_back({VimRequest::kDiagnostic, {}, static_cast<i64>(times)}); return done();
        case kLB | 'D': requests_.push_back({VimRequest::kDiagnostic, "first"}); return done();
        case kRB | 'D': requests_.push_back({VimRequest::kDiagnostic, "last"}); return done();
        case kLB | ' ':
        case kRB | ' ': {
            if (refuse()) return done();
            const bool above = c == (kLB | ' ');
            const u64 at = above ? ed.buf.line_start(line) : ed.line_end(line);
            std::string breaks;
            for (u32 i = 0; i < times; ++i) breaks += line_break(ed);
            ed.replace(at, at, breaks);
            changing_ = true;
            go(ed, above ? cur + breaks.size() : cur, true);
            return done();
        }
        case kG | 't': requests_.push_back({VimRequest::kTab, {}, 1}); return done();
        case kG | 'T': requests_.push_back({VimRequest::kTab, {}, -1}); return done();
        case kG | 'r': requests_.push_back({VimRequest::kReferences, {}}); return done();
        case kG | 'd':
            remember_jump(ed, cur);
            requests_.push_back({VimRequest::kDefinition, {}});
            return done();
        case kG | 'D':  // (Vim's own gD: the declaration, here the server's)
            remember_jump(ed, cur);
            requests_.push_back({VimRequest::kPlaces, {}, 2});
            return done();
        case kG | 'y':
            remember_jump(ed, cur);
            requests_.push_back({VimRequest::kPlaces, {}, 1});
            return done();
        case kG | 'O': requests_.push_back({VimRequest::kOutline, {}}); return done();
        case 'K': requests_.push_back({VimRequest::kHover, {}}); return done();
        default:
            for (const VimCommandKey& v : kVimCommands)
                if (c == (kG | static_cast<u32>(v.key))) requests_.push_back({VimRequest::kCommand, std::string(v.title)});
            return done();
    }
}

// ---------------------------------------------------------------- the command line

void Vim::open_command(Editor& ed, char kind, std::string_view text, bool selected) {
    if (confirm_.on) confirm_end(ed);
    if (mode_ == kInsert || mode_ == kReplace || insert_once_) leave_insert(ed);
    if (visual()) leave_visual(ed);
    if (mode_ == kCommand) mode_ = cmd_return_;
    clear_pending();
    cmd_op_ = cmd_reg_ = 0;
    mode_ = kCommand;
    cmd_return_ = kNormal;
    cmd_kind_ = kind;
    cmd_ = std::string(text);
    cmd_selected_ = selected && !cmd_.empty();
    cmd_origin_ = ed.cursor();
    saved_pattern_ = pattern_;
    inc_rev_ = ~0ull;
    history_at_ = history_.size();
    message_.clear();
    if (kind != ':' && !cmd_.empty()) incsearch(ed);
}

// Shows where the search being typed would land (incsearch).
void Vim::incsearch(Editor& ed) {
    pattern_ = parse_pattern(cmd_, last_with_);
    highlight_on_ = true;
    u64 hit = kNone;
    if (!pattern_.empty() && !pattern_.bad()) {  // (half a pattern, "\(" on its way to "\(a\)", finds nothing yet)
        const bool forward = cmd_kind_ == '/';
        const Pattern& was = inc_pattern_;
        // Text typed onto the end: a match of this is a match of what was
        // there before, so none comes before the one found then.
        const bool longer = inc_rev_ == ed.revision() && !pattern_.re && !was.re && !was.bad() && !pattern_.word && !was.word &&
                            pattern_.ignore_case == was.ignore_case && pattern_.text.size() > was.text.size() && pattern_.text.starts_with(was.text);
        const u64 o = cmd_origin_, h = inc_hit_, size = ed.buf.size();
        if (!longer) hit = find(ed.buf, pattern_, o, forward, true);
        else if (h != kNone && forward) hit = find(ed.buf, pattern_, h - 1, true, true, nullptr, h > o ? size - h + o + 1 : o + 1 - h);
        else if (h != kNone) hit = find(ed.buf, pattern_, h + 1, false, true, nullptr, h < o ? h + 1 + size - o : h - o + 1);
        inc_pattern_ = pattern_;
        inc_hit_ = hit;
        inc_rev_ = ed.revision();
    }
    const Mode keep = mode_;
    mode_ = kNormal;  // (so that the cursor is put on a character)
    go(ed, hit == kNone ? cmd_origin_ : hit);
    mode_ = keep;
    jumped_ = true;  // (to the match, or back: shown as a jump's place is)
}

u32 Vim::command_key(Editor& ed, const VimKey& k) {
    const bool searching = cmd_kind_ != ':';
    const char op = cmd_op_, op_reg = cmd_reg_;
    auto close = [&] {
        mode_ = cmd_return_;  // (Visual mode, if a search was begun there)
        cmd_return_ = kNormal;
        cmd_op_ = 0;
        cmd_selected_ = false;
        clear_pending();
    };
    if (k.kind == VimKey::kEsc || (k.ctrl && (k.ch == '[' || k.ch == 'c'))) {
        close();
        if (searching) {
            pattern_ = saved_pattern_;
            go(ed, cmd_origin_);
            jumped_ = true;
        }
        return kHandled;
    }
    if (k.kind == VimKey::kEnter || (k.ctrl && (k.ch == 'm' || k.ch == 'j'))) {
        const std::string line = cmd_;
        close();
        if (!line.empty() && (history_.empty() || history_.back() != std::string(1, cmd_kind_) + line)) history_.push_back(std::string(1, cmd_kind_) + line);
        if (searching) {
            if (!line.empty()) pattern_ = parse_pattern(line, last_with_);
            else pattern_ = saved_pattern_;
            search_forward_ = cmd_kind_ == '/';
            ed.move_to(cmd_origin_, false);
            const bool found = search(ed, search_forward_, 1);
            if (!found) failed_ = true;
            const u64 to = ed.cursor();
            if (found && op) {  // "d/word": from where the cursor was to where the word is
                ed.move_to(cmd_origin_, false);
                reg_ = op_reg;
                u64 lo = std::min(cmd_origin_, to), hi = std::max(cmd_origin_, to);
                const bool linewise = exclusive_trim(ed, &lo, &hi);
                if (linewise) {  // (the lines from lo's to hi's, whole)
                    const u64 last = ed.buf.line_of(hi);
                    lo = ed.buf.line_start(ed.buf.line_of(lo));
                    hi = last + 1 < ed.buf.line_count() ? ed.buf.line_start(last + 1) : ed.buf.size();
                }
                operate(ed, op, lo, hi, linewise);
                if (mode_ == kNormal) clear_pending();
                else reg_ = 0;
            } else if (found && to != cmd_origin_) {
                remember_jump(ed, cmd_origin_);
            }
        } else {
            run_command(ed, line);
        }
        return kHandled;
    }
    // What is on the line selected (Cmd+F put it there): what is typed
    // takes its place, Backspace clears it, an arrow keeps it.
    if (std::exchange(cmd_selected_, false)) {
        const bool cut = k.kind == VimKey::kBackspace || (k.ctrl && (k.ch == 'h' || k.ch == 'u' || k.ch == 'w'));
        if (cut || (k.kind == VimKey::kChar && !k.ctrl) || k.kind == VimKey::kTab || (k.ctrl && k.ch == 'r')) cmd_.clear();
        if (cut) {
            if (searching) incsearch(ed);
            return kHandled;
        }
        if (k.kind == VimKey::kLeft || k.kind == VimKey::kRight || k.kind == VimKey::kHome || k.kind == VimKey::kEnd) return kHandled;
    }
    bool edited = false;
    if (k.kind == VimKey::kBackspace || (k.ctrl && k.ch == 'h')) {
        if (cmd_.empty()) {
            close();
            if (searching) pattern_ = saved_pattern_, go(ed, cmd_origin_);
            return kHandled;
        }
        cmd_.resize(last_char_start(cmd_));  // (a letter with its marks, an emoji with what is joined to it: one)
        edited = true;
    } else if (k.ctrl && k.ch == 'u') {
        cmd_.clear();
        edited = true;
    } else if (k.ctrl && k.ch == 'w') {
        while (!cmd_.empty() && cmd_.back() == ' ') cmd_.pop_back();
        while (!cmd_.empty() && cmd_.back() != ' ') cmd_.pop_back();
        edited = true;
    } else if (k.ctrl && k.ch == 'r') {
        // (<C-r> and a register would need another key: the unnamed one is the useful case.)
        const std::string& t = fetch(0)->text;
        cmd_ += t.substr(0, t.find('\n'));
        edited = true;
    } else if (k.kind == VimKey::kUp || k.kind == VimKey::kDown) {
        // Earlier and later lines of the same kind.
        for (size_t i = history_at_;;) {
            if (k.kind == VimKey::kUp ? i == 0 : i + 1 >= history_.size()) break;
            k.kind == VimKey::kUp ? --i : ++i;
            if (history_[i][0] == cmd_kind_) {
                history_at_ = i;
                cmd_ = history_[i].substr(1);
                edited = true;
                break;
            }
        }
    } else if (k.kind == VimKey::kChar && !k.ctrl) {
        cmd_ += utf8_of(k.ch);
        edited = true;
    } else if (k.kind == VimKey::kTab) {
        cmd_ += '\t';
        edited = true;
    }
    if (edited && searching) incsearch(ed);
    return kHandled;
}

// :s/this/that/flags on lines line0..line1. The flags: g every match on a
// line (else the first), c ask about each, i and I ignore case or not, n
// only count them, e say nothing when there is none, & the last :s's flags
// first.
void Vim::substitute(Editor& ed, u64 line0, u64 line1, std::string_view rest, bool flags) {
    if (rest.empty()) return;
    const std::string whole(rest);  // (`rest` may be last_sub_ itself)
    rest = whole;
    const char delim = rest[0];
    std::string parts[3];
    int part = 0;
    for (size_t i = 1; i < rest.size(); ++i) {
        if (rest[i] == '\\' && i + 1 < rest.size() && rest[i + 1] == delim) {
            parts[part] += delim;
            ++i;
        } else if (rest[i] == '\\' && i + 1 < rest.size() && part < 2) {  // (an escape is the pattern's or the replacement's to read)
            parts[part] += rest[i];
            parts[part] += rest[++i];
        } else if (rest[i] == delim && part < 2) {
            ++part;
        } else {
            parts[part] += rest[i];
        }
    }
    if (const size_t bar = parts[2].find('|'); flags && bar != std::string::npos) {  // (after the flags: another command, which is not run)
        message_ = "E488: Trailing characters: " + parts[2].substr(bar);
        return;
    }
    std::string fl = flags ? parts[2] : std::string();
    if (fl.starts_with('&')) fl = last_flags_ + fl.substr(1);
    auto has = [&](char f) { return fl.find(f) != std::string::npos; };
    const bool all = has('g'), ask = has('c'), count_only = has('n'), quiet = has('e');
    // The pattern (none typed: the last search's), and the case: i and I
    // say, over what it was ("\c" and "\C" in it still decide).
    Pattern p;
    const int icase = has('i') ? 1 : has('I') ? 0 : -1;
    if (parts[0].empty() && (icase < 0 || !pattern_.re)) {
        p = pattern_;
        if (icase >= 0) p.ignore_case = icase == 1;
    } else {
        VimRegex::Options o;
        o.ignore_case = icase == 1;
        o.last_sub = last_with_;
        p = compiled(parts[0].empty() ? pattern_.text : parts[0], o);
    }
    if (p.empty()) {
        message_ = "E35: No previous regular expression";
        return;
    }
    if (p.bad()) {
        message_ = p.error;
        return;
    }
    // "~" in what it becomes: what the last :s put in.
    std::string with;
    for (size_t k = 0; k < parts[1].size(); ++k) {
        if (parts[1][k] == '\\' && k + 1 < parts[1].size()) {
            with += parts[1][k];
            with += parts[1][++k];
        } else if (parts[1][k] == '~') {
            with += last_with_;
        } else {
            with += parts[1][k];
        }
    }
    if (!count_only && refuse()) return;
    last_sub_ = whole;
    last_flags_ = fl;
    last_with_ = with;
    pattern_ = p;
    highlight_on_ = true;
    const u64 last = std::min(line1, lines_of(ed) - 1);
    if (line0 > last) return;
    // Matches start on the lines (their ends included, for "$" and "\n"); they may run on past them.
    const u64 lo = ed.buf.line_start(line0), hi = ed.line_end(last) + 1;
    const u32 runs = overruns(p);
    if (count_only) {
        u64 count = 0, lines = 0, line_was = kNone;
        NextLines lines_after(ed.buf);
        each_whole(ed.buf, p, lo, hi, !all, [&](const RegexMatch& m) {
            ++count;
            if (line_was == kNone || m.start >= line_was) {
                ++lines;
                line_was = lines_after.after(m.start);
            }
            return true;
        });
        if (overruns(p) != runs) {
            message_ = VimRegex::kTooSlow;
            return;
        }
        if (count == 0) {
            if (!quiet) message_ = "E486: Pattern not found: " + p.text;
            return;
        }
        message_ = std::to_string(count) + (count == 1 ? " match on " : " matches on ") + std::to_string(lines) + (lines == 1 ? " line" : " lines");
        return;
    }
    if (ask) {
        confirm_ = Confirm{};
        confirm_.on = true;
        confirm_.p = p;
        confirm_.with = with;
        confirm_.next = lo;
        confirm_.end = hi;
        confirm_.per_line = !all;
        ed.begin_group();
        confirm_next(ed);
        if (confirm_.on || confirm_.count) return;
        if (!quiet || overruns(p) != runs) message_ = not_found(p, runs);
        return;
    }
    u64 lines = 0, at = 0;
    const u64 count = replace_all(ed, p, with, true, lo, hi, !all, &lines, &at);
    if (count == 0) {
        if (!quiet || overruns(p) != runs) message_ = not_found(p, runs);
        return;
    }
    go(ed, first_nonblank(ed, std::min(ed.buf.line_of(at), lines_of(ed) - 1)));
    if (count > 2 || lines > 1) message_ = std::to_string(count) + (count == 1 ? " substitution on " : " substitutions on ") + std::to_string(lines) + (lines == 1 ? " line" : " lines");
}

// :s///c: the next match, shown, and the question asked; or the end.
void Vim::confirm_next(Editor& ed) {
    Confirm& c = confirm_;
    bool found = false;
    each_whole(ed.buf, c.p, c.next, c.end, false, [&](const RegexMatch& m) {
        if (m.start == m.end && m.start == c.last_end) return true;  // (nothing, where the last one ended)
        c.m = m;
        found = true;
        return false;
    });
    if (!found) return confirm_end(ed);
    highlight_on_ = true;
    go(ed, c.m.start);
    message_ = "replace with " + c.with + " (y/n/a/q/l/^E/^Y)?";
}

void Vim::confirm_end(Editor& ed) {
    Confirm& c = confirm_;
    if (!c.on) return;
    c.on = false;
    ed.end_group();
    message_.clear();
    if (c.count == 0) return;
    go(ed, first_nonblank(ed, std::min(c.last_line, lines_of(ed) - 1)));
    if (c.count > 2 || c.lines > 1)
        message_ = std::to_string(c.count) + (c.count == 1 ? " substitution on " : " substitutions on ") + std::to_string(c.lines) + (c.lines == 1 ? " line" : " lines");
}

u32 Vim::confirm_key(Editor& ed, const VimKey& k) {
    Confirm& c = confirm_;
    // The match asked about, replaced; the next is looked for after it.
    auto take = [&] {
        std::string out;
        replacement(c.m, c.with, true, [&](u64 a, u64 b) { return ed.buf.text(a, b > a ? b - a : 0); }, &out, line_break(ed));
        ed.replace(c.m.start, c.m.end, out);
        const u64 e = c.m.start + out.size();
        c.end = c.end + out.size() - (c.m.end - c.m.start);
        const u64 line = ed.buf.line_of(c.m.start);
        if (line != c.last_line) ++c.lines;
        c.last_line = line;
        ++c.count;
        c.last_end = e;
        c.next = e;
        if (c.per_line) {
            char before = 0;
            if (e > c.m.start) ed.buf.read(e - 1, &before, 1);
            c.next = before == '\n' ? e : next_line_start(ed.buf, e);
        }
    };
    const u32 ch = k.kind == VimKey::kChar && !k.ctrl ? k.ch : 0;
    if (k.kind == VimKey::kEsc || ch == 'q' || (k.ctrl && (k.ch == 'c' || k.ch == '['))) {
        confirm_end(ed);
    } else if (ch == 'y') {
        take();
        confirm_next(ed);
    } else if (ch == 'l') {
        take();
        confirm_end(ed);
    } else if (ch == 'n') {
        c.last_end = c.m.end;
        c.next = c.per_line ? next_line_start(ed.buf, c.m.start) : std::max(c.m.end, c.m.anchor);
        confirm_next(ed);
    } else if (ch == 'a') {  // this one and all the rest, without asking
        take();
        bool same_line = false;  // (the next one is on the line of the one just taken: no line more)
        each_whole(ed.buf, c.p, c.next, c.end, c.per_line, [&](const RegexMatch& m) {
            same_line = ed.buf.line_of(m.start) == c.last_line;
            return false;
        });
        u64 lines = 0, last = 0;
        const u64 more = replace_all(ed, c.p, c.with, true, c.next, c.end, c.per_line, &lines, &last);
        if (more) {
            c.lines += lines - (same_line ? 1 : 0);
            c.last_line = ed.buf.line_of(last);
        }
        c.count += more;
        confirm_end(ed);
    }
    return kHandled;
}

void Vim::run_command(Editor& ed, std::string line) {
    size_t at = 0;
    auto skip = [&] {
        while (at < line.size() && line[at] == ' ') ++at;
    };
    skip();
    while (at < line.size() && line[at] == ':') ++at;
    // The lines it is about.
    const u64 n = lines_of(ed), here = ed.buf.line_of(ed.cursor());
    u64 line0 = here, line1 = here;
    bool ranged = false, unset = false;
    auto address = [&](u64* out) {
        skip();
        if (at >= line.size()) return false;
        if (std::isdigit(static_cast<u8>(line[at]))) {
            u64 v = 0;
            while (at < line.size() && std::isdigit(static_cast<u8>(line[at]))) v = v * 10 + static_cast<u64>(line[at++] - '0');
            *out = std::min(v ? v - 1 : 0, n - 1);
        } else if (line[at] == '.') {
            *out = here, ++at;
        } else if (line[at] == '$') {
            *out = n - 1, ++at;
        } else if (line.compare(at, 2, "'<") == 0 || line.compare(at, 2, "'>") == 0) {
            const u64 a = std::min(last_vstart_, last_vend_), b = std::max(last_vstart_, last_vend_);
            if (a == kNone) unset = true;  // (nothing has been selected yet)
            *out = ed.buf.line_of(std::min(line[at + 1] == '<' ? a : b, ed.buf.size()));
            at += 2;
        } else {
            return false;
        }
        return true;
    };
    if (at < line.size() && line[at] == '%') {
        line0 = 0, line1 = n - 1, ranged = true, ++at;
    } else if (address(&line0)) {
        ranged = true;
        line1 = line0;
        skip();
        if (at < line.size() && line[at] == ',') {
            ++at;
            if (!address(&line1)) line1 = line0;
        }
        if (line1 < line0) std::swap(line0, line1);
    }
    if (unset) {
        message_ = "E20: Mark not set";
        return;
    }
    skip();
    if (at >= line.size()) {  // a number alone: go to that line (in the cursor's column: 'startofline' is off)
        if (ranged) {
            if (line1 != here) remember_jump(ed, ed.cursor());
            go(ed, std::min(ed.off_at(line1, want_col_), line_last(ed, line1)), true);
        }
        return;
    }
    // :s and its delimiter, :> and :<, then the commands that are words.
    auto trailing = [&](std::string_view rest) { message_ = "E488: Trailing characters: " + std::string(rest); };
    if (line[at] == 's' && at + 1 < line.size() && line[at + 1] == '|') return trailing(line.substr(at + 1));  // (not a delimiter, as in Vim)
    if (line[at] == 's' && at + 1 < line.size() && !std::isalpha(static_cast<u8>(line[at + 1]))) return substitute(ed, line0, line1, std::string_view(line).substr(at + 1));
    if ((line[at] == '&' || line[at] == '>' || line[at] == '<') && line.find('|', at) != std::string::npos) return trailing(line.substr(line.find('|', at)));
    if ((line[at] == '&' || line.substr(at) == "s") && !last_sub_.empty()) return substitute(ed, line0, line1, last_sub_, line.substr(at) == "&&");  // :s and :& again, :&& with its flags
    if (line[at] == '>' || line[at] == '<') return operate_lines(ed, line[at], line0, line1);
    // What the application or a script does: :=EXPR, :lua and the like, :set, and commands with a capital.
    auto outside = [&](std::string name, std::string arg, bool bang) {
        VimRequest q{VimRequest::kEx, std::move(arg), 0, bang};
        q.name = std::move(name);
        q.line0 = line0, q.line1 = line1, q.ranged = ranged;
        requests_.push_back(std::move(q));
    };
    if (line[at] == '=') return outside("=", line.substr(at + 1), false);
    size_t end = at;
    while (end < line.size() && (std::isalnum(static_cast<u8>(line[end])) || (end > at && line[end] == '_'))) ++end;
    const std::string name = line.substr(at, end - at);
    const bool bang = end < line.size() && line[end] == '!';
    at = end + (bang ? 1 : 0);
    skip();
    std::string arg = line.substr(at);
    while (!arg.empty() && arg.back() == ' ') arg.pop_back();
    if (!name.empty() && std::isupper(static_cast<u8>(name[0])) && has_user_command && has_user_command(name)) return outside(name, arg, bang);
    auto is = [&](std::initializer_list<const char*> names) {
        for (const char* x : names)
            if (name == x) return true;
        return false;
    };
    // "|" does not run a second command: it is said, rather than taken
    // into a file's name or left out. (:lua, the mappings, :command and
    // the searches in files keep it as part of what they are given, as a
    // script's own command does.)
    if (const size_t bar = arg.find('|'); bar != std::string::npos && !is({"lua", "command", "com", "gr", "grep", "vim", "vimgrep", "Rg"}) && !name.ends_with("map"))
        return trailing(arg.substr(bar));
    auto lines_range = [&](u64* lo, u64* hi) {
        *lo = ed.buf.line_start(line0);
        *hi = line1 + 1 < ed.buf.line_count() ? ed.buf.line_start(line1 + 1) : ed.buf.size();
    };
    if (is({"w", "write"})) requests_.push_back({VimRequest::kSave, arg});
    else if (is({"q", "quit", "close", "clo", "bd", "bdelete", "bw", "bwipeout", "tabc", "tabclose"})) requests_.push_back({VimRequest::kClose, {}, 0, bang});
    else if (is({"wq", "x", "xit", "exit"})) requests_.push_back({VimRequest::kSave, arg, 0, false, true});
    else if (is({"wa", "wall"})) requests_.push_back({VimRequest::kSaveAll, {}});
    else if (is({"qa", "qall", "quita", "quitall"})) requests_.push_back({VimRequest::kQuitAll, {}, 0, bang});
    else if (is({"wqa", "wqall", "xa", "xall"})) requests_.push_back({VimRequest::kSaveAll, {}, 0, false, true});
    else if (is({"e", "edit"}) && bang && arg.empty()) requests_.push_back({VimRequest::kReload, {}});
    else if (is({"e", "edit", "tabe", "tabedit", "tabnew", "new", "enew"})) requests_.push_back(arg.empty() ? VimRequest{VimRequest::kTab, {}, 0} : VimRequest{VimRequest::kOpen, arg});
    else if (is({"bn", "bnext", "tabn", "tabnext"})) requests_.push_back({VimRequest::kTab, {}, 1});
    else if (is({"bp", "bprev", "bprevious", "bN", "bNext", "tabp", "tabprevious", "tabN", "tabNext"})) requests_.push_back({VimRequest::kTab, {}, -1});
    else if (is({"sp", "split"})) requests_.push_back({VimRequest::kSplit, arg, 1});
    else if (is({"vs", "vsp", "vsplit"})) requests_.push_back({VimRequest::kSplit, arg, 0});
    else if (is({"fin", "find", "Files"})) requests_.push_back({VimRequest::kFind, arg, 0});
    else if (is({"gr", "grep", "vim", "vimgrep", "Rg"})) requests_.push_back({VimRequest::kFind, arg, 1});
    else if (is({"noh", "nohl", "nohlsearch"})) highlight_on_ = false, requests_.push_back({VimRequest::kNoHighlight, {}});
    else if (is({"d", "de", "del", "delete"})) {
        u64 lo, hi;
        lines_range(&lo, &hi);
        operate(ed, 'd', lo, hi, true);
    } else if (is({"y", "ya", "yank"})) {
        u64 lo, hi;
        lines_range(&lo, &hi);
        operate(ed, 'y', lo, hi, true);
    } else if (is({"u", "un", "undo"})) {
        if (!refuse()) ed.undo(), go(ed, ed.cursor());
    } else if (is({"red", "redo"})) {
        if (!refuse()) ed.redo(), go(ed, ed.cursor());
    } else if (is({"se", "set", "setl", "setlocal", "lua", "luafile", "so", "source", "mes", "messages", "map", "nmap", "imap", "vmap", "xmap", "omap", "noremap",
                   "nnoremap", "inoremap", "vnoremap", "xnoremap", "onoremap", "unmap", "nunmap", "iunmap", "vunmap", "xunmap", "ounmap", "command", "com", "delcommand", "delc"})) {
        outside(name, arg, bang);
    } else {
        message_ = "E492: Not an editor command: " + line;
    }
}

// ---------------------------------------------------------------- keys in

u32 Vim::feed(Editor& ed, const VimKey& k) {
    typed_.clear();
    const bool idle = mode_ == kNormal && !op_ && !counted_ && !reg_ && await_ == kNothing;
    if (idle) failed_ = false, cmd_from_ = ed.cursor();
    if (macro_into_ && macro_depth_ == 0 && !replaying_) macro_keys_.push_back(k);
    if (!replaying_ && macro_depth_ == 0) {
        if (idle) {  // this key starts a command: what it changes, "." will do again
            current_.clear();
            changing_ = false;
        }
        current_.push_back(k);
    }
    u32 r = 0;
    if (confirm_.on) return confirm_key(ed, k);  // (:s///c asks about each match)
    // What a command changes is one step to undo, and undone it puts the
    // cursor back where the command began, as Vim does: before "A" took it
    // to the end of the line, "v" began a selection, ":" was typed. (Insert
    // mode has its group open already.)
    const bool grouped = mode_ != kInsert && mode_ != kReplace;
    if (grouped) ed.begin_group(cmd_from_);
    switch (mode_) {
        case kInsert:
        case kReplace:
            r = insert_key(ed, k);
            if ((r & kTyped) && !is_enter(k)) auto_indent_ = false;  // (something typed after the indent: it stays)
            break;
        case kCommand: r = command_key(ed, k); break;
        default: r = normal_key(ed, k); break;
    }
    if (grouped) ed.end_group();
    if (mode_ == kNormal && !op_ && !counted_ && !reg_ && await_ == kNothing) {
        if (changing_ && !replaying_ && macro_depth_ == 0 && !(current_.size() == 1 && current_[0].kind == VimKey::kChar && current_[0].ch == '.')) dot_ = current_;
        if (changing_) last_change_ = ed.cursor();
        changing_ = false;
        if (insert_once_ == 2) {  // <C-o>, just now: the next command is the one
            insert_once_ = 1;
        } else if (insert_once_ == 1) {  // and that was it
            insert_once_ = 0;
            mode_ = kInsert;
            // Typing was going on at the end of the line: it goes on there,
            // if the command left the cursor on that line's last character.
            const u64 at = ed.cursor(), l = ed.buf.line_of(at), end = ed.line_end(l);
            if (once_eol_ && l == once_line_ && at < end && ed.next_char(at) == end) ed.move_to(end, false);
        }
    }
    return r;
}

void Vim::keys(Editor& ed, std::string_view s) {
    static const struct {
        const char* name;
        u8 kind;
        u32 ch;
    } named[] = {{"Esc", VimKey::kEsc, 0},     {"CR", VimKey::kEnter, 0},   {"Enter", VimKey::kEnter, 0}, {"BS", VimKey::kBackspace, 0},
                 {"Tab", VimKey::kTab, 0},     {"Left", VimKey::kLeft, 0},  {"Right", VimKey::kRight, 0}, {"Up", VimKey::kUp, 0},
                 {"Down", VimKey::kDown, 0},   {"Home", VimKey::kHome, 0},  {"End", VimKey::kEnd, 0},     {"Del", VimKey::kDelete, 0},
                 {"PageUp", VimKey::kPageUp, 0}, {"PageDown", VimKey::kPageDown, 0}, {"Space", VimKey::kChar, ' '}, {"lt", VimKey::kChar, '<'}};
    for (size_t i = 0; i < s.size();) {
        VimKey k;
        const size_t close = s[i] == '<' ? s.find('>', i) : std::string_view::npos;
        if (close != std::string_view::npos && close > i + 1) {
            const std::string_view name = s.substr(i + 1, close - i - 1);
            bool known = false;
            if (name.size() == 3 && (name[0] == 'C' || name[0] == 'c') && name[1] == '-') {
                k.ch = static_cast<u32>(lower(name[2]));
                k.ctrl = true;
                known = true;
            }
            for (const auto& x : named)
                if (name == x.name) k.kind = x.kind, k.ch = x.ch, known = true;
            if (known) {
                feed(ed, k);
                i = close + 1;
                continue;
            }
        }
        const Decoded d = decode_utf8(reinterpret_cast<const u8*>(s.data()) + i, s.size() - i);
        k.ch = d.cp;
        if (k.ch == '\n') k.kind = VimKey::kEnter, k.ch = 0;
        feed(ed, k);
        i += d.len;
    }
}

}  // namespace blit
