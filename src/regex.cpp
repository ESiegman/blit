#include "regex.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "fuzzy.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

constexpr u32 kNoChar = 0xFFFFFFFFu;  // before the start of the text, or past its end
constexpr u64 kNone = ~0ull;
constexpr u32 kInf = 0xFFFFFFFFu;     // a count with no top ("*", "\{2,}")
constexpr size_t kMostCode = 1 << 17; // instructions a pattern may compile to ("\{1000}" of something long)

// Vim's word characters ('iskeyword' as it comes): letters, digits, the
// underscore, and every character past ASCII.
bool word_char(u32 c) { return c != kNoChar && (c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'); }

// Lower case, for letters that have it: ASCII, Latin-1, Latin Extended-A,
// Greek and Cyrillic. (Enough for \c to find "École" from "école".)
u32 fold(u32 c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        if (c == 0x178) return 0xFF;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return c | 1;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

// And upper case, the other way.
u32 upper(u32 c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        return c & ~1u;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

bool cased(u32 c) { return fold(c) != c || upper(c) != c; }

void put_utf8(std::string& s, u32 cp) {
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
}

// ---------------------------------------------------------------- the program

struct CharClass {
    std::vector<std::pair<u32, u32>> ranges;
    bool negate = false;
    bool newline = false;  // a line break too (\_x, [\n])
    bool cr = false;       // "\r" is in it by name (so it takes the one before a line's "\n")
    u64 ascii[2] = {};     // which ASCII characters it takes, case and negation worked in
    bool raw(u32 c) const {
        for (const auto& r : ranges)
            if (c >= r.first && c <= r.second) return true;
        return false;
    }
    bool in(u32 c, bool icase) const { return (raw(c) || (icase && (raw(fold(c)) || raw(upper(c))))) != negate; }
    void finish(bool icase) {
        for (u32 c = 0; c < 128; ++c)
            if (c == '\n' ? newline : in(c, icase)) ascii[c >> 6] |= 1ull << (c & 63);
    }
    bool test(u32 c, bool icase) const { return c < 128 ? ((ascii[c >> 6] >> (c & 63)) & 1) != 0 : in(c, icase); }
    void add(u32 a, u32 b) { ranges.push_back({a, b}); }
};

// The last five are the backtracker's alone (a program for the Pike VM or
// the DFA never has them): \1 to \9, look-around and \@>, and the check
// that a turn of a "*" took something.
enum Op : u8 { kChar, kCharI, kAny, kClass, kNl, kMatch, kSplit, kJmp, kSave, kAssert, kBackref, kLook, kLookEnd, kMark, kCheck };
// After kEof, the backtracker's alone too: a line or a column by number
// (\%23l, \%<23l, \%>23l, \%23c and the like).
enum Assert : u8 { kBol, kEol, kBow, kEow, kBof, kEof, kLineIs, kColIs };
enum Compare : u8 { kAt, kBefore, kAfter };
enum LookKind : u8 { kAhead, kNotAhead, kBehind, kNotBehind, kAtomic };

struct Inst {
    Op op;
    u8 a = 0;  // kAny: 1 if it takes a line break too; kAssert: which
    u32 x = 0, y = 0;
};

// A look-around (\@= \@! \@<= \@<! \@>) in the backtracker's program: its
// own program runs from the kLook to its kLookEnd; `end` is just after that.
struct Look {
    u8 kind = kAhead;
    u32 end = 0;
    u32 bytes = 0;                // \@123<=: no further back than that (0: no limit)
    u32 least = 0, most = 0;      // the characters it can take (most: kInf, any number)
};

}  // namespace

struct RegexProgram {
    std::vector<Inst> code;
    std::vector<CharClass> classes;
    u32 groups = 0;  // the highest \( \) group (0 to 9)
    u32 slots = 3;   // start, end, the groups' two each, where the thread began (and the backtracker's marks)
    bool icase = false, multiline = false, zs = false, empty = false, cr = false;
    std::string required;  // plain text every match has (ASCII, if the case is ignored)
    bool first[256] = {};  // the bytes a match can start at
    bool skip = false;     // and they are few enough to be worth looking for
    u32 anchor_slot() const { return anchor; }
    u32 anchor = 2;  // where the thread began: the slot after the groups'
    // A pattern with back-references, look-around, \& or a line's number
    // runs on the backtracker. Its lines are found first by `relaxed`, a
    // program for the DFA whose matches take in all of this one's (what
    // those things ask is left out), when there is one.
    bool backtrack = false;
    std::vector<Look> looks;
    std::unique_ptr<RegexProgram> relaxed;
    // With no back-reference, what is ahead of the backtracker at a place
    // in the program and the text does not depend on how it got there: the
    // places it has tried (at each kSplit) are remembered and not tried
    // again, as RE2's BitState does. That makes it find just what the Pike
    // VM would (which never runs two threads at one place), in time the
    // text's length times the program's. split_of: each kSplit's number
    // (kNoChar elsewhere); scope_of: for each, 0 or the look-around it is
    // in, plus one (what a look-behind finds depends on where it must end,
    // so its places are forgotten each time it is asked).
    bool memo = false;
    std::vector<u32> split_of, scope_of;
    // What the backtracker's program asks first, before any choice: a
    // match then starts only at a line's start or a word's, and the places
    // between are passed over without a try.
    enum Lead : u8 { kLeadAny, kLeadLine, kLeadWord };
    u8 lead = kLeadAny;
};

namespace {

// ---------------------------------------------------------------- reading a pattern

struct Node {
    enum Kind : u8 { kEmpty, kLit, kAnyN, kClassN, kNlN, kCat, kAlt, kRep, kGroup, kAssertN, kZsN, kZeN, kBackrefN, kLookN } kind = kEmpty;
    // kLit: the character; kClassN: which class; kAssertN: which; kGroup:
    // the group (0: one that catches nothing); kBackrefN: the group it
    // stands for; kLookN: its LookKind.
    u32 c = 0;
    bool nl = false;
    u32 min = 0, max = 0;  // kRep: how many; kLookN: min is \@123<='s 123; kAssertN: min the number, max its Compare
    bool greedy = true;
    std::vector<u32> kids;
};

struct Tok {
    enum K : u8 { kEnd, kChar, kMagic, kClassT, kNlT, kZs, kZe, kBof, kEof, kBol, kEol, kOpenPlain, kCollection, kBackrefT, kOptional, kLineT, kColT } k = kEnd;
    u32 c = 0;
    bool nl = false;  // \_x
    u32 n = 0;        // kLineT, kColT: the number
};

class Parser {
public:
    Parser(std::string_view s, const VimRegex::Options& o) : s_(s), opt_(o) {}

    std::vector<Node> nodes;
    std::vector<CharClass> classes;
    u32 groups = 0;
    bool set_icase = false, set_noicase = false;
    bool backtrack = false;  // something in it only the backtracker can do
    bool backref = false;    // a back-reference in particular
    int engine = 0;          // \%#=1 first: the backtracker for it all (as Vim's old engine); \%#=2, \%#=0: as it comes
    std::string error;

    u32 parse() {
        if (s_.size() >= 5 && s_.starts_with("\\%#=") && s_[4] >= '0' && s_[4] <= '2') {
            engine = s_[4] - '0';
            i_ = 5;
        }
        const u32 root = alternation();
        if (error.empty() && tok().k != Tok::kEnd) {
            if (tok().k == Tok::kMagic && tok().c == ')') error = level_ == 3 ? "E54: Unmatched )" : "E54: Unmatched \\)";
            else error = "E383: Invalid search string";
        }
        return root;
    }

private:
    std::string_view s_;
    VimRegex::Options opt_;
    size_t i_ = 0;
    int level_ = 2;  // 0 \V, 1 \M, 2 \m (magic), 3 \v
    bool at_start_ = true;
    bool have_ = false;
    Tok tok_;
    int depth_ = 0;
    bool closed_[10] = {};  // the groups whose "\)" has been read (a back-reference may only come after it)

    u32 node(Node n) {
        nodes.push_back(std::move(n));
        return static_cast<u32>(nodes.size() - 1);
    }
    u32 lit(u32 c) {
        Node n;
        n.kind = Node::kLit;
        n.c = c;
        return node(std::move(n));
    }

    u32 decode_at(size_t at, size_t* len) const {
        const Decoded d = decode_utf8(reinterpret_cast<const u8*>(s_.data()) + at, s_.size() - at);
        *len = d.len;
        return d.cp;
    }

    // Skips \c \C \v \m \M \V \Z, wherever they are.
    bool flag_at(size_t at) const {
        return at + 1 < s_.size() && s_[at] == '\\' && std::strchr("cCvmMVZ", s_[at + 1]) && s_[at + 1] != 0;
    }

    // Is a "$" here the end of a line (and not a dollar)? At the end of the
    // pattern, or before "\|", "\)", "\&" or "\n" (or "|", ")", "&" with \v).
    bool dollar_magic(size_t at) const {
        if (level_ == 3) return true;
        size_t p = at + 1;
        bool all = level_ == 3;
        while (flag_at(p)) {
            if (s_[p + 1] == 'v') all = true;
            else if (s_[p + 1] == 'm' || s_[p + 1] == 'M' || s_[p + 1] == 'V') all = false;
            p += 2;
        }
        if (p >= s_.size()) return true;
        if (s_[p] == '\\' && p + 1 < s_.size() && std::strchr("|&)n", s_[p + 1])) return true;
        return all && std::strchr("|&)", s_[p]);
    }

    // Reads a number in the given base, at most `most` digits.
    u32 number(int base, int most) {
        u32 v = 0;
        int n = 0;
        while (i_ < s_.size() && n < most) {
            const char ch = s_[i_];
            int d = -1;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            if (d < 0 || d >= base) break;
            v = v * static_cast<u32>(base) + static_cast<u32>(d);
            ++i_, ++n;
        }
        if (n == 0) error = "E678: Invalid character after \\%[dxouU]";
        return v;
    }

    Tok read() {
        Tok t;
        for (;;) {
            if (i_ >= s_.size()) return t;
            if (!flag_at(i_)) break;
            const char f = s_[i_ + 1];
            if (f == 'c') set_icase = true;
            else if (f == 'C') set_noicase = true;
            else if (f == 'v') level_ = 3;
            else if (f == 'm') level_ = 2;
            else if (f == 'M') level_ = 1;
            else if (f == 'V') level_ = 0;
            i_ += 2;
        }
        size_t len = 1;
        const u32 ch = static_cast<u8>(s_[i_]);
        // The characters that may be magic, and when they are without a backslash.
        auto bare_magic = [&](u32 c) {
            switch (c) {
                case '^': return level_ == 3 || (level_ >= 1 && at_start_);
                case '$': return level_ >= 1 && dollar_magic(i_);
                case '.':
                case '*':
                case '[':
                case '~': return level_ >= 2;
                case '(':
                case ')':
                case '|':
                case '+':
                case '=':
                case '?':
                case '{':
                case '@':
                case '%':
                case '<':
                case '>':
                case '&': return level_ == 3;
                default: return false;
            }
        };
        if (ch != '\\') {
            if (ch < 0x80 && std::strchr("^$.*[~()|+=?{@%<>&", static_cast<char>(ch)) && ch != 0 && bare_magic(ch)) {
                ++i_;
                t.k = Tok::kMagic;
                t.c = ch;
                return magic(t);
            }
            t.k = Tok::kChar;
            t.c = decode_at(i_, &len);
            i_ += len;
            return t;
        }
        // A backslash.
        if (i_ + 1 >= s_.size()) {  // at the very end: itself
            ++i_;
            t.k = Tok::kChar;
            t.c = '\\';
            return t;
        }
        const char n = s_[i_ + 1];
        i_ += 2;
        if (n == '^' || n == '$') {  // "\^" and "\$": magic after \V, else the character
            if (level_ == 0) {
                t.k = n == '^' ? Tok::kBol : Tok::kEol;
                return t;
            }
            t.k = Tok::kChar;
            t.c = static_cast<u8>(n);
            return t;
        }
        if (std::strchr(".*[~()|+=?{@%<>&", n)) {  // the other way round from without the backslash
            if (!bare_magic(static_cast<u8>(n))) {
                t.k = Tok::kMagic;
                t.c = static_cast<u8>(n);
                return magic(t);
            }
            t.k = Tok::kChar;
            t.c = static_cast<u8>(n);
            return t;
        }
        switch (n) {
            case 'n': t.k = Tok::kNlT; return t;
            case 't': t.k = Tok::kChar, t.c = '\t'; return t;
            case 'e': t.k = Tok::kChar, t.c = 27; return t;
            case 'r': t.k = Tok::kChar, t.c = '\r'; return t;
            case 'b': t.k = Tok::kChar, t.c = 8; return t;
            case 'z':
                if (i_ < s_.size() && (s_[i_] == 's' || s_[i_] == 'e')) {
                    t.k = s_[i_] == 's' ? Tok::kZs : Tok::kZe;
                    ++i_;
                    return t;
                }
                error = "E68: Invalid character after \\z (only \\zs and \\ze are known here)";
                return t;
            case '_': {
                if (i_ >= s_.size()) {
                    error = "E63: Invalid use of \\_";
                    return t;
                }
                const char x = s_[i_++];
                if (x == '^') t.k = Tok::kBol;
                else if (x == '$') t.k = Tok::kEol;
                else if (x == '.') t.k = Tok::kMagic, t.c = '.', t.nl = true;
                else if (x == '[') t.k = Tok::kCollection, t.nl = true;
                else if (std::strchr("dDwWsSaAlLuUxXoOhHiIkKfFpP", x) && x != 0) t.k = Tok::kClassT, t.c = static_cast<u8>(x), t.nl = true;
                else error = "E63: Invalid use of \\_";
                return t;
            }
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': t.k = Tok::kBackrefT, t.c = static_cast<u32>(n - '0'); return t;
            case '/':
            case '\\':
            case '?': t.k = Tok::kChar, t.c = static_cast<u8>(n); return t;
            default:
                if (std::strchr("dDwWsSaAlLuUxXoOhHiIkKfFpP", n)) {
                    t.k = Tok::kClassT;
                    t.c = static_cast<u8>(n);
                    return t;
                }
                // Anything else: the character itself.
                i_ -= 1;
                t.k = Tok::kChar;
                t.c = decode_at(i_, &len);
                i_ += len;
                return t;
        }
    }

    // A magic "%", "[", "^" or "$": what it is.
    Tok magic(Tok t) {
        if (t.c == '^') {
            t.k = Tok::kBol;
            return t;
        }
        if (t.c == '$') {
            t.k = Tok::kEol;
            return t;
        }
        if (t.c == '[') {
            t.k = Tok::kCollection;
            return t;
        }
        if (t.c != '%') return t;
        if (i_ >= s_.size()) {
            error = "E71: Invalid character after \\%";
            return t;
        }
        // A line or a column by number: "\%23l", before it "\%<23l", after it "\%>23l" (and "c").
        {
            size_t k = i_;
            const u32 cmp = s_[k] == '<' ? kBefore : s_[k] == '>' ? kAfter : kAt;
            if (cmp != kAt) ++k;
            if (k < s_.size() && s_[k] >= '0' && s_[k] <= '9') {
                u64 v = 0;
                while (k < s_.size() && s_[k] >= '0' && s_[k] <= '9') v = std::min<u64>(v * 10 + static_cast<u64>(s_[k++] - '0'), 0xFFFFFFFFu);
                if (k < s_.size() && (s_[k] == 'l' || s_[k] == 'c')) {
                    t.k = s_[k] == 'l' ? Tok::kLineT : Tok::kColT;
                    t.c = cmp;
                    t.n = static_cast<u32>(v);
                    i_ = k + 1;
                    return t;
                }
                error = "E71: Invalid character after \\%: of the numbered kinds, a line (l) and a column (c) are known here";
                return t;
            }
        }
        const char x = s_[i_++];
        switch (x) {
            case '(': t.k = Tok::kOpenPlain; return t;
            case '[': t.k = Tok::kOptional; return t;
            case '^': t.k = Tok::kBof; return t;
            case '$': t.k = Tok::kEof; return t;
            case 'd': t.k = Tok::kChar, t.c = number(10, 10); return t;
            case 'x': t.k = Tok::kChar, t.c = number(16, 2); return t;
            case 'u': t.k = Tok::kChar, t.c = number(16, 4); return t;
            case 'U': t.k = Tok::kChar, t.c = number(16, 8); return t;
            case 'o': t.k = Tok::kChar, t.c = number(8, 4); return t;
            default: error = std::string("E71: Invalid character after \\%: \\%") + x + " is not supported"; return t;
        }
    }

    const Tok& tok() {
        if (!have_) {
            tok_ = read();
            have_ = true;
        }
        return tok_;
    }
    void take() {
        have_ = false;
        // "^" is a line's start just after these, as at the pattern's start.
        at_start_ = (tok_.k == Tok::kMagic && (tok_.c == '(' || tok_.c == '|' || tok_.c == '&')) || tok_.k == Tok::kOpenPlain || tok_.k == Tok::kNlT ||
                    tok_.k == Tok::kBol;
    }

    u32 alternation() {
        std::vector<u32> branches{branch()};
        while (error.empty() && tok().k == Tok::kMagic && tok_.c == '|') {
            take();
            branches.push_back(branch());
        }
        if (branches.size() == 1) return branches[0];
        Node n;
        n.kind = Node::kAlt;
        n.kids = std::move(branches);
        return node(std::move(n));
    }

    u32 look(u32 kind, u32 of, u32 bytes = 0) {
        Node n;
        n.kind = Node::kLookN;
        n.c = kind;
        n.min = bytes;
        n.kids = {of};
        backtrack = true;
        return node(std::move(n));
    }

    // "a\&b\&c": c, where a and b match as well (each a look-ahead, as Vim
    // has it: "foobar\&foo" is "\(foobar\)\@=foo").
    u32 branch() {
        u32 last = concat();
        if (!(error.empty() && tok().k == Tok::kMagic && tok_.c == '&')) return last;
        std::vector<u32> parts;
        while (error.empty() && tok().k == Tok::kMagic && tok_.c == '&') {
            take();
            parts.push_back(look(kAhead, last));
            last = concat();
        }
        parts.push_back(last);
        Node n;
        n.kind = Node::kCat;
        n.kids = std::move(parts);
        return node(std::move(n));
    }

    u32 concat() {
        std::vector<u32> parts;
        while (error.empty()) {
            const Tok& t = tok();
            if (t.k == Tok::kEnd || (t.k == Tok::kMagic && (t.c == '|' || t.c == '&' || t.c == ')'))) break;
            const u32 p = piece();
            if (p != kNoChar) parts.push_back(p);
        }
        if (parts.size() == 1) return parts[0];
        Node n;
        n.kind = parts.empty() ? Node::kEmpty : Node::kCat;
        n.kids = std::move(parts);
        return node(std::move(n));
    }

    bool is_multi(const Tok& t) const { return t.k == Tok::kMagic && (t.c == '*' || t.c == '+' || t.c == '=' || t.c == '?' || t.c == '{' || t.c == '@'); }

    u32 piece() {
        // A multi with nothing before it is the character it is written with ("/*" looks for a star).
        if (is_multi(tok())) {
            const u32 c = tok_.c;
            take();
            if (c == '{' || c == '@') {
                error = c == '{' ? "E64: \\{ follows nothing" : "E64: \\@ follows nothing";
                return kNoChar;
            }
            return lit(c);
        }
        u32 a = atom();
        if (!error.empty() || a == kNoChar) return a;
        while (error.empty() && is_multi(tok())) {
            const u32 c = tok_.c;
            take();
            if (c == '@') {
                a = look_around(a);
                if (a == kNoChar) return kNoChar;
                continue;
            }
            Node r;
            r.kind = Node::kRep;
            r.kids = {a};
            if (c == '*') r.min = 0, r.max = kInf;
            else if (c == '+') r.min = 1, r.max = kInf;
            else if (c == '=' || c == '?') r.min = 0, r.max = 1;
            else if (!brace(&r)) return kNoChar;
            a = node(std::move(r));
        }
        return a;
    }

    // After "\@": "=" "!" ">" "<=" "<!", with a number of bytes first for
    // the last two ("\@12<=": no further back than that).
    u32 look_around(u32 of) {
        u64 bytes = 0;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') bytes = std::min<u64>(bytes * 10 + static_cast<u64>(s_[i_++] - '0'), 1u << 30);
        const char k = i_ < s_.size() ? s_[i_++] : 0;
        const char k2 = k == '<' && i_ < s_.size() ? s_[i_++] : 0;
        u32 kind;
        if (k == '=') kind = kAhead;
        else if (k == '!') kind = kNotAhead;
        else if (k == '>') kind = kAtomic;
        else if (k == '<' && k2 == '=') kind = kBehind;
        else if (k == '<' && k2 == '!') kind = kNotBehind;
        else {
            error = "E59: Invalid character after \\@";
            return kNoChar;
        }
        return look(kind, of, static_cast<u32>(bytes));
    }

    // "\%[abc]": as much of a, then b, then c as is there ("r\%[ead]" is r,
    // re, rea or read): each an optional atom inside the one before.
    u32 optional_seq() {
        std::vector<u32> items;
        for (;;) {
            const Tok& t = tok();
            if (!error.empty()) return kNoChar;
            if (t.k == Tok::kEnd) {
                error = "E69: Missing ] after \\%[";
                return kNoChar;
            }
            if (t.k == Tok::kChar && t.c == ']') {
                take();
                break;
            }
            if ((t.k == Tok::kMagic && std::strchr("()|&", static_cast<char>(t.c))) || is_multi(t) || t.k == Tok::kOpenPlain || t.k == Tok::kOptional) {
                error = "E369: Invalid item in \\%[]";
                return kNoChar;
            }
            const u32 a = atom();
            if (!error.empty() || a == kNoChar) return kNoChar;
            items.push_back(a);
        }
        if (items.empty()) {
            error = "E70: Empty \\%[]";
            return kNoChar;
        }
        u32 inner = kNoChar;
        for (size_t k = items.size(); k-- > 0;) {
            u32 body = items[k];
            if (inner != kNoChar) {
                Node c;
                c.kind = Node::kCat;
                c.kids = {items[k], inner};
                body = node(std::move(c));
            }
            Node r;
            r.kind = Node::kRep;
            r.min = 0, r.max = 1;
            r.kids = {body};
            inner = node(std::move(r));
        }
        return inner;
    }

    // Is there a look-behind further on? (Vim lets a back-reference come
    // before its group then, the group being in the look-behind.)
    bool behind_later() const {
        for (size_t k = i_; k + 2 < s_.size(); ++k)
            if (s_[k] == '@' && s_[k + 1] == '<' && (s_[k + 2] == '=' || s_[k + 2] == '!')) return true;
        return false;
    }

    // "\{n,m}" and its kinds, after the "{": "-" first for as few as may be.
    bool brace(Node* r) {
        if (i_ < s_.size() && s_[i_] == '-') r->greedy = false, ++i_;
        auto num = [&](u32* out) {
            if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return false;
            u64 v = 0;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') v = std::min<u64>(v * 10 + static_cast<u64>(s_[i_++] - '0'), 100000);
            *out = static_cast<u32>(v);
            return true;
        };
        u32 a = 0, b = 0;
        const bool has_a = num(&a);
        if (i_ < s_.size() && s_[i_] == ',') {
            ++i_;
            const bool has_b = num(&b);
            r->min = has_a ? a : 0;
            r->max = has_b ? b : kInf;
        } else {
            r->min = r->max = has_a ? a : 0;
            if (!has_a) r->max = kInf;  // "\{}" is "*"
        }
        if (i_ < s_.size() && s_[i_] == '\\') ++i_;
        if (i_ >= s_.size() || s_[i_] != '}') {
            error = "E554: Syntax error in \\{...}";
            return false;
        }
        ++i_;
        if (r->max != kInf && r->min > r->max) std::swap(r->min, r->max);  // (Vim takes them either way round)
        return true;
    }

    u32 cls(u32 letter, bool nl) {
        CharClass c;
        const u32 low = letter | 0x20;
        c.negate = letter != low;  // (\D is not \d; but \I, \K, \F and \P are theirs without digits, below)
        auto digits = [&] { c.add('0', '9'); };
        switch (low) {
            case 'd': digits(); break;
            case 'w': digits(), c.add('A', 'Z'), c.add('a', 'z'), c.add('_', '_'); break;
            case 's': c.add(' ', ' '), c.add('\t', '\t'); break;
            case 'a': c.add('A', 'Z'), c.add('a', 'z'); break;
            case 'l': c.add('a', 'z'); break;
            case 'u': c.add('A', 'Z'); break;
            case 'x': digits(), c.add('A', 'F'), c.add('a', 'f'); break;
            case 'o': c.add('0', '7'); break;
            case 'h': c.add('A', 'Z'), c.add('a', 'z'), c.add('_', '_'); break;
            case 'i':
            case 'k':
                if (letter == low) digits();
                c.add('A', 'Z'), c.add('a', 'z'), c.add('_', '_'), c.add(0x80, 0x10FFFF);
                c.negate = false;
                break;
            case 'f':
                if (letter == low) digits();
                c.add('A', 'Z'), c.add('a', 'z');
                for (const char* p = "/.-_+,#$%~="; *p; ++p) c.add(static_cast<u8>(*p), static_cast<u8>(*p));
                c.add(0x80, 0x10FFFF);
                c.negate = false;
                break;
            case 'p':
                c.add(' ', '~'), c.add(0xA0, 0x10FFFF);
                if (letter != low) {  // printable, not a digit
                    c.ranges = {{' ', '/'}, {':', '~'}, {0xA0, 0x10FFFF}};
                }
                c.negate = false;
                break;
            default: break;
        }
        c.newline = nl;
        classes.push_back(std::move(c));
        Node n;
        n.kind = Node::kClassN;
        n.c = static_cast<u32>(classes.size() - 1);
        return node(std::move(n));
    }

    // "[...]", from just after the "[". With no "]" to close it, the "[" is itself.
    u32 collection(bool nl) {
        const size_t start = i_;
        CharClass c;
        c.newline = nl;
        if (i_ < s_.size() && s_[i_] == '^') c.negate = true, ++i_;
        bool first = true;
        auto named = [&](std::string_view name) {
            if (name == "alpha") c.add('A', 'Z'), c.add('a', 'z');
            else if (name == "digit") c.add('0', '9');
            else if (name == "alnum") c.add('0', '9'), c.add('A', 'Z'), c.add('a', 'z');
            else if (name == "lower") c.add('a', 'z'), c.add(0xDF, 0xFF), c.add(0x3B1, 0x3C9), c.add(0x430, 0x45F);
            else if (name == "upper") c.add('A', 'Z'), c.add(0xC0, 0xDE), c.add(0x391, 0x3A9), c.add(0x400, 0x42F);
            else if (name == "space") c.add(' ', ' '), c.add('\t', '\r');
            else if (name == "blank") c.add(' ', ' '), c.add('\t', '\t');
            else if (name == "punct") c.add('!', '/'), c.add(':', '@'), c.add('[', '`'), c.add('{', '~');
            else if (name == "xdigit") c.add('0', '9'), c.add('A', 'F'), c.add('a', 'f');
            else if (name == "cntrl") c.add(1, 31), c.add(127, 127);
            else if (name == "print") c.add(' ', '~'), c.add(0xA0, 0x10FFFF);
            else if (name == "graph") c.add('!', '~'), c.add(0xA1, 0x10FFFF);
            else if (name == "return") c.add('\r', '\r'), c.cr = true;
            else if (name == "tab") c.add('\t', '\t');
            else if (name == "escape") c.add(27, 27);
            else if (name == "backspace") c.add(8, 8);
            else if (name == "ident" || name == "keyword") c.add('0', '9'), c.add('A', 'Z'), c.add('a', 'z'), c.add('_', '_'), c.add(0x80, 0x10FFFF);
            else if (name == "fname") c.add('0', '9'), c.add('A', 'Z'), c.add('a', 'z'), c.add(0x80, 0x10FFFF);
            else return false;
            return true;
        };
        // One character of it, escapes read; kNoChar where it was a named class.
        auto item = [&](bool* was_class) -> u32 {
            *was_class = false;
            if (s_[i_] == '[' && i_ + 1 < s_.size() && (s_[i_ + 1] == ':' || s_[i_ + 1] == '=' || s_[i_ + 1] == '.')) {
                const char kind = s_[i_ + 1];
                const size_t close = s_.find(std::string{kind, ']'}, i_ + 2);
                if (close != std::string_view::npos) {
                    const std::string_view name = s_.substr(i_ + 2, close - i_ - 2);
                    if (kind == ':') {
                        if (named(name)) {
                            i_ = close + 2;
                            *was_class = true;
                            return kNoChar;
                        }
                    } else if (!name.empty()) {  // [=a=] and [.a.]: the character
                        size_t len;
                        const u32 cp = decode_at(i_ + 2, &len);
                        i_ = close + 2;
                        return cp;
                    }
                }
            }
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                const char e = s_[i_ + 1];
                switch (e) {
                    case 'e': i_ += 2; return 27;
                    case 't': i_ += 2; return '\t';
                    case 'r': i_ += 2; c.cr = true; return '\r';
                    case 'b': i_ += 2; return 8;
                    case 'n':
                        i_ += 2;
                        c.newline = true;
                        *was_class = true;
                        return kNoChar;
                    case 'd': i_ += 2; return number(10, 10);
                    case 'o': i_ += 2; return number(8, 4);
                    case 'x': i_ += 2; return number(16, 2);
                    case 'u': i_ += 2; return number(16, 4);
                    case 'U': i_ += 2; return number(16, 8);
                    case '\\':
                    case ']':
                    case '^':
                    case '-': i_ += 2; return static_cast<u8>(e);
                    default: ++i_; return '\\';  // (a backslash before anything else is itself)
                }
            }
            size_t len;
            const u32 cp = decode_at(i_, &len);
            i_ += len;
            return cp;
        };
        for (;;) {
            if (i_ >= s_.size()) {  // never closed: a "[" as it stands
                i_ = start;
                return lit('[');
            }
            if (s_[i_] == ']' && !first) {
                ++i_;
                break;
            }
            first = false;
            bool was_class;
            const u32 a = item(&was_class);
            if (!error.empty()) return kNoChar;
            if (was_class) continue;
            if (i_ + 1 < s_.size() && s_[i_] == '-' && s_[i_ + 1] != ']') {
                ++i_;
                bool b_class;
                const u32 b = item(&b_class);
                if (b_class) {  // (a range up to a class is no range)
                    c.add(a, a), c.add('-', '-');
                    continue;
                }
                if (b < a) {
                    error = "E944: Reverse range in character class";
                    return kNoChar;
                }
                c.add(a, b);
                if (a <= '\r' && '\r' <= b) c.cr = true;
            } else {
                c.add(a, a);
                if (a == '\r') c.cr = true;
            }
        }
        if (c.negate) c.cr = false;
        classes.push_back(std::move(c));
        Node n;
        n.kind = Node::kClassN;
        n.c = static_cast<u32>(classes.size() - 1);
        return node(std::move(n));
    }

    u32 atom() {
        const Tok t = tok();
        take();
        Node n;
        switch (t.k) {
            case Tok::kChar: return lit(t.c);
            case Tok::kNlT: n.kind = Node::kNlN; return node(std::move(n));
            case Tok::kClassT: return cls(t.c, t.nl);
            case Tok::kCollection: return collection(t.nl);
            case Tok::kZs: n.kind = Node::kZsN; return node(std::move(n));
            case Tok::kZe: n.kind = Node::kZeN; return node(std::move(n));
            case Tok::kBol: n.kind = Node::kAssertN, n.c = kBol; return node(std::move(n));
            case Tok::kEol: n.kind = Node::kAssertN, n.c = kEol; return node(std::move(n));
            case Tok::kBof: n.kind = Node::kAssertN, n.c = kBof; return node(std::move(n));
            case Tok::kEof: n.kind = Node::kAssertN, n.c = kEof; return node(std::move(n));
            case Tok::kLineT:
            case Tok::kColT:
                n.kind = Node::kAssertN, n.c = t.k == Tok::kLineT ? kLineIs : kColIs, n.min = t.n, n.max = t.c;
                backtrack = true;
                return node(std::move(n));
            case Tok::kBackrefT:
                if (!closed_[t.c] && !behind_later()) {
                    error = "E65: Illegal back reference";
                    return kNoChar;
                }
                n.kind = Node::kBackrefN, n.c = t.c;
                backtrack = backref = true;
                return node(std::move(n));
            case Tok::kOptional: return optional_seq();
            case Tok::kOpenPlain: return group(0);
            case Tok::kMagic:
                switch (t.c) {
                    case '.': n.kind = Node::kAnyN, n.nl = t.nl; return node(std::move(n));
                    case '<': n.kind = Node::kAssertN, n.c = kBow; return node(std::move(n));
                    case '>': n.kind = Node::kAssertN, n.c = kEow; return node(std::move(n));
                    case '(':
                        if (groups >= 9) {
                            error = "E51: Too many \\(";
                            return kNoChar;
                        }
                        return group(++groups);
                    case '~': {  // the last substitute string, as it stands
                        std::vector<u32> parts;
                        const std::string_view sub = opt_.last_sub;
                        for (size_t k = 0; k < sub.size();) {
                            const Decoded d = decode_utf8(reinterpret_cast<const u8*>(sub.data()) + k, sub.size() - k);
                            parts.push_back(lit(d.cp));
                            k += d.len;
                        }
                        n.kind = parts.empty() ? Node::kEmpty : Node::kCat;
                        n.kids = std::move(parts);
                        return node(std::move(n));
                    }
                    default: break;
                }
                if (error.empty()) error = "E383: Invalid search string";  // (a "\%" not known has said why already)
                return kNoChar;
            default: return kNoChar;
        }
    }

    u32 group(u32 number) {
        if (++depth_ > 200) {
            error = "E: The pattern nests too deeply";
            return kNoChar;
        }
        const u32 inner = alternation();
        --depth_;
        if (!error.empty()) return kNoChar;
        if (!(tok().k == Tok::kMagic && tok_.c == ')')) {
            error = number ? (level_ == 3 ? "E54: Unmatched (" : "E54: Unmatched \\(") : "E53: Unmatched \\%(";
            return kNoChar;
        }
        take();
        if (number) closed_[number] = true;
        Node n;
        n.kind = Node::kGroup;
        n.c = number;
        n.kids = {inner};
        return node(std::move(n));
    }
};

// ---------------------------------------------------------------- the program from the pattern

// What the tree says about every match: can it be empty, and a run of
// plain text it must have (the longest one found).
bool nullable(const std::vector<Node>& ns, u32 at) {
    const Node& n = ns[at];
    switch (n.kind) {
        case Node::kLit:
        case Node::kAnyN:
        case Node::kClassN:
        case Node::kNlN: return false;
        case Node::kCat:
            for (const u32 k : n.kids)
                if (!nullable(ns, k)) return false;
            return true;
        case Node::kAlt:
            for (const u32 k : n.kids)
                if (nullable(ns, k)) return true;
            return false;
        case Node::kRep: return n.min == 0 || nullable(ns, n.kids[0]);
        case Node::kGroup: return nullable(ns, n.kids[0]);
        case Node::kLookN: return n.c != kAtomic || nullable(ns, n.kids[0]);
        default: return true;  // (assertions, \zs, a back-reference: its group may have taken nothing)
    }
}

// The characters a node can take: at least, and at most (kInf: no limit).
// For a look-behind, which then need be tried only that far back.
std::pair<u32, u32> span_of(const std::vector<Node>& ns, u32 at) {
    const Node& n = ns[at];
    auto add = [](u32 a, u32 b) { return a >= kInf - b ? kInf : a + b; };
    auto mul = [](u32 a, u32 b) { return a == 0 || b == 0 ? 0u : a >= kInf / b ? kInf : a * b; };
    switch (n.kind) {
        case Node::kLit:
        case Node::kAnyN:
        case Node::kClassN: return {1, 1};
        case Node::kNlN: return {1, 2};  // ("\r\n")
        case Node::kCat: {
            std::pair<u32, u32> s{0, 0};
            for (const u32 k : n.kids) {
                const auto [lo, hi] = span_of(ns, k);
                s = {add(s.first, lo), add(s.second, hi)};
            }
            return s;
        }
        case Node::kAlt: {
            std::pair<u32, u32> s{kInf, 0};
            for (const u32 k : n.kids) {
                const auto [lo, hi] = span_of(ns, k);
                s = {std::min(s.first, lo), std::max(s.second, hi)};
            }
            return s;
        }
        case Node::kRep: {
            const auto [lo, hi] = span_of(ns, n.kids[0]);
            return {mul(n.min, lo), n.max == kInf ? (hi == 0 ? 0 : kInf) : mul(n.max, hi)};
        }
        case Node::kGroup: return span_of(ns, n.kids[0]);
        case Node::kLookN: return n.c == kAtomic ? span_of(ns, n.kids[0]) : std::pair<u32, u32>{0, 0};
        case Node::kBackrefN: return {0, kInf};
        default: return {0, 0};
    }
}

class Compiler {
public:
    // kPike: for the Pike VM and the DFA. kBacktrack: for the backtracker,
    // with the back-references and look-around. kRelaxed: for the DFA, the
    // same with those left out, so that it finds every line the pattern
    // could match on (a back-reference takes any text on the line, a
    // look-around and a line's number always hold).
    enum Mode : u8 { kPike, kBacktrack, kRelaxed };
    Compiler(const std::vector<Node>& nodes, RegexProgram& p, Mode mode = kPike, bool checks = false) : n_(nodes), p_(p), mode_(mode), checks_(checks) {}
    bool emit(u32 at) {
        if (p_.code.size() > kMostCode) return false;
        const Node& n = n_[at];
        switch (n.kind) {
            case Node::kEmpty: return true;
            case Node::kLit:
                if (p_.icase && cased(n.c)) add({kCharI, 0, fold(n.c), 0});
                else add({kChar, 0, n.c, 0});
                return true;
            case Node::kAnyN: add({kAny, static_cast<u8>(n.nl ? 1 : 0), 0, 0}); return true;
            case Node::kClassN: add({kClass, 0, n.c, 0}); return true;
            case Node::kNlN: add({kNl, 0, 0, 0}); return true;
            case Node::kAssertN:
                if (n.c == kLineIs || n.c == kColIs) {
                    if (mode_ == kBacktrack) add({kAssert, static_cast<u8>(n.c), n.min, n.max});
                    return true;
                }
                add({kAssert, static_cast<u8>(n.c), 0, 0});
                return true;
            case Node::kZsN: add({kSave, 0, 0, 0}); return true;
            case Node::kZeN: add({kSave, 0, 1, 0}); return true;
            case Node::kBackrefN:
                if (mode_ == kBacktrack) {
                    add({kBackref, 0, n.c, 0});
                } else {  // any text on the line: L: split body, out; body: any; jmp L
                    const size_t split = add({kSplit, 0, 0, 0});
                    add({kAny, 0, 0, 0});
                    add({kJmp, 0, static_cast<u32>(split), 0});
                    branch(split, static_cast<u32>(split + 1), static_cast<u32>(p_.code.size()), true);
                }
                return true;
            case Node::kLookN: {
                if (mode_ != kBacktrack) return n.c == kAtomic ? emit(n.kids[0]) : true;
                const auto [least, most] = span_of(n_, n.kids[0]);
                const u32 idx = static_cast<u32>(p_.looks.size());
                p_.looks.push_back({static_cast<u8>(n.c), 0, n.min, least, most});
                add({kLook, 0, idx, 0});
                if (!emit(n.kids[0])) return false;
                add({kLookEnd, 0, 0, 0});
                p_.looks[idx].end = static_cast<u32>(p_.code.size());
                return true;
            }
            case Node::kCat:
                for (const u32 k : n.kids)
                    if (!emit(k)) return false;
                return true;
            case Node::kGroup:
                if (n.c) add({kSave, 0, 2 * n.c, 0});
                if (!emit(n.kids[0])) return false;
                if (n.c) add({kSave, 0, 2 * n.c + 1, 0});
                return true;
            case Node::kAlt: {
                std::vector<size_t> jumps;
                for (size_t k = 0; k < n.kids.size(); ++k) {
                    if (k + 1 < n.kids.size()) {
                        const size_t split = add({kSplit, 0, 0, 0});
                        p_.code[split].x = static_cast<u32>(p_.code.size());
                        if (!emit(n.kids[k])) return false;
                        jumps.push_back(add({kJmp, 0, 0, 0}));
                        p_.code[split].y = static_cast<u32>(p_.code.size());
                    } else if (!emit(n.kids[k])) {
                        return false;
                    }
                }
                for (const size_t j : jumps) p_.code[j].x = static_cast<u32>(p_.code.size());
                return true;
            }
            case Node::kRep: {
                for (u32 k = 0; k < n.min; ++k)
                    if (!emit(n.kids[0])) return false;
                if (n.max == kInf) {  // L: split body, out; body; jmp L
                    const size_t split = add({kSplit, 0, 0, 0});
                    const u32 body = static_cast<u32>(p_.code.size());
                    // (The backtracker would go round a body that took
                    // nothing for ever: a turn must take something. The
                    // Pike VM never comes back to a place in one step, nor
                    // does the backtracker where it remembers the places
                    // it has been: see RegexProgram::memo.)
                    const bool check = mode_ == kBacktrack && checks_ && nullable(n_, n.kids[0]);
                    const u32 mark = check ? p_.slots++ : 0;
                    if (check) add({kMark, 0, mark, 0});
                    if (!emit(n.kids[0])) return false;
                    if (check) add({kCheck, 0, mark, 0});
                    add({kJmp, 0, static_cast<u32>(split), 0});
                    branch(split, body, static_cast<u32>(p_.code.size()), n.greedy);
                    return true;
                }
                // Each one more that may be there: split body, out.
                std::vector<size_t> splits;
                for (u32 k = n.min; k < n.max; ++k) {
                    splits.push_back(add({kSplit, 0, 0, 0}));
                    if (!emit(n.kids[0])) return false;
                }
                const u32 out = static_cast<u32>(p_.code.size());
                for (const size_t s : splits) branch(s, static_cast<u32>(s + 1), out, n.greedy);
                return true;
            }
        }
        return true;
    }

private:
    const std::vector<Node>& n_;
    RegexProgram& p_;
    Mode mode_;
    bool checks_;
    size_t add(Inst i) {
        p_.code.push_back(i);
        return p_.code.size() - 1;
    }
    void branch(size_t split, u32 body, u32 out, bool greedy) {
        p_.code[split].x = greedy ? body : out;
        p_.code[split].y = greedy ? out : body;
    }
};

// The plain text a node always takes in, if all of it is plain (else
// nothing, and *best gets the longest run found inside it).
bool literal_of(const std::vector<Node>& ns, u32 at, bool icase, std::string* whole, std::string* best) {
    const Node& n = ns[at];
    auto keep = [&](const std::string& s) {
        if (s.size() > best->size()) *best = s;
    };
    switch (n.kind) {
        case Node::kLit:
            if (icase && n.c >= 0x80 && cased(n.c)) return false;  // (memchr's way with case is ASCII's)
            put_utf8(*whole, n.c);
            return true;
        case Node::kAssertN:
        case Node::kZsN:
        case Node::kZeN:
        case Node::kEmpty: return true;
        case Node::kLookN:  // (takes nothing, but for \@>: so the text either side of it is one run)
            if (n.c == kAtomic) return literal_of(ns, n.kids[0], icase, whole, best);
            return true;
        case Node::kGroup: return literal_of(ns, n.kids[0], icase, whole, best);
        case Node::kCat: {
            std::string run;
            bool all = true;
            for (const u32 k : n.kids) {
                std::string part;
                const Node& kid = ns[k];
                if (kid.kind == Node::kLookN && (kid.c == kAhead || kid.c == kBehind)) {
                    // What a look-ahead finds goes on from the text before
                    // it; what a look-behind finds runs into the text after
                    // it. (On the match's line either way, unless it takes
                    // a line break, and then nothing here is used.)
                    const bool plain = literal_of(ns, kid.kids[0], icase, &part, best);
                    if (kid.c == kAhead) {
                        if (plain) run += part;
                        keep(run);
                        run.clear();
                    } else {
                        keep(run);
                        run = plain ? part : std::string();
                    }
                    all = false;
                    continue;
                }
                if (literal_of(ns, k, icase, &part, best)) {
                    run += part;
                } else {
                    keep(run);
                    run.clear();
                    all = false;
                    // (A piece repeated at least once still has its own text,
                    // and one repeated an exact number of times starts a run.)
                    std::string inner, inner_best;
                    if (kid.kind == Node::kRep && kid.min >= 1 && literal_of(ns, kid.kids[0], icase, &inner, &inner_best)) {
                        keep(inner);
                        if (kid.min == kid.max)
                            for (u32 r = 0; r < kid.min && r < 64; ++r) run += inner;
                    }
                }
            }
            keep(run);
            if (all) *whole = run;
            return all;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------- the text

struct Ch {
    u32 cp;
    u32 len;
};

// A Buffer, read where it lies, with the piece last read kept to hand.
class BufText {
public:
    explicit BufText(const Buffer& b) : b_(b), size_(b.size()) {}
    u64 size() const { return size_; }
    Ch at(u64 pos) {
        if (pos >= size_) return {kNoChar, 0};
        if (pos < base_ || pos >= base_ + len_) load(pos);
        const u8* q = p_ + (pos - base_);
        if (*q < 0x80) return {*q, 1};
        const u64 left = base_ + len_ - pos;
        Decoded d = decode_utf8(q, static_cast<size_t>(std::min<u64>(left, 4)));
        if (d.cp == 0xFFFD && d.len == 1 && left < 4) {  // it runs on into the next piece
            u8 tmp[4];
            const u64 got = b_.read(pos, reinterpret_cast<char*>(tmp), 4);
            d = decode_utf8(tmp, static_cast<size_t>(got));
        }
        return {d.cp, d.len};
    }
    u8 byte(u64 pos) {
        if (pos >= size_) return 0;
        if (pos < base_ || pos >= base_ + len_) load(pos);
        return p_[pos - base_];
    }
    // The bytes from pos to the end of the piece they are in (the piece
    // kept to hand, so a walk on through the text costs no lookups).
    std::string_view here(u64 pos) {
        if (pos >= size_) return {};
        if (pos < base_ || pos >= base_ + len_) load(pos);
        return {reinterpret_cast<const char*>(p_) + (pos - base_), static_cast<size_t>(len_ - (pos - base_))};
    }
    u32 before(u64 pos) {
        if (pos == 0 || pos > size_) return kNoChar;
        if (pos > base_ && pos <= base_ + len_) {  // (in the piece to hand: the backtracker asks this all the time)
            const u8* q = p_ + (pos - base_);
            if (q[-1] < 0x80) return q[-1];
            if (pos - base_ >= 4) return last_char(q - 4, 4);
        }
        u8 tmp[4];
        const u64 a = pos >= 4 ? pos - 4 : 0;
        const u64 got = b_.read(a, reinterpret_cast<char*>(tmp), pos - a);
        return last_char(tmp, got);
    }
    // Where the character before `pos` starts.
    u64 back(u64 pos) {
        if (pos == 0) return 0;
        if (pos > base_ && pos <= base_ + len_ && (p_[pos - base_ - 1] < 0x80 || pos - base_ >= 4))
            return pos - last_len(p_ + (pos - base_) - std::min<u64>(pos - base_, 4), std::min<u64>(pos - base_, 4));
        u8 tmp[4];
        const u64 a = pos >= 4 ? pos - 4 : 0;
        return pos - last_len(tmp, b_.read(a, reinterpret_cast<char*>(tmp), pos - a));
    }
    template <class Fn>
    void chunks(u64 from, Fn&& fn) {
        u64 base = from;
        for (Buffer::Reader r = b_.reader(from); !r.at_end();) {
            const std::string_view c = r.chunk();
            if (!fn(c, base)) return;
            base += c.size();
            r.advance(static_cast<u32>(c.size()));
        }
    }
    u64 read(u64 off, char* dst, u64 n) { return b_.read(off, dst, n); }
    u64 line_start(u64 pos) { return b_.line_start(b_.line_of(pos)); }
    u64 line_of(u64 pos) { return b_.line_of(std::min(pos, size_)); }

    static u32 last_char(const u8* tmp, u64 got) {
        if (got == 0) return kNoChar;
        u64 k = got - 1;
        while (k > 0 && is_cont(tmp[k])) --k;
        const Decoded d = decode_utf8(tmp + k, static_cast<size_t>(got - k));
        return k + d.len == got ? d.cp : 0xFFFD;
    }
    // The bytes of the last character in tmp (a broken one is a byte, as
    // reading forward takes it).
    static u64 last_len(const u8* tmp, u64 got) {
        if (got == 0) return 0;
        u64 k = got - 1;
        while (k > 0 && is_cont(tmp[k])) --k;
        const Decoded d = decode_utf8(tmp + k, static_cast<size_t>(got - k));
        return k + d.len == got ? got - k : 1;
    }

private:
    void load(u64 pos) {
        if (started_ && pos == base_ + len_) {  // (the next piece: on from the last)
            r_.advance(static_cast<u32>(r_.chunk().size()));
        } else {
            r_ = b_.reader(pos);
            started_ = true;
        }
        const std::string_view c = r_.chunk();
        base_ = pos;
        p_ = reinterpret_cast<const u8*>(c.data());
        len_ = c.size();
    }
    const Buffer& b_;
    u64 size_;
    Buffer::Reader r_{};
    bool started_ = false;
    u64 base_ = 0, len_ = 0;
    const u8* p_ = nullptr;
};

class StrText {
public:
    explicit StrText(std::string_view s) : s_(s) {}
    u64 size() const { return s_.size(); }
    Ch at(u64 pos) const {
        if (pos >= s_.size()) return {kNoChar, 0};
        const u8* q = reinterpret_cast<const u8*>(s_.data()) + pos;
        if (*q < 0x80) return {*q, 1};
        const Decoded d = decode_utf8(q, static_cast<size_t>(s_.size() - pos));
        return {d.cp, d.len};
    }
    u8 byte(u64 pos) const { return pos < s_.size() ? static_cast<u8>(s_[pos]) : 0; }
    std::string_view here(u64 pos) const { return pos < s_.size() ? s_.substr(pos) : std::string_view(); }
    u32 before(u64 pos) const {
        if (pos == 0 || pos > s_.size()) return kNoChar;
        const u64 a = pos >= 4 ? pos - 4 : 0;
        return BufText::last_char(reinterpret_cast<const u8*>(s_.data()) + a, pos - a);
    }
    u64 back(u64 pos) const {
        if (pos == 0) return 0;
        const u64 a = pos >= 4 ? pos - 4 : 0;
        return pos - BufText::last_len(reinterpret_cast<const u8*>(s_.data()) + a, pos - a);
    }
    u64 line_of(u64 pos) const { return static_cast<u64>(std::count(s_.begin(), s_.begin() + static_cast<std::ptrdiff_t>(std::min<u64>(pos, s_.size())), '\n')); }
    template <class Fn>
    void chunks(u64 from, Fn&& fn) const {
        if (from < s_.size()) fn(s_.substr(from), from);
    }
    u64 read(u64 off, char* dst, u64 n) const {
        if (off >= s_.size()) return 0;
        n = std::min<u64>(n, s_.size() - off);
        std::memcpy(dst, s_.data() + off, n);
        return n;
    }
    u64 line_start(u64 pos) const {
        const size_t nl = pos == 0 ? std::string_view::npos : s_.rfind('\n', pos - 1);
        return nl == std::string_view::npos ? 0 : nl + 1;
    }

private:
    std::string_view s_;
};

// The first byte at or after `from` that the table has, before `to`; `to` if none.
template <class Text>
u64 skip_to(Text& t, u64 from, u64 to, const bool* table) {
    u64 found = to;
    t.chunks(from, [&](std::string_view c, u64 base) {
        const u64 n = std::min<u64>(c.size(), to > base ? to - base : 0);
        const u8* p = reinterpret_cast<const u8*>(c.data());
        for (u64 i = 0; i < n; ++i)
            if (table[p[i]]) {
                found = base + i;
                return false;
            }
        return base + c.size() < to;
    });
    return found;
}

// Where the line that `pos` is on ends (its "\n"), or the text's end.
template <class Text>
u64 line_end(Text& t, u64 pos) {
    u64 found = t.size();
    t.chunks(pos, [&](std::string_view c, u64 base) {
        if (const void* q = std::memchr(c.data(), '\n', c.size())) {
            found = base + static_cast<u64>(static_cast<const char*>(q) - c.data());
            return false;
        }
        return true;
    });
    return found;
}

// The first place `lit` is at or after `from`, starting before `to`; kNone if nowhere.
template <class Text>
u64 find_literal(Text& t, std::string_view lit, bool icase, u64 from, u64 to) {
    const u64 len = lit.size(), size = t.size();
    if (len == 0 || size < len || from >= to) return kNone;
    to = std::min(to, size - len + 1);
    if (from >= to) return kNone;
    u64 found = kNone;
    std::string seam;
    t.chunks(from, [&](std::string_view c, u64 cs) {
        const u64 ce = cs + c.size();
        if (c.size() >= len) {
            const size_t stop = static_cast<size_t>(std::min(to, ce - len + 1) - cs);
            const size_t i = find_text(c, lit, icase, 0, stop);
            if (i != std::string_view::npos) {
                found = cs + i;
                return false;
            }
        }
        // One that starts in this piece and ends in a later one.
        const u64 s0 = std::max(cs, ce + 1 >= len ? ce + 1 - len : 0), s1 = std::min(to, ce);
        if (len > 1 && s0 < s1) {
            seam.resize(static_cast<size_t>(std::min(size - s0, s1 - s0 + len - 1)));
            t.read(s0, seam.data(), seam.size());
            const size_t i = find_text(seam, lit, icase, 0, static_cast<size_t>(s1 - s0));
            if (i != std::string_view::npos) {
                found = s0 + i;
                return false;
            }
        }
        return ce < to;
    });
    return found;
}

}  // namespace

// ---------------------------------------------------------------- running it

struct VimRegex::Searcher::State {
    // What the Pike VM and the DFA run: the pattern's program, or for one
    // the backtracker runs, the relaxed one that finds its lines.
    const RegexProgram& p;
    explicit State(const RegexProgram& prog) : p(prog.backtrack && prog.relaxed ? *prog.relaxed : prog) {
        bt = prog.backtrack ? &prog : nullptr;
        const size_t n = p.code.size();
        for (List* l : {&a, &b}) {
            l->sparse.assign(n, 0);
            l->pcs.assign(n, 0);
            l->caps.assign(n * p.slots, 0);
        }
        work.assign(p.slots, 0);
        mark.assign(n, 0);
        if (bt) held.assign(bt->slots, RegexMatch::kUnset);
    }

    // The VM's threads at one place in the text: each a place in the
    // program, in the order they come (the first is preferred), with what
    // its groups have caught so far.
    struct List {
        std::vector<u32> sparse, pcs;
        std::vector<u64> caps;
        u32 n = 0;
        bool has(u32 pc) const {
            const u32 i = sparse[pc];
            return i < n && pcs[i] == pc;
        }
        u32 add(u32 pc) {
            sparse[pc] = n;
            pcs[n] = pc;
            return n++;
        }
    };
    List a, b;
    std::vector<u64> work;
    struct Step {
        u32 pc;
        u32 slot;  // kNoChar: go on at pc; else put `value` back in that slot
        u64 value;
    };
    std::vector<Step> stack;

    // Where a thread is: the character before, the one there, the one after.
    struct Ctx {
        u32 prev, cur, next;
        u64 pos;
    };
    static bool holds(u8 which, const Ctx& c) {
        switch (which) {
            case kBol: return c.prev == '\n' || c.prev == kNoChar;
            case kEol: return c.cur == '\n' || c.cur == kNoChar || (c.cur == '\r' && c.next == '\n');
            case kBow: return !word_char(c.prev) && word_char(c.cur);
            case kEow: return word_char(c.prev) && !word_char(c.cur);
            case kBof: return c.pos == 0;
            case kEof: return c.cur == kNoChar;
            default: return false;
        }
    }

    // A thread at pc, and every place it reaches without taking a character.
    void add(List& l, u32 pc0, const u64* caps, const Ctx& ctx) {
        std::memcpy(work.data(), caps, p.slots * sizeof(u64));
        stack.clear();
        stack.push_back({pc0, kNoChar, 0});
        while (!stack.empty()) {
            const Step e = stack.back();
            stack.pop_back();
            if (e.slot != kNoChar) {
                work[e.slot] = e.value;
                continue;
            }
            u32 pc = e.pc;
            for (;;) {
                if (l.has(pc)) break;
                const u32 idx = l.add(pc);
                const Inst& in = p.code[pc];
                if (in.op == kJmp) {
                    pc = in.x;
                    continue;
                }
                if (in.op == kSplit) {
                    stack.push_back({in.y, kNoChar, 0});
                    pc = in.x;
                    continue;
                }
                if (in.op == kSave) {
                    stack.push_back({0, in.x, work[in.x]});
                    work[in.x] = ctx.pos;
                    ++pc;
                    continue;
                }
                if (in.op == kAssert) {
                    if (!holds(in.a, ctx)) break;
                    ++pc;
                    continue;
                }
                std::memcpy(&l.caps[static_cast<size_t>(idx) * p.slots], work.data(), p.slots * sizeof(u64));
                break;
            }
        }
    }

    // Does the instruction take the character? (A "\r" just before a "\n"
    // is part of the line's end: only "\n" and a "\r" asked for by name take it.)
    bool takes(const Inst& in, u32 cur, u32 next) const {
        const bool crlf = cur == '\r' && next == '\n';
        switch (in.op) {
            case kChar: return cur == in.x;
            case kCharI: return cur != kNoChar && fold(cur) == in.x;
            case kAny: return cur != kNoChar && ((cur != '\n' && !crlf) || in.a);
            case kClass: {
                const CharClass& c = p.classes[in.x];
                if (crlf) return c.cr || c.newline;
                return cur != kNoChar && c.test(cur, p.icase);
            }
            default: return false;
        }
    }

    // The leftmost match whose start is in [from, to), the one the program
    // prefers of those that start there.
    template <class Text>
    bool vm(Text& t, u64 from, u64 to, RegexMatch* m) {
        const u64 size = t.size();
        to = std::min(to, size + 1);
        if (from >= to) return false;
        u64 pos = from;
        u32 prev = t.before(from);
        Ch cur = t.at(pos), nxt = cur.len ? t.at(pos + cur.len) : Ch{kNoChar, 0};
        List* cl = &a;
        List* nl = &b;
        cl->n = nl->n = 0;
        bool matched = false;
        const u32 slots = p.slots, anchor = p.anchor_slot();
        std::vector<u64>& fresh = start_caps;
        fresh.assign(slots, RegexMatch::kUnset);
        for (;;) {
            if (!matched && pos < to) {
                if (cl->n == 0 && p.skip) {
                    // Nothing under way: on to the next place a match can
                    // start. (Such a pattern never matches nothing, so
                    // where no character can start one there is none.)
                    const u64 end = std::min(to, size);
                    const u64 at = pos < end ? skip_to(t, pos, end, p.first) : end;
                    if (at >= end) break;
                    if (at != pos) {
                        pos = at;
                        prev = t.before(pos);
                        cur = t.at(pos);
                        nxt = t.at(pos + cur.len);
                    }
                }
                fresh[0] = pos;
                fresh[anchor] = pos;
                add(*cl, 0, fresh.data(), {prev, cur.cp, nxt.cp, pos});
            }
            if (cl->n == 0) break;
            nl->n = 0;
            const u64 at_next = pos + cur.len;
            u32 nxt2 = kNoChar;
            if (nxt.cp == '\r') nxt2 = t.at(at_next + nxt.len).cp;
            for (u32 i = 0; i < cl->n; ++i) {
                const u32 pc = cl->pcs[i];
                const Inst& in = p.code[pc];
                const u64* caps = &cl->caps[static_cast<size_t>(i) * slots];
                if (in.op == kMatch) {
                    m->anchor = caps[anchor];
                    m->start = caps[0];
                    m->end = caps[1] == RegexMatch::kUnset ? pos : caps[1];
                    if (m->end < m->start) m->end = m->start;
                    m->group[0][0] = m->start;
                    m->group[0][1] = m->end;
                    for (u32 g = 1; g < 10; ++g) {
                        const bool set = g <= p.groups && caps[2 * g] != RegexMatch::kUnset && caps[2 * g + 1] != RegexMatch::kUnset;
                        m->group[g][0] = set ? caps[2 * g] : RegexMatch::kUnset;
                        m->group[g][1] = set ? caps[2 * g + 1] : RegexMatch::kUnset;
                    }
                    matched = true;
                    break;  // (the threads after it are less preferred)
                }
                if (in.op == kNl) {
                    if (cur.cp == '\n') add(*nl, pc + 1, caps, {cur.cp, nxt.cp, nxt2, at_next});
                    else if (cur.cp == '\r' && nxt.cp == '\n') add(*nl, pc, caps, {cur.cp, nxt.cp, nxt2, at_next});
                    continue;
                }
                if (in.op <= kClass && takes(in, cur.cp, nxt.cp)) add(*nl, pc + 1, caps, {cur.cp, nxt.cp, nxt2, at_next});
            }
            if (cur.cp == kNoChar) break;
            std::swap(cl, nl);
            prev = cur.cp;
            pos = at_next;
            cur = nxt;
            nxt = cur.len ? t.at(pos + cur.len) : Ch{kNoChar, 0};
        }
        return matched;
    }
    std::vector<u64> start_caps;

    // ------------------------------------------------------------ the DFA
    //
    // For a pattern that stays on one line: which lines have a match. A
    // state is the set of the program's places the VM's threads would be
    // at, before the steps that take no character are followed (those
    // depend on the character to come: "$", "\>"), and what the character
    // before was.
    enum Prev : u8 { kPrevBof, kPrevBol, kPrevWord, kPrevOther };
    enum Next : u8 { kNextEol, kNextEof, kNextWord, kNextOther };
    static constexpr u32 kUnknown = 0xFFFFFFFFu, kHit = 0x80000000u;
    static constexpr size_t kMostStates = 3000;
    std::unordered_map<std::string, u32> ids;
    std::vector<std::vector<u32>> sets;
    std::vector<u8> prevs;
    std::vector<u32> table;     // a state's 128 ASCII next states, kHit if a match ends before that character
    std::vector<u8> at_end;     // a match ending at the line's end: 0 not known, 1 no, 2 yes
    std::unordered_map<u64, u32> wide;  // the same past ASCII, by state and character
    bool dfa_failed = false;    // too many states: the VM alone, then
    std::vector<u32> mark;
    u32 gen = 0;
    std::vector<u32> todo, took;
    std::string key;

    u32 state(const std::vector<u32>& pcs, u8 prev) {
        key.assign(1, static_cast<char>(prev));
        key.append(reinterpret_cast<const char*>(pcs.data()), pcs.size() * sizeof(u32));
        if (const auto it = ids.find(key); it != ids.end()) return it->second;
        if (sets.size() >= kMostStates) {
            dfa_failed = true;
            return 0;
        }
        const u32 id = static_cast<u32>(sets.size());
        ids.emplace(key, id);
        sets.push_back(pcs);
        prevs.push_back(prev);
        table.resize(table.size() + 128, kUnknown);
        at_end.push_back(0);
        return id;
    }

    // The places reached from a state's (and from a new thread's) without
    // taking a character, the character to come being of kind `next`:
    // those that take one go in `took`; true if the match is among them.
    bool close(u32 s, u8 next) {
        if (++gen == 0) {
            std::fill(mark.begin(), mark.end(), 0);
            gen = 1;
        }
        const u8 prev = prevs[s];
        bool hit = false;
        took.clear();
        todo.clear();
        todo.push_back(0);
        for (const u32 pc : sets[s]) todo.push_back(pc);
        while (!todo.empty()) {
            u32 pc = todo.back();
            todo.pop_back();
            for (;;) {
                if (mark[pc] == gen) break;
                mark[pc] = gen;
                const Inst& in = p.code[pc];
                if (in.op == kJmp) {
                    pc = in.x;
                    continue;
                }
                if (in.op == kSplit) {
                    todo.push_back(in.y);
                    pc = in.x;
                    continue;
                }
                if (in.op == kSave) {
                    ++pc;
                    continue;
                }
                if (in.op == kAssert) {
                    bool ok = false;
                    switch (in.a) {
                        case kBol: ok = prev == kPrevBof || prev == kPrevBol; break;
                        case kEol: ok = next == kNextEol || next == kNextEof; break;
                        case kBow: ok = prev != kPrevWord && next == kNextWord; break;
                        case kEow: ok = prev == kPrevWord && next != kNextWord; break;
                        case kBof: ok = prev == kPrevBof; break;
                        case kEof: ok = next == kNextEof; break;
                        default: break;
                    }
                    if (!ok) break;
                    ++pc;
                    continue;
                }
                if (in.op == kMatch) hit = true;
                else took.push_back(pc);
                break;
            }
        }
        return hit;
    }

    u32 step(u32 s, u32 cp) {
        const bool w = word_char(cp);
        const bool hit = close(s, w ? kNextWord : kNextOther);
        std::vector<u32> next;
        for (const u32 pc : took)
            if (takes(p.code[pc], cp, 0)) next.push_back(pc + 1);
        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        const u32 to = state(next, w ? kPrevWord : kPrevOther);
        return to | (hit ? kHit : 0);
    }

    bool ends(u32 s, bool eof) {
        if (!eof && at_end[s]) return at_end[s] == 2;
        const bool hit = close(s, eof ? kNextEof : kNextEol);
        if (!eof) at_end[s] = hit ? 2 : 1;
        return hit;
    }

    u8 prev_kind(u32 cp) const { return cp == kNoChar ? kPrevBof : cp == '\n' ? kPrevBol : word_char(cp) ? kPrevWord : kPrevOther; }

    // The start of the first line, from `from` on (and starting before
    // `to`), that has a match starting at or after `from`: kNone if none,
    // `from` itself for the first line. *gave_up: the DFA grew too large.
    // (Inlined where it is used: as a function of its own, called from the
    // backtracker's search as well, it ran 2.5% slower in a string.)
    template <class Text>
    [[gnu::always_inline]] u64 dfa_line(Text& t, u64 from, u64 to, bool* gave_up) {
        *gave_up = false;
        const std::vector<u32> none;
        u32 s = state(none, prev_kind(t.before(from)));
        const u32 bol = state(none, kPrevBol);
        if (dfa_failed) return *gave_up = true, kNone;
        u64 ls = from, result = kNone;
        bool ended = false;  // the line's end was a "\r\n", seen at its "\r"
        bool stop = false;
        u32 skip = 0;        // bytes at the start of the next piece that end a character begun in the last
        // A character past ASCII (or a "\r" alone): false to stop (a match, or too many states).
        auto wide_step = [&](u32 cp) {
            const u64 k = (static_cast<u64>(s) << 32) | cp;
            u32 to_s;
            if (const auto it = wide.find(k); it != wide.end()) {
                to_s = it->second;
            } else {
                to_s = step(s, cp);
                if (dfa_failed) return false;
                wide.emplace(k, to_s);
            }
            if (to_s & kHit) {
                result = ls;
                return false;
            }
            s = to_s;
            return true;
        };
        t.chunks(from, [&](std::string_view c, u64 base0) {
            const u32 k0 = std::min<u32>(skip, static_cast<u32>(c.size()));
            skip -= k0;
            const u8* q = reinterpret_cast<const u8*>(c.data()) + k0;
            const size_t n = c.size() - k0;
            const u64 base = base0 + k0;
            for (size_t i = 0; i < n; ++i) {
                const u8 ch = q[i];
                if (ch < 0x80 && ch != '\n' && ch != '\r') {
                    if (ended) continue;
                    u32 to_s = table[static_cast<size_t>(s) * 128 + ch];
                    if (to_s == kUnknown) {
                        to_s = step(s, ch);  // (which may grow the table)
                        if (dfa_failed) return stop = true, false;
                        table[static_cast<size_t>(s) * 128 + ch] = to_s;
                    }
                    if (to_s & kHit) {
                        result = ls;
                        return stop = true, false;
                    }
                    s = to_s;
                    continue;
                }
                const u64 at = base + i;
                if (ch == '\n') {
                    if (!ended && ends(s, false)) {
                        result = ls;
                        return stop = true, false;
                    }
                    ended = false;
                    ls = at + 1;
                    if (ls >= to) return stop = true, false;
                    s = bol;
                    continue;
                }
                if (ended) continue;
                if (ch == '\r') {
                    const u8 after = i + 1 < n ? q[i + 1] : t.byte(at + 1);
                    if (after == '\n') {
                        if (ends(s, false)) {
                            result = ls;
                            return stop = true, false;
                        }
                        ended = true;
                        continue;
                    }
                    if (!wide_step('\r')) return stop = true, false;
                    continue;
                }
                Decoded d = decode_utf8(q + i, n - i);
                if (d.cp == 0xFFFD && d.len == 1 && n - i < 4) {  // (perhaps one that runs on into the next piece)
                    u8 tmp[4];
                    const u64 got = t.read(at, reinterpret_cast<char*>(tmp), 4);
                    d = decode_utf8(tmp, static_cast<size_t>(got));
                }
                if (!wide_step(d.cp)) return stop = true, false;
                if (d.len > n - i) {
                    skip = d.len - static_cast<u32>(n - i);
                    return true;
                }
                i += d.len - 1;
            }
            return true;
        });
        if (dfa_failed) return *gave_up = true, kNone;
        if (stop) return result;
        // The end of the text ends the last line.
        if (!ended && ls < to && ends(s, true)) return ls;
        return kNone;
    }

    // ------------------------------------------------------------ the backtracker
    //
    // For a pattern with back-references or look-around, which no automaton
    // can follow: one way at a time, in the order the program prefers (so
    // the match is the one the Pike VM would give), going back to the last
    // choice when a way fails. That can take time exponential in the text,
    // so steps are counted: past a budget the search stops (overran), and
    // whoever asked says so.
    const RegexProgram* bt = nullptr;  // the program for the backtracker, if it is one
    static constexpr u64 kBudget = 1ull << 25;  // steps a search may take, whatever it covers (some 0.1 s)
    static constexpr u64 kBudgetPerByte = 64;   // and more for each byte its starts go over
    struct Choice {
        u32 pc;
        u32 slot;  // kNoChar: a way not yet tried, from pc at `v`; else put `v` back in that slot
        u64 v;
    };
    std::vector<Choice> trail;
    std::vector<u64> held;  // the slots: \zs, \ze, the groups, where it began, the marks
    u64 steps = 0, budget = 0;
    bool overran = false;
    // The line the backtracker last asked the number of: [start, end) and its number.
    u64 line_lo = kNone, line_hi = 0, line_no = 0;

    // The places tried (RegexProgram::memo): a table by kSplit and
    // position, each with the number its scope had then. A scope gets a new
    // number when what was found in it no longer holds (a new search; a
    // look-behind asked to end somewhere else; a look-around that held,
    // whose way through is marked too), which forgets its places at once.
    std::vector<u64> memo_keys;
    std::vector<u32> memo_marks;
    size_t memo_used = 0;
    std::vector<u32> scope_mark;
    u32 mark_next = 0;
    static constexpr u64 kMemoEmpty = ~0ull;
    static constexpr size_t kMemoMost = 1 << 20;  // (past that the table starts again: more work, never a wrong answer)

    void forget_scope(u32 scope) {
        if (++mark_next == 0) {  // (once in four billion: every mark goes)
            std::fill(memo_keys.begin(), memo_keys.end(), kMemoEmpty);
            memo_used = 0;
            mark_next = 1;
        }
        scope_mark[scope] = mark_next;
    }
    void forget_all() {
        scope_mark.resize(bt->looks.size() + 1);
        for (u32 k = 0; k < scope_mark.size(); ++k) forget_scope(k);
    }
    // False if the kSplit at pc has been tried at pos already (in its scope as it is now); else it is now.
    bool first_visit(u32 pc, u64 pos) {
        const u32 split = bt->split_of[pc];
        const u32 mark_now = scope_mark[bt->scope_of[split]];
        if (memo_used * 2 >= memo_keys.size()) {
            const size_t grown = memo_keys.empty() ? 1024 : memo_keys.size() * 2;
            std::vector<u64> keys(std::min(grown, kMemoMost), kMemoEmpty);
            std::vector<u32> marks(keys.size(), 0);
            size_t used = 0;
            if (grown <= kMemoMost)
                for (size_t i = 0; i < memo_keys.size(); ++i) {
                    if (memo_keys[i] == kMemoEmpty) continue;
                    size_t at = static_cast<size_t>((memo_keys[i] * 0x9E3779B97F4A7C15ull) >> 40) & (keys.size() - 1);
                    while (keys[at] != kMemoEmpty) at = (at + 1) & (keys.size() - 1);
                    keys[at] = memo_keys[i], marks[at] = memo_marks[i];
                    ++used;
                }
            memo_keys = std::move(keys);
            memo_marks = std::move(marks);
            memo_used = used;
        }
        const u64 k = (pos << 18) | split;  // (a program has fewer than 1 << 18 instructions: see kMostCode)
        const size_t mask = memo_keys.size() - 1;
        for (size_t at = static_cast<size_t>((k * 0x9E3779B97F4A7C15ull) >> 40) & mask;; at = (at + 1) & mask) {
            if (memo_keys[at] == k) {
                if (memo_marks[at] == mark_now) return false;
                memo_marks[at] = mark_now;
                return true;
            }
            if (memo_keys[at] == kMemoEmpty) {
                memo_keys[at] = k;
                memo_marks[at] = mark_now;
                ++memo_used;
                return true;
            }
        }
    }

    static bool takes_in(const RegexProgram& q, const Inst& in, u32 cur, u32 next) {
        const bool crlf = cur == '\r' && next == '\n';
        switch (in.op) {
            case kChar: return cur == in.x;
            case kCharI: return cur != kNoChar && fold(cur) == in.x;
            case kAny: return cur != kNoChar && ((cur != '\n' && !crlf) || in.a);
            case kClass: {
                const CharClass& c = q.classes[in.x];
                if (crlf) return c.cr || c.newline;
                return cur != kNoChar && c.test(cur, q.icase);
            }
            default: return false;
        }
    }

    // The line `pos` is on (from 0), and where it starts. Asked mostly of
    // the line the last answer was about, or the next.
    template <class Text>
    u64 line_at(Text& t, u64 pos, u64* start) {
        if (line_lo == kNone || pos < line_lo) {
            line_no = t.line_of(pos);
            line_lo = t.line_start(pos);
            line_hi = line_end(t, line_lo) + 1;
        } else if (pos >= line_hi) {  // (on from the line known: the line breaks after it counted)
            u64 n = 0;
            t.chunks(line_hi, [&](std::string_view c, u64 base) {
                n += static_cast<u64>(std::count(c.data(), c.data() + std::min<u64>(c.size(), pos - base), '\n'));
                return base + c.size() < pos;
            });
            line_no += 1 + n;
            line_lo = t.line_start(pos);
            line_hi = line_end(t, line_lo) + 1;
        }
        *start = line_lo;
        return line_no;
    }

    template <class Text>
    bool holds_at(Text& t, const Inst& in, u64 pos) {
        switch (in.a) {
            case kBol: {
                const u32 prev = t.before(pos);
                return prev == '\n' || prev == kNoChar;
            }
            case kEol: {
                const u32 cur = t.at(pos).cp;
                return cur == '\n' || cur == kNoChar || (cur == '\r' && t.at(pos + 1).cp == '\n');
            }
            case kBow: return !word_char(t.before(pos)) && word_char(t.at(pos).cp);
            case kEow: return word_char(t.before(pos)) && !word_char(t.at(pos).cp);
            case kBof: return pos == 0;
            case kEof: return pos >= t.size();
            case kLineIs:
            case kColIs: {
                u64 start;
                const u64 line = line_at(t, pos, &start);
                const u64 v = (in.a == kLineIs ? line : pos - start) + 1;  // (Vim counts both from 1; a column in bytes)
                return in.y == kAt ? v == in.x : in.y == kBefore ? v < in.x : v > in.x;
            }
            default: return false;
        }
    }

    // Does the text at `pos` say again what [lo, hi) says? (In either case
    // if the case is ignored.) *len: how much it took.
    template <class Text>
    bool same_text(Text& t, u64 lo, u64 hi, u64 pos, u64* len) {
        const u64 size = t.size();
        if (!bt->icase) {
            if (pos + (hi - lo) > size) return false;
            for (u64 k = 0; k < hi - lo; ++k)
                if (t.byte(lo + k) != t.byte(pos + k)) return false;
            *len = hi - lo;
            return true;
        }
        u64 i = lo, j = pos;
        while (i < hi) {
            const Ch x = t.at(i), y = t.at(j);
            if (y.cp == kNoChar || fold(x.cp) != fold(y.cp)) return false;
            i += x.len, j += y.len;
        }
        *len = j - pos;
        return true;
    }

    // Puts back what the ways tried since `base` changed, and forgets them.
    void undo_to(size_t base) {
        while (trail.size() > base) {
            const Choice c = trail.back();
            trail.pop_back();
            if (c.slot != kNoChar) held[c.slot] = c.v;
        }
    }
    // A look-around that held is not gone back into: the ways not tried in
    // it are forgotten, what it caught stays (to be put back if the way it
    // is on fails).
    void keep_caught(size_t base) {
        size_t k = base;
        for (size_t i = base; i < trail.size(); ++i)
            if (trail[i].slot != kNoChar) trail[k++] = trail[i];
        trail.resize(k);
    }

    // Runs the program from pc at pos to its kMatch, or (a look-around's)
    // to the kLookEnd, which must then be at `target` unless that is kNone.
    // *out: where it ended. The ways not tried stay on the trail.
    template <class Text>
    bool run(Text& t, u32 pc, u64 pos, u64 target, u64* out) {
        const RegexProgram& q = *bt;
        const size_t base = trail.size();
        for (;;) {
            if (++steps > budget) {
                overran = true;
                return false;
            }
            const Inst& in = q.code[pc];
            switch (in.op) {
                case kChar:
                case kCharI:
                case kAny:
                case kClass: {
                    const Ch c = t.at(pos);
                    if (!takes_in(q, in, c.cp, c.cp == '\r' ? t.at(pos + 1).cp : kNoChar)) goto fail;
                    pos += c.len;
                    ++pc;
                    continue;
                }
                case kNl: {
                    const u32 c = t.at(pos).cp;
                    if (c == '\n') pos += 1;
                    else if (c == '\r' && t.at(pos + 1).cp == '\n') pos += 2;
                    else goto fail;
                    ++pc;
                    continue;
                }
                case kSplit:
                    if (q.memo && !first_visit(pc, pos)) goto fail;
                    trail.push_back({in.y, kNoChar, pos});
                    pc = in.x;
                    continue;
                case kJmp: pc = in.x; continue;
                case kSave:
                case kMark:
                    trail.push_back({0, in.x, held[in.x]});
                    held[in.x] = pos;
                    ++pc;
                    continue;
                case kCheck:
                    if (held[in.x] == pos) goto fail;
                    ++pc;
                    continue;
                case kAssert:
                    if (!holds_at(t, in, pos)) goto fail;
                    ++pc;
                    continue;
                case kBackref: {
                    const u64 lo = held[2 * in.x], hi = held[2 * in.x + 1];
                    if (lo != RegexMatch::kUnset && hi != RegexMatch::kUnset && hi > lo) {  // (a group that took nothing, or none yet: nothing)
                        u64 len = 0;
                        if (!same_text(t, lo, hi, pos, &len)) goto fail;
                        pos += len;
                    }
                    ++pc;
                    continue;
                }
                case kLook: {
                    const Look& lk = q.looks[in.x];
                    const size_t at_look = trail.size();
                    u64 end = pos;
                    const bool back = lk.kind == kBehind || lk.kind == kNotBehind;
                    if (q.memo && back) forget_scope(in.x + 1);
                    const bool hit = back ? behind(t, pc + 1, pos, lk, &end) : run(t, pc + 1, pos, kNone, &end);
                    if (overran) return false;
                    if (q.memo && hit) forget_scope(in.x + 1);
                    const bool want = lk.kind == kAhead || lk.kind == kBehind || lk.kind == kAtomic;
                    if (hit != want) {
                        undo_to(at_look);
                        goto fail;
                    }
                    keep_caught(at_look);
                    if (lk.kind == kAtomic) pos = end;
                    pc = lk.end;
                    continue;
                }
                case kLookEnd:
                    if (target != kNone && pos != target) goto fail;
                    *out = pos;
                    return true;
                case kMatch: *out = pos; return true;
            }
        fail:
            for (;;) {
                if (trail.size() == base) return false;
                const Choice c = trail.back();
                trail.pop_back();
                if (c.slot != kNoChar) {
                    held[c.slot] = c.v;
                    continue;
                }
                pc = c.pc;
                pos = c.v;
                break;
            }
        }
    }

    // A look-behind: its program from each place before `pos`, nearest
    // first, to end at `pos`. As in Vim, no further back than the start of
    // the line before, nor than its byte limit ("\@12<="; past a line's
    // start, counted from that line's end), nor than it can take
    // characters.
    template <class Text>
    bool behind(Text& t, u32 pc, u64 pos, const Look& lk, u64* out) {
        u64 s = pos, from = pos;
        u32 chars = 0, lines = 0;
        for (;;) {
            if (chars >= lk.least) {
                if (run(t, pc, s, pos, out)) return true;
                if (overran) return false;
            }
            if (chars >= lk.most || s == 0) return false;
            const u64 was = t.back(s);
            if (t.byte(was) == '\n') {
                if (++lines == 2) return false;
                from = was;
            }
            if (lk.bytes && from - was > lk.bytes) return false;
            s = was;
            ++chars;
            if (++steps > budget) {
                overran = true;
                return false;
            }
        }
    }

    // These walk on through the piece kept to hand (Text::here), where
    // skip_to() and line_end() start a new walk down the Buffer's tree: the
    // backtracker asks them at every start.
    template <class Text>
    static u64 skip_near(Text& t, u64 pos, u64 end, const bool* table) {
        for (std::string_view c; pos < end && !(c = t.here(pos)).empty();) {
            const u8* q = reinterpret_cast<const u8*>(c.data());
            const size_t n = static_cast<size_t>(std::min<u64>(c.size(), end - pos));
            for (size_t i = 0; i < n; ++i)
                if (table[q[i]]) return pos + i;
            pos += n;
        }
        return end;
    }
    template <class Text>
    static u64 line_end_near(Text& t, u64 pos) {
        for (std::string_view c; !(c = t.here(pos)).empty(); pos += c.size())
            if (const void* q = std::memchr(c.data(), '\n', c.size())) return pos + static_cast<u64>(static_cast<const char*>(q) - c.data());
        return t.size();
    }
    // The first place at or after pos where a line starts (the text's
    // size + 1 if none), and where a word does.
    template <class Text>
    static u64 line_start_from(Text& t, u64 pos) {
        if (pos == 0 || t.before(pos) == '\n') return pos;
        const u64 e = line_end_near(t, pos);
        return e < t.size() ? e + 1 : t.size() + 1;
    }
    template <class Text>
    static u64 word_start_from(Text& t, u64 pos) {
        bool was_word = word_char(t.before(pos));
        for (std::string_view c; !(c = t.here(pos)).empty();) {
            const u8* q = reinterpret_cast<const u8*>(c.data());
            size_t i = 0;
            while (i < c.size()) {
                u32 cp = q[i], len = 1;
                if (cp >= 0x80) {  // (one that runs on into the next piece is read whole)
                    const Ch ch = t.at(pos + i);
                    cp = ch.cp, len = ch.len;
                }
                const bool w = word_char(cp);
                if (w && !was_word) return pos + i;
                was_word = w;
                i += len;
            }
            pos += i;
        }
        return t.size() + 1;
    }

    // The first match whose start is in [from, to): each start in turn.
    template <class Text>
    bool backtrack(Text& t, u64 from, u64 to, RegexMatch* m) {
        const RegexProgram& q = *bt;
        const u64 size = t.size();
        to = std::min(to, size + 1);
        // (A try that fails puts back every slot it set: they are cleared once.)
        std::fill(held.begin(), held.end(), RegexMatch::kUnset);
        for (u64 pos = from; pos < to;) {
            if (q.skip) {  // (to the next place a match can start: such a pattern never matches nothing)
                const u64 end = std::min(to, size);
                const u64 at = pos < end ? skip_near(t, pos, end, q.first) : end;
                if (at >= end) return false;
                budget += kBudgetPerByte * (at - pos);
                pos = at;
            }
            if (q.lead != RegexProgram::kLeadAny) {
                const u64 at = q.lead == RegexProgram::kLeadLine ? line_start_from(t, pos) : word_start_from(t, pos);
                if (at >= to) return false;
                budget += kBudgetPerByte * (at - pos);
                pos = at;
            }
            held[0] = held[q.anchor] = pos;
            trail.clear();
            u64 end = pos;
            if (run(t, 0, pos, kNone, &end)) {
                m->anchor = held[q.anchor];
                m->start = held[0];
                m->end = held[1] == RegexMatch::kUnset ? end : held[1];
                if (m->end < m->start) m->end = m->start;
                m->group[0][0] = m->start;
                m->group[0][1] = m->end;
                for (u32 g = 1; g < 10; ++g) {
                    const bool set = g <= q.groups && held[2 * g] != RegexMatch::kUnset && held[2 * g + 1] != RegexMatch::kUnset;
                    m->group[g][0] = set ? held[2 * g] : RegexMatch::kUnset;
                    m->group[g][1] = set ? held[2 * g + 1] : RegexMatch::kUnset;
                }
                return true;
            }
            if (overran) return false;
            const u32 len = t.at(pos).len;
            if (len == 0) return false;  // (the end of the text)
            pos += len;
            budget += kBudgetPerByte * len;
        }
        return false;
    }
};

namespace {

// The same for a pattern the backtracker runs: the lines are found as
// before, by the plain text every match has or else by the DFA (on the
// relaxed program), and the backtracker tried only on them.
template <class Text>
[[gnu::noinline]] bool search_bt(VimRegex::Searcher::State& st, Text& t, u64 from, u64 to, RegexMatch* m) {
    using State = VimRegex::Searcher::State;
    const RegexProgram& q = *st.bt;
    const u64 size = t.size();
    to = std::min(to, size + 1);
    if (from >= to) return false;
    st.budget = st.steps + VimRegex::Searcher::State::kBudget;
    st.line_lo = kNone;
    if (q.memo) st.forget_all();
    if (!q.multiline && q.required.size() >= 2) {
        const u64 lim = to > size ? size : line_end(t, to > from ? to - 1 : to);
        // (Looked for from the start of the line: the text a look-behind
        // finds is before where the match starts.)
        for (u64 at = from, look = q.looks.empty() ? from : t.line_start(from); at < to; look = at) {
            const u64 hit = find_literal(t, q.required, q.icase, look, lim + 1);
            if (hit == kNone) return false;
            const u64 ls = std::max(at, t.line_start(hit)), le = State::line_end_near(t, hit);
            if (ls >= to) return false;
            if (st.backtrack(t, ls, std::min(to, le + 1), m)) return true;
            if (st.overran) return false;
            at = le + 1;
        }
        return false;
    }
    if (!q.multiline && !q.cr && q.relaxed && !st.p.empty && !st.dfa_failed && to - from >= 256) {
        // (Where the DFA lets nearly every line through, it is left out for
        // the rest: the backtracker goes on through the text by itself.)
        u64 lines = 0, passed = 0;
        for (u64 at = from; at < to;) {
            bool gave_up = false;
            const u64 ls = st.dfa_line(t, at, to, &gave_up);
            if (gave_up) return st.backtrack(t, at, to, m);
            if (ls == kNone || ls >= to) return false;
            const u64 le = State::line_end_near(t, ls);
            if (st.backtrack(t, ls, std::min(to, le + 1), m)) return true;
            if (st.overran) return false;
            passed += ls == at;
            at = le + 1;
            if (++lines == 256 && passed * 4 > lines * 3) return st.backtrack(t, at, to, m);
        }
        return false;
    }
    return st.backtrack(t, from, to, m);
}

template <class Text>
bool search_in(VimRegex::Searcher::State& st, Text& t, u64 from, u64 to, RegexMatch* m) {
    const RegexProgram& p = st.p;
    const u64 size = t.size();
    to = std::min(to, size + 1);
    if (from >= to) return false;
    // A piece of plain text every match has, on the match's line: the lines it is on, through the VM.
    if (!p.multiline && p.required.size() >= 3) {
        // (The text for a match starting before `to` ends with that line.)
        const u64 lim = to > size ? size : line_end(t, to > from ? to - 1 : to);
        for (u64 at = from; at < to;) {
            const u64 hit = find_literal(t, p.required, p.icase, at, lim + 1);
            if (hit == kNone) return false;
            const u64 ls = std::max(at, t.line_start(hit)), le = line_end(t, hit);
            if (ls >= to) return false;
            if (st.vm(t, ls, std::min(to, le + 1), m)) return true;
            at = le + 1;
        }
        return false;
    }
    // A pattern that stays on one line: the DFA finds the line, the VM the match.
    if (!p.multiline && !p.cr && !st.dfa_failed && to - from >= 256) {
        for (u64 at = from; at < to;) {
            bool gave_up = false;
            const u64 ls = st.dfa_line(t, at, to, &gave_up);
            if (gave_up) return st.vm(t, at, to, m);
            if (ls == kNone || ls >= to) return false;
            const u64 le = line_end(t, ls);
            if (st.vm(t, ls, std::min(to, le + 1), m)) return true;
            at = le + 1;
        }
        return false;
    }
    return st.vm(t, from, to, m);
}

}  // namespace

namespace {

// A program for the pattern's tree, made in one of the Compiler's ways,
// with what is known of its matches beforehand; null if it is too large.
std::unique_ptr<RegexProgram> make_program(const std::vector<Node>& nodes, u32 root, u32 groups, bool icase, const std::vector<CharClass>& classes, Compiler::Mode mode,
                                           bool backref = false) {
    auto prog = std::make_unique<RegexProgram>();
    prog->icase = icase;
    prog->groups = groups;
    prog->anchor = 2 + 2 * groups;
    prog->slots = prog->anchor + 1;  // (the backtracker's marks come after it)
    prog->classes = classes;
    Compiler cc(nodes, *prog, mode, backref);
    if (!cc.emit(root) || prog->code.size() > kMostCode) return nullptr;
    prog->code.push_back({kMatch, 0, 0, 0});
    if (mode == Compiler::kBacktrack) {
        u32 pc = 0;
        while (prog->code[pc].op == kSave || prog->code[pc].op == kMark) ++pc;
        if (prog->code[pc].op == kAssert && prog->code[pc].a == kBol) prog->lead = RegexProgram::kLeadLine;
        if (prog->code[pc].op == kAssert && prog->code[pc].a == kBow) prog->lead = RegexProgram::kLeadWord;
    }
    if (mode == Compiler::kBacktrack && !backref) {
        prog->memo = true;
        prog->split_of.assign(prog->code.size(), kNoChar);
        std::vector<u32> scope(prog->code.size(), 0);
        for (u32 i = 0; i < prog->looks.size(); ++i) {  // (in the order they start: one inside another comes after it)
            u32 at = 0;
            while (!(prog->code[at].op == kLook && prog->code[at].x == i)) ++at;
            for (u32 pc = at + 1; pc < prog->looks[i].end; ++pc) scope[pc] = i + 1;
        }
        for (u32 pc = 0; pc < prog->code.size(); ++pc)
            if (prog->code[pc].op == kSplit) {
                prog->split_of[pc] = static_cast<u32>(prog->scope_of.size());
                prog->scope_of.push_back(scope[pc]);
            }
    }
    for (const Inst& in : prog->code) {
        if (in.op == kNl || (in.op == kAny && in.a) || (in.op == kClass && prog->classes[in.x].newline)) prog->multiline = true;
        if (in.op == kSave && in.x == 0) prog->zs = true;
        if ((in.op == kChar && in.x == '\r') || (in.op == kClass && prog->classes[in.x].cr)) prog->cr = true;
    }
    prog->empty = nullable(nodes, root);
    {
        std::string whole, best;
        if (literal_of(nodes, root, icase, &whole, &best)) best = whole;
        if (icase)
            for (const char ch : best)
                if (static_cast<u8>(ch) >= 0x80) best.clear();
        if (best.find('\n') != std::string::npos) best.clear();
        prog->required = std::move(best);
    }
    // The bytes a match can start at: those the first characters it can take start with.
    if (!prog->empty) {
        std::vector<u32> todo{0};
        std::vector<bool> seen(prog->code.size());
        bool all = false;
        auto lead = [&](u32 cp) {
            std::string s;
            put_utf8(s, cp);
            prog->first[static_cast<u8>(s[0])] = true;
        };
        while (!todo.empty() && !all) {
            const u32 pc = todo.back();
            todo.pop_back();
            if (seen[pc]) continue;
            seen[pc] = true;
            const Inst& in = prog->code[pc];
            switch (in.op) {
                case kJmp: todo.push_back(in.x); break;
                case kSplit: todo.push_back(in.x), todo.push_back(in.y); break;
                case kSave:
                case kAssert:
                case kMark:
                case kCheck:
                case kLookEnd: todo.push_back(pc + 1); break;
                case kLook:  // (one that takes nothing: what comes after it; \@>: what it takes)
                    todo.push_back(prog->looks[in.x].kind == kAtomic ? pc + 1 : prog->looks[in.x].end);
                    break;
                case kChar: lead(in.x); break;
                case kCharI:
                    lead(in.x);
                    lead(upper(in.x));
                    break;
                case kNl: prog->first[static_cast<u8>('\n')] = prog->first[static_cast<u8>('\r')] = true; break;
                case kClass: {
                    const CharClass& c = prog->classes[in.x];
                    for (u32 b = 0; b < 128; ++b)
                        if (c.test(b, icase)) prog->first[b] = true;
                    if (c.cr) prog->first[static_cast<u8>('\r')] = true;
                    bool wide = c.negate;
                    for (const auto& r : c.ranges) wide = wide || r.second >= 0x80;
                    if (wide)
                        for (u32 b = 0x80; b < 256; ++b) prog->first[b] = true;
                    break;
                }
                default: all = true; break;  // (kAny, kMatch, kBackref)
            }
        }
        size_t count = 0;
        for (const bool f : prog->first) count += f;
        prog->skip = !all && count <= 96;
    }
    return prog;
}

}  // namespace

// ---------------------------------------------------------------- the outside

VimRegex::~VimRegex() = default;

bool VimRegex::multiline() const { return prog_ && prog_->multiline; }
bool VimRegex::has_zs() const { return prog_ && prog_->zs; }
bool VimRegex::can_be_empty() const { return prog_ && prog_->empty; }
bool VimRegex::backtracks() const { return prog_ && prog_->backtrack; }
const std::string& VimRegex::required() const {
    static const std::string none;
    return prog_ ? prog_->required : plain_text_.empty() ? none : plain_text_;
}

std::shared_ptr<const VimRegex> VimRegex::compile(std::string_view pattern, const Options& o, std::string* error) {
    Parser ps(pattern, o);
    const u32 root = ps.parse();
    if (!ps.error.empty() || root == kNoChar) {
        if (error) *error = ps.error.empty() ? "E383: Invalid search string" : ps.error;
        return nullptr;
    }
    // The case: \c, \C, or else the options ('smartcase': a capital typed, not after a backslash, makes it exact).
    bool icase = o.ignore_case;
    if (icase && o.smart_case) {
        for (size_t i = 0; i < pattern.size();) {
            if (pattern[i] == '\\') {
                i += 2;
                continue;
            }
            const Decoded d = decode_utf8(reinterpret_cast<const u8*>(pattern.data()) + i, pattern.size() - i);
            if (fold(d.cp) != d.cp) {
                icase = false;
                break;
            }
            i += d.len;
        }
    }
    if (ps.set_noicase) icase = false;
    if (ps.set_icase) icase = true;
    std::shared_ptr<VimRegex> re(new VimRegex);
    re->icase_ = icase;

    // Plain text, perhaps as a whole word: looked for as text by whoever asked.
    {
        const std::vector<Node>& ns = ps.nodes;
        std::vector<u32> seq;
        const Node& r = ns[root];
        if (r.kind == Node::kCat) seq = r.kids;
        else seq = {root};
        bool word = false;
        if (seq.size() >= 2 && ns[seq.front()].kind == Node::kAssertN && ns[seq.front()].c == kBow && ns[seq.back()].kind == Node::kAssertN &&
            ns[seq.back()].c == kEow) {
            word = true;
            seq = std::vector<u32>(seq.begin() + 1, seq.end() - 1);
        }
        std::string text;
        bool plain = !seq.empty();
        for (const u32 k : seq) {
            const Node& n = ns[k];
            if (n.kind == Node::kLit && !(icase && n.c >= 0x80 && cased(n.c))) put_utf8(text, n.c);
            else if (n.kind == Node::kNlN) text += '\n';
            else plain = false;
            if (!plain) break;
        }
        if ((plain && !text.empty()) || r.kind == Node::kEmpty) {  // (nothing at all, as "\c" alone, is plain and empty: no pattern)
            re->plain_ = true;
            re->plain_word_ = word;
            re->plain_text_ = std::move(text);
            return re;
        }
    }

    for (CharClass& c : ps.classes) c.finish(icase);
    const bool bt = ps.backtrack || ps.engine == 1;
    std::unique_ptr<RegexProgram> prog = make_program(ps.nodes, root, ps.groups, icase, ps.classes, bt ? Compiler::kBacktrack : Compiler::kPike, ps.backref);
    if (!prog) {
        if (error) *error = "E363: The pattern is too large";
        return nullptr;
    }
    if (bt) {
        prog->backtrack = true;
        // (Too large, or one that matches nothing everywhere: the backtracker goes through all of it.)
        prog->relaxed = make_program(ps.nodes, root, ps.groups, icase, ps.classes, Compiler::kRelaxed);
        if (prog->relaxed) prog->relaxed->required = prog->required;
    }
    re->prog_ = std::move(prog);
    return re;
}

VimRegex::Searcher::Searcher(const VimRegex& re) : re_(re) {
    if (re.prog_) st_ = std::make_unique<State>(*re.prog_);
}

VimRegex::Searcher::~Searcher() = default;

bool VimRegex::Searcher::next(const Buffer& buf, u64 from, u64 to, RegexMatch* m) {
    BufText t(buf);
    if (!st_) return false;
    if (!st_->bt) return search_in(*st_, t, from, to, m);
    st_->overran = false;
    const bool hit = search_bt(*st_, t, from, to, m);
    if (st_->overran) re_.overruns_.fetch_add(1, std::memory_order_relaxed);
    return hit;
}

bool VimRegex::Searcher::next(std::string_view text, u64 from, u64 to, RegexMatch* m) {
    StrText t(text);
    if (!st_) return false;
    if (!st_->bt) return search_in(*st_, t, from, to, m);
    st_->overran = false;
    const bool hit = search_bt(*st_, t, from, to, m);
    if (st_->overran) re_.overruns_.fetch_add(1, std::memory_order_relaxed);
    return hit;
}

bool VimRegex::Searcher::overran() const { return st_ && st_->overran; }

bool VimRegex::search(const Buffer& buf, u64 from, u64 to, RegexMatch* m) const {
    Searcher s(*this);
    return s.next(buf, from, to, m);
}

bool VimRegex::search(std::string_view text, u64 from, u64 to, RegexMatch* m) const {
    Searcher s(*this);
    return s.next(text, from, to, m);
}

// ---------------------------------------------------------------- replacing

void regex_expand(std::string_view r, const RegexMatch& m, const std::function<std::string(u64, u64)>& text, std::string* out, std::string_view nl) {
    enum Case : u8 { kAsIs, kUpper, kLower };
    Case all = kAsIs, one = kAsIs;
    auto put = [&](std::string_view s) {
        for (size_t i = 0; i < s.size();) {
            const Decoded d = decode_utf8(reinterpret_cast<const u8*>(s.data()) + i, s.size() - i);
            u32 cp = d.cp;
            const Case c = one != kAsIs ? one : all;
            one = kAsIs;
            if (c == kUpper) cp = upper(cp);
            else if (c == kLower) cp = fold(cp);
            if (c == kAsIs) out->append(s.substr(i, d.len));
            else put_utf8(*out, cp);
            i += d.len;
        }
    };
    auto group = [&](u32 g) {
        if (m.group[g][0] == RegexMatch::kUnset || m.group[g][1] < m.group[g][0]) return;
        put(text(m.group[g][0], m.group[g][1]));
    };
    for (size_t k = 0; k < r.size(); ++k) {
        const char ch = r[k];
        if (ch == '&') {
            group(0);
            continue;
        }
        if (ch != '\\' || k + 1 >= r.size()) {
            put(r.substr(k, 1));
            continue;
        }
        const char n = r[++k];
        switch (n) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': group(static_cast<u32>(n - '0')); break;
            case 'r':
            case 'n': out->append(nl); break;
            case 't': *out += '\t'; break;
            case 'u': one = kUpper; break;
            case 'l': one = kLower; break;
            case 'U': all = kUpper; break;
            case 'L': all = kLower; break;
            case 'E':
            case 'e': all = kAsIs; break;
            default: put(r.substr(k, 1)); break;  // "\&", "\\", "\/" and the like: the character
        }
    }
}

bool regex_replacement_plain(std::string_view r) {
    for (size_t k = 0; k < r.size(); ++k) {
        if (r[k] == '&') return false;
        if (r[k] == '\\' && k + 1 < r.size()) {
            const char n = r[++k];
            if ((n >= '0' && n <= '9') || std::strchr("uUlLeE", n)) return false;
        }
    }
    return true;
}

std::string regex_escape(std::string_view text, char delimiter) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        const bool special = ch == '\\' || ch == '.' || ch == '*' || ch == '[' || ch == '~' || ch == delimiter || (ch == '^' && i == 0) ||
                             (ch == '$' && i + 1 == text.size());
        if (special) out += '\\';
        if (ch == '\n') {
            out += "\\n";
            continue;
        }
        out += ch;
    }
    return out;
}

}  // namespace blit
