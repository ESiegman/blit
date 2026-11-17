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

enum Op : u8 { kChar, kCharI, kAny, kClass, kNl, kMatch, kSplit, kJmp, kSave, kAssert };
enum Assert : u8 { kBol, kEol, kBow, kEow, kBof, kEof };

struct Inst {
    Op op;
    u8 a = 0;  // kAny: 1 if it takes a line break too; kAssert: which
    u32 x = 0, y = 0;
};

}  // namespace

struct RegexProgram {
    std::vector<Inst> code;
    std::vector<CharClass> classes;
    u32 groups = 0;  // the highest \( \) group (0 to 9)
    u32 slots = 3;   // start, end, the groups' two each, where the thread began
    bool icase = false, multiline = false, zs = false, empty = false, cr = false;
    std::string required;  // plain text every match has (ASCII, if the case is ignored)
    bool first[256] = {};  // the bytes a match can start at
    bool skip = false;     // and they are few enough to be worth looking for
    u32 anchor_slot() const { return slots - 1; }
};

namespace {

// ---------------------------------------------------------------- reading a pattern

struct Node {
    enum Kind : u8 { kEmpty, kLit, kAnyN, kClassN, kNlN, kCat, kAlt, kRep, kGroup, kAssertN, kZsN, kZeN } kind = kEmpty;
    u32 c = 0;  // kLit: the character; kClassN: which class; kAssertN: which; kGroup: the group (0: one that catches nothing)
    bool nl = false;
    u32 min = 0, max = 0;
    bool greedy = true;
    std::vector<u32> kids;
};

struct Tok {
    enum K : u8 { kEnd, kChar, kMagic, kClassT, kNlT, kZs, kZe, kBof, kEof, kBol, kEol, kOpenPlain, kCollection } k = kEnd;
    u32 c = 0;
    bool nl = false;  // \_x
};

class Parser {
public:
    Parser(std::string_view s, const VimRegex::Options& o) : s_(s), opt_(o) {}

    std::vector<Node> nodes;
    std::vector<CharClass> classes;
    u32 groups = 0;
    bool set_icase = false, set_noicase = false;
    std::string error;

    u32 parse() {
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
            case '9': error = "Back-references (\\1 in a pattern) are not supported"; return t;
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
        const char x = s_[i_++];
        switch (x) {
            case '(': t.k = Tok::kOpenPlain; return t;
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
        std::vector<u32> branches{concat()};
        while (error.empty() && tok().k == Tok::kMagic && (tok_.c == '|' || tok_.c == '&')) {
            if (tok_.c == '&') {
                error = "\\& is not supported";
                break;
            }
            take();
            branches.push_back(concat());
        }
        if (branches.size() == 1) return branches[0];
        Node n;
        n.kind = Node::kAlt;
        n.kids = std::move(branches);
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
            Node r;
            r.kind = Node::kRep;
            r.kids = {a};
            if (c == '*') r.min = 0, r.max = kInf;
            else if (c == '+') r.min = 1, r.max = kInf;
            else if (c == '=' || c == '?') r.min = 0, r.max = 1;
            else if (c == '@') {
                error = "Look-behind and look-ahead (\\@) are not supported";
                return kNoChar;
            } else if (!brace(&r)) {
                return kNoChar;
            }
            a = node(std::move(r));
        }
        return a;
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
        Node n;
        n.kind = Node::kGroup;
        n.c = number;
        n.kids = {inner};
        return node(std::move(n));
    }
};

// ---------------------------------------------------------------- the program from the pattern

class Compiler {
public:
    Compiler(const std::vector<Node>& nodes, RegexProgram& p) : n_(nodes), p_(p) {}
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
            case Node::kAssertN: add({kAssert, static_cast<u8>(n.c), 0, 0}); return true;
            case Node::kZsN: add({kSave, 0, 0, 0}); return true;
            case Node::kZeN: add({kSave, 0, 1, 0}); return true;
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
                    if (!emit(n.kids[0])) return false;
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
    size_t add(Inst i) {
        p_.code.push_back(i);
        return p_.code.size() - 1;
    }
    void branch(size_t split, u32 body, u32 out, bool greedy) {
        p_.code[split].x = greedy ? body : out;
        p_.code[split].y = greedy ? out : body;
    }
};

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
        default: return true;
    }
}

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
        case Node::kGroup: return literal_of(ns, n.kids[0], icase, whole, best);
        case Node::kCat: {
            std::string run;
            bool all = true;
            for (const u32 k : n.kids) {
                std::string part;
                if (literal_of(ns, k, icase, &part, best)) {
                    run += part;
                } else {
                    keep(run);
                    run.clear();
                    all = false;
                    // (A piece repeated at least once still has its own text,
                    // and one repeated an exact number of times starts a run.)
                    std::string inner, inner_best;
                    const Node& kid = ns[k];
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
    u32 before(u64 pos) {
        if (pos == 0 || pos > size_) return kNoChar;
        u8 tmp[4];
        const u64 a = pos >= 4 ? pos - 4 : 0;
        const u64 got = b_.read(a, reinterpret_cast<char*>(tmp), pos - a);
        return last_char(tmp, got);
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

    static u32 last_char(const u8* tmp, u64 got) {
        if (got == 0) return kNoChar;
        u64 k = got - 1;
        while (k > 0 && is_cont(tmp[k])) --k;
        const Decoded d = decode_utf8(tmp + k, static_cast<size_t>(got - k));
        return k + d.len == got ? d.cp : 0xFFFD;
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
    u32 before(u64 pos) const {
        if (pos == 0 || pos > s_.size()) return kNoChar;
        const u64 a = pos >= 4 ? pos - 4 : 0;
        return BufText::last_char(reinterpret_cast<const u8*>(s_.data()) + a, pos - a);
    }
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
    const RegexProgram& p;
    explicit State(const RegexProgram& prog) : p(prog) {
        const size_t n = p.code.size();
        for (List* l : {&a, &b}) {
            l->sparse.assign(n, 0);
            l->pcs.assign(n, 0);
            l->caps.assign(n * p.slots, 0);
        }
        work.assign(p.slots, 0);
        mark.assign(n, 0);
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
    template <class Text>
    u64 dfa_line(Text& t, u64 from, u64 to, bool* gave_up) {
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
};

namespace {

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

// ---------------------------------------------------------------- the outside

VimRegex::~VimRegex() = default;

bool VimRegex::multiline() const { return prog_ && prog_->multiline; }
bool VimRegex::has_zs() const { return prog_ && prog_->zs; }
bool VimRegex::can_be_empty() const { return prog_ && prog_->empty; }
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

    auto prog = std::make_unique<RegexProgram>();
    prog->icase = icase;
    prog->groups = ps.groups;
    prog->slots = 2 + 2 * ps.groups + 1;
    for (CharClass& c : ps.classes) c.finish(icase);
    prog->classes = std::move(ps.classes);
    Compiler cc(ps.nodes, *prog);
    if (!cc.emit(root) || prog->code.size() > kMostCode) {
        if (error) *error = "E363: The pattern is too large";
        return nullptr;
    }
    prog->code.push_back({kMatch, 0, 0, 0});
    for (const Inst& in : prog->code) {
        if (in.op == kNl || (in.op == kAny && in.a) || (in.op == kClass && prog->classes[in.x].newline)) prog->multiline = true;
        if (in.op == kSave && in.x == 0) prog->zs = true;
        if ((in.op == kChar && in.x == '\r') || (in.op == kClass && prog->classes[in.x].cr)) prog->cr = true;
    }
    prog->empty = nullable(ps.nodes, root);
    {
        std::string whole, best;
        if (literal_of(ps.nodes, root, icase, &whole, &best)) best = whole;
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
                case kAssert: todo.push_back(pc + 1); break;
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
                default: all = true; break;  // (kAny, kMatch)
            }
        }
        size_t count = 0;
        for (const bool f : prog->first) count += f;
        prog->skip = !all && count <= 96;
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
    return search_in(*st_, t, from, to, m);
}

bool VimRegex::Searcher::next(std::string_view text, u64 from, u64 to, RegexMatch* m) {
    StrText t(text);
    if (!st_) return false;
    return search_in(*st_, t, from, to, m);
}

bool VimRegex::search(const Buffer& buf, u64 from, u64 to, RegexMatch* m) const {
    Searcher s(*this);
    return s.next(buf, from, to, m);
}

bool VimRegex::search(std::string_view text, u64 from, u64 to, RegexMatch* m) const {
    Searcher s(*this);
    return s.next(text, from, to, m);
}

// ---------------------------------------------------------------- replacing

void regex_expand(std::string_view r, const RegexMatch& m, const std::function<std::string(u64, u64)>& text, std::string* out) {
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
            case 'n': *out += '\n'; break;
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
