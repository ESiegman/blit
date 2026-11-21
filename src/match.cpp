#include "match.hpp"

#include <bitset>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "common.hpp"

namespace blit {

// ---------------------------------------------------------------- Lua patterns
// A port of the matcher in Lua's lstrlib.c (captures and back-references
// included; %b and %f not).

namespace {

struct Lua {
    Lua(const char* text_end_, const char* pat_end_) : text_end(text_end_), pat_end(pat_end_) {}
    const char* text_end;
    const char* pat_end;
    int depth = 0;
    // The captures open and closed so far: where each starts, and its length
    // (kOpen: not closed yet; kPosition: "()", a place alone).
    static constexpr ptrdiff_t kOpen = -1, kPosition = -2;
    struct Capture {
        const char* at;
        ptrdiff_t len;
    } caps[kMaxLuaCaptures];  // (only those below level are ever read)
    int level = 0;
    // A pattern like ".-.-.-x" can take time exponential in the text, so the
    // work is capped; running out ends the search as no match.
    static constexpr long kBudget = 1 << 18;
    long steps = 0;
    bool unsupported = false;

    static bool in_class(int c, int cl) {
        bool r;
        switch (std::tolower(cl)) {
            case 'a': r = std::isalpha(c); break;
            case 'c': r = std::iscntrl(c); break;
            case 'd': r = std::isdigit(c); break;
            case 'g': r = std::isgraph(c); break;
            case 'l': r = std::islower(c); break;
            case 'p': r = std::ispunct(c); break;
            case 's': r = std::isspace(c); break;
            case 'u': r = std::isupper(c); break;
            case 'w': r = std::isalnum(c); break;
            case 'x': r = std::isxdigit(c); break;
            default: return cl == c;
        }
        return std::isupper(cl) ? !r : r;
    }

    // End of the single-character pattern item starting at p.
    const char* item_end(const char* p) {
        if (p >= pat_end) return p;
        const char c = *p++;
        if (c == '%') return p < pat_end ? p + 1 : p;
        if (c == '[') {
            if (p < pat_end && *p == '^') ++p;
            do {  // the first character after [ or [^ is always literal
                if (p >= pat_end) {  // no closing ]
                    unsupported = true;
                    return pat_end;
                }
                if (*p++ == '%' && p < pat_end) ++p;
            } while (p >= pat_end || *p != ']');
            return p + 1;
        }
        return p;
    }

    bool in_set(int c, const char* p, const char* ec) {  // p at '[', ec at ']'
        bool sig = true;
        if (*(p + 1) == '^') {
            sig = false;
            ++p;
        }
        while (++p < ec) {
            if (*p == '%') {
                ++p;
                if (in_class(c, static_cast<unsigned char>(*p))) return sig;
            } else if (*(p + 1) == '-' && p + 2 < ec) {
                p += 2;
                if (static_cast<unsigned char>(*(p - 2)) <= c && c <= static_cast<unsigned char>(*p)) return sig;
            } else if (static_cast<unsigned char>(*p) == c) {
                return sig;
            }
        }
        return !sig;
    }

    bool single(const char* s, const char* p, const char* ep) {
        if (s >= text_end) return false;
        const int c = static_cast<unsigned char>(*s);
        switch (*p) {
            case '.': return true;
            case '%': return in_class(c, static_cast<unsigned char>(*(p + 1)));
            case '[': return in_set(c, p, ep - 1);
            default: return static_cast<unsigned char>(*p) == c;
        }
    }

    // Matches the pattern from p against the text from s; returns where the
    // match ends, or null.
    const char* match(const char* s, const char* p) {
        if (unsupported || ++steps > kBudget) {
            unsupported = true;
            return nullptr;
        }
        if (++depth > 200) {
            --depth;
            return nullptr;
        }
        const char* result = nullptr;
        for (;;) {
            if (p >= pat_end) {
                result = s;
                break;
            }
            if (*p == '(') {
                const bool position = p + 1 < pat_end && *(p + 1) == ')';
                result = open_capture(s, position ? p + 2 : p + 1, position ? kPosition : kOpen);
                break;
            }
            if (*p == ')') {
                result = close_capture(s, p + 1);
                break;
            }
            if (*p == '$' && p + 1 == pat_end) {
                result = s == text_end ? s : nullptr;
                break;
            }
            if (*p == '%' && p + 1 < pat_end && (*(p + 1) == 'b' || *(p + 1) == 'f')) {
                unsupported = true;
                break;
            }
            if (*p == '%' && p + 1 >= pat_end) {  // a pattern cannot end in a lone %
                unsupported = true;
                break;
            }
            if (*p == '%' && *(p + 1) >= '0' && *(p + 1) <= '9') {  // %1: what capture 1 took, again
                const int l = *(p + 1) - '1';
                if (l < 0 || l >= level || caps[l].len == kOpen) {
                    unsupported = true;
                    break;
                }
                const ptrdiff_t len = caps[l].len == kPosition ? 0 : caps[l].len;
                if (text_end - s < len || std::memcmp(caps[l].at, s, static_cast<size_t>(len)) != 0) break;
                s += len;
                p += 2;
                continue;
            }
            const char* ep = item_end(p);
            if (unsupported) break;
            const char q = ep < pat_end ? *ep : '\0';
            if (q == '?') {
                if (single(s, p, ep)) {
                    if (const char* r = match(s + 1, ep + 1)) {
                        result = r;
                        break;
                    }
                }
                p = ep + 1;
                continue;
            }
            if (q == '+' || q == '*') {
                if (q == '+' && !single(s, p, ep)) break;
                const char* from = q == '+' ? s + 1 : s;
                ptrdiff_t n = 0;
                while (single(from + n, p, ep)) ++n;
                for (; n >= 0; --n)  // longest first
                    if (const char* r = match(from + n, ep + 1)) {
                        result = r;
                        break;
                    }
                break;
            }
            if (q == '-') {  // shortest first
                for (;;) {
                    if (const char* r = match(s, ep + 1)) {
                        result = r;
                        break;
                    }
                    if (!single(s, p, ep)) break;
                    ++s;
                }
                break;
            }
            if (!single(s, p, ep)) break;
            ++s;
            p = ep;
        }
        --depth;
        return result;
    }

    const char* open_capture(const char* s, const char* p, ptrdiff_t what) {
        if (level >= kMaxLuaCaptures) {
            unsupported = true;
            return nullptr;
        }
        caps[level] = {s, what};
        ++level;
        const char* r = match(s, p);
        if (!r) --level;  // (undone, for the next try)
        return r;
    }

    const char* close_capture(const char* s, const char* p) {
        int l = level - 1;
        while (l >= 0 && caps[l].len != kOpen) --l;
        if (l < 0) {  // a ) with no ( before it
            unsupported = true;
            return nullptr;
        }
        caps[l].len = s - caps[l].at;
        const char* r = match(s, p);
        if (!r) caps[l].len = kOpen;
        return r;
    }

    // Where the pattern first matches from `from` on (searching, unless it
    // starts with ^); null if nowhere.
    const char* find(const char* from, std::string_view pattern) {
        const char* p = pattern.data();
        const bool anchored = !pattern.empty() && *p == '^';
        if (anchored) ++p;
        for (const char* s = from;; ++s) {
            level = 0;
            if (match(s, p)) return s;
            if (anchored || unsupported || s >= text_end) return nullptr;
        }
    }
};

}  // namespace

bool lua_match(std::string_view text, std::string_view pattern) {
    Lua m{text.data() + text.size(), pattern.data() + pattern.size()};
    return m.find(text.data(), pattern) != nullptr;
}

int lua_find(std::string_view text, std::string_view pattern, std::string_view* caps) {
    Lua m{text.data() + text.size(), pattern.data() + pattern.size()};
    if (!m.find(text.data(), pattern)) return -1;
    for (int i = 0; i < m.level; ++i) {
        const Lua::Capture& c = m.caps[i];
        caps[i] = c.len < 0 ? std::string_view(c.at, 0) : std::string_view(c.at, static_cast<size_t>(c.len));
    }
    return m.level;
}

int lua_pattern_check(std::string_view pattern, std::string* why) {
    int count = 0, open = 0;
    for (size_t i = pattern.starts_with('^') ? 1 : 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '%') {
            if (i + 1 >= pattern.size()) return *why = "it ends in a lone %", -1;
            const char e = pattern[++i];
            if (e == 'b' || e == 'f') return *why = "%b and %f are not supported", -1;
            if (e >= '0' && e <= '9' && (e == '0' || e - '0' > count)) return *why = std::string("%") + e + " names no capture before it", -1;
        } else if (c == '[') {
            size_t j = i + 1;
            if (j < pattern.size() && pattern[j] == '^') ++j;
            do {  // (as item_end: the first character in a set is itself, ] included)
                if (j >= pattern.size()) break;
                if (pattern[j++] == '%' && j < pattern.size()) ++j;
            } while (j < pattern.size() && pattern[j] != ']');
            if (j >= pattern.size()) return *why = "a [ is not closed", -1;
            i = j;
        } else if (c == '(') {
            if (++count > kMaxLuaCaptures) return *why = "it has more than " + std::to_string(kMaxLuaCaptures) + " captures", -1;
            if (i + 1 < pattern.size() && pattern[i + 1] == ')') ++i;
            else ++open;
        } else if (c == ')') {
            if (--open < 0) return *why = "a ) closes no (", -1;
        }
    }
    if (open) return *why = "a ( is not closed", -1;
    return count;
}

// ---------------------------------------------------------------- regular expressions
// Parsed to a tree, compiled to a small program, and run as a Pike VM, so
// matching takes time proportional to text length times pattern size
// whatever the pattern is.

namespace {

struct Node {
    enum Kind : u8 { kEmpty, kChar, kAny, kSet, kStart, kEnd, kCat, kAlt, kStar, kPlus, kQuest } kind;
    u32 a = 0, b = 0;  // children, or the character / set index
};

struct Inst {
    enum Op : u8 { kChar, kAny, kSet, kSplit, kJump, kStart, kEnd, kMatch } op;
    u32 x = 0, y = 0;  // kChar: character. kSet: set index. kSplit: both targets. kJump: target.
};

struct Program {
    std::vector<Node> nodes;
    std::vector<Inst> prog;
    std::vector<std::bitset<256>> sets;
    std::string_view pat;
    size_t at = 0;
    bool icase = false;
    bool bad = false;

    u32 node(Node n) {
        nodes.push_back(n);
        return static_cast<u32>(nodes.size() - 1);
    }
    u32 set_node(const std::bitset<256>& set) {
        sets.push_back(set);
        return node({Node::kSet, static_cast<u32>(sets.size() - 1)});
    }
    void add_range(std::bitset<256>& set, int lo, int hi) {
        for (int c = lo; c <= hi; ++c) {
            set.set(static_cast<size_t>(c));
            if (icase && std::isalpha(c)) {
                set.set(static_cast<size_t>(std::tolower(c)));
                set.set(static_cast<size_t>(std::toupper(c)));
            }
        }
    }
    static bool escape_class(std::bitset<256>& set, char e) {
        std::bitset<256> cls;
        switch (std::tolower(static_cast<unsigned char>(e))) {
            case 'd':
                for (int c = '0'; c <= '9'; ++c) cls.set(static_cast<size_t>(c));
                break;
            case 'w':
                for (int c = 0; c < 128; ++c)
                    if (std::isalnum(c) || c == '_') cls.set(static_cast<size_t>(c));
                break;
            case 's':
                for (char c : {' ', '\t', '\n', '\r', '\f', '\v'}) cls.set(static_cast<unsigned char>(c));
                break;
            default: return false;
        }
        if (std::isupper(static_cast<unsigned char>(e))) cls.flip();
        set |= cls;
        return true;
    }
    u32 literal(char c) {
        if (icase && std::isalpha(static_cast<unsigned char>(c))) {
            std::bitset<256> set;
            add_range(set, static_cast<unsigned char>(c), static_cast<unsigned char>(c));
            return set_node(set);
        }
        return node({Node::kChar, static_cast<unsigned char>(c)});
    }

    // alternation := sequence ('|' sequence)*
    u32 alternation() {
        u32 left = sequence();
        while (at < pat.size() && pat[at] == '|') {
            ++at;
            const u32 right = sequence();
            left = node({Node::kAlt, left, right});
        }
        return left;
    }
    u32 sequence() {
        u32 seq = node({Node::kEmpty});
        while (!bad && at < pat.size() && pat[at] != '|' && pat[at] != ')') {
            u32 item = atom();
            while (at < pat.size() && (pat[at] == '*' || pat[at] == '+' || pat[at] == '?')) {
                const char q = pat[at++];
                item = node({q == '*' ? Node::kStar : q == '+' ? Node::kPlus : Node::kQuest, item});
            }
            seq = node({Node::kCat, seq, item});
        }
        return seq;
    }
    u32 atom() {
        const char c = pat[at++];
        if (c == '(') {
            if (at + 1 < pat.size() && pat[at] == '?' && pat[at + 1] == ':') at += 2;
            const u32 inner = alternation();
            if (at >= pat.size() || pat[at] != ')') bad = true;
            else ++at;
            return inner;
        }
        if (c == '[') {
            std::bitset<256> set;
            const bool neg = at < pat.size() && pat[at] == '^';
            if (neg) ++at;
            for (bool first = true; at < pat.size() && (pat[at] != ']' || first); first = false) {
                char lo = pat[at++];
                if (lo == '\\' && at < pat.size()) {
                    lo = pat[at++];
                    if (escape_class(set, lo)) continue;
                }
                if (at + 1 < pat.size() && pat[at] == '-' && pat[at + 1] != ']') {
                    add_range(set, static_cast<unsigned char>(lo), static_cast<unsigned char>(pat[at + 1]));
                    at += 2;
                } else {
                    add_range(set, static_cast<unsigned char>(lo), static_cast<unsigned char>(lo));
                }
            }
            if (at >= pat.size()) {
                bad = true;
                return node({Node::kEmpty});
            }
            ++at;
            if (neg) set.flip();
            return set_node(set);
        }
        if (c == '.') return node({Node::kAny});
        if (c == '^') return node({Node::kStart});
        if (c == '$') return node({Node::kEnd});
        if (c == '\\' && at < pat.size()) {
            const char e = pat[at++];
            if (e == 'c') {
                icase = true;  // Vim: ignore case from here on
                return node({Node::kEmpty});
            }
            if (e == 'v' || e == 'm') return node({Node::kEmpty});  // Vim "magic" markers
            std::bitset<256> set;
            if (escape_class(set, e)) return set_node(set);
            return literal(e);
        }
        if (c == '*' || c == '+' || c == '?') {
            bad = true;  // nothing to repeat
            return node({Node::kEmpty});
        }
        return literal(c);
    }

    u32 emit(Inst i) {
        prog.push_back(i);
        return static_cast<u32>(prog.size() - 1);
    }
    u32 here() const { return static_cast<u32>(prog.size()); }
    void gen(u32 n) {
        const Node nd = nodes[n];
        switch (nd.kind) {
            case Node::kEmpty: break;
            case Node::kChar: emit({Inst::kChar, nd.a}); break;
            case Node::kAny: emit({Inst::kAny}); break;
            case Node::kSet: emit({Inst::kSet, nd.a}); break;
            case Node::kStart: emit({Inst::kStart}); break;
            case Node::kEnd: emit({Inst::kEnd}); break;
            case Node::kCat:
                gen(nd.a);
                gen(nd.b);
                break;
            case Node::kAlt: {
                const u32 split = emit({Inst::kSplit});
                prog[split].x = here();
                gen(nd.a);
                const u32 jump = emit({Inst::kJump});
                prog[split].y = here();
                gen(nd.b);
                prog[jump].x = here();
                break;
            }
            case Node::kStar: {
                const u32 split = emit({Inst::kSplit});
                prog[split].x = here();
                gen(nd.a);
                emit({Inst::kJump, split});
                prog[split].y = here();
                break;
            }
            case Node::kPlus: {
                const u32 start = here();
                gen(nd.a);
                const u32 split = emit({Inst::kSplit, start});
                prog[split].y = here();
                break;
            }
            case Node::kQuest: {
                const u32 split = emit({Inst::kSplit});
                prog[split].x = here();
                gen(nd.a);
                prog[split].y = here();
                break;
            }
        }
    }

    bool compile(std::string_view pattern) {
        pat = pattern;
        const u32 root = alternation();
        if (at != pat.size()) bad = true;
        if (bad) return false;
        gen(root);
        emit({Inst::kMatch});
        return true;
    }

    // Adds a thread at pc, following splits, jumps and assertions.
    void add(std::vector<u32>& list, std::vector<u32>& seen, u32 stamp, u32 pc, bool at_start, bool at_end) const {
        if (seen[pc] == stamp) return;
        seen[pc] = stamp;
        const Inst& in = prog[pc];
        switch (in.op) {
            case Inst::kJump: add(list, seen, stamp, in.x, at_start, at_end); break;
            case Inst::kSplit:
                add(list, seen, stamp, in.x, at_start, at_end);
                add(list, seen, stamp, in.y, at_start, at_end);
                break;
            case Inst::kStart:
                if (at_start) add(list, seen, stamp, pc + 1, at_start, at_end);
                break;
            case Inst::kEnd:
                if (at_end) add(list, seen, stamp, pc + 1, at_start, at_end);
                break;
            default: list.push_back(pc);
        }
    }

    bool search(std::string_view text) const {
        std::vector<u32> cur, next, seen(prog.size(), 0);
        u32 stamp = 1;
        add(cur, seen, stamp, 0, true, text.empty());
        for (size_t i = 0;; ++i) {
            const bool at_end = i == text.size();
            next.clear();
            ++stamp;
            for (u32 pc : cur) {
                const Inst& in = prog[pc];
                if (in.op == Inst::kMatch) return true;
                if (at_end) continue;
                const unsigned char c = static_cast<unsigned char>(text[i]);
                const bool ok = in.op == Inst::kAny || (in.op == Inst::kChar && in.x == c) ||
                                (in.op == Inst::kSet && sets[in.x].test(c));
                if (ok) add(next, seen, stamp, pc + 1, false, i + 1 == text.size());
            }
            if (at_end) return false;
            add(next, seen, stamp, 0, false, i + 1 == text.size());  // a match may also begin here
            cur.swap(next);
        }
    }
};

}  // namespace

struct Regex::Impl {
    Program p;
};

Regex::Regex(std::string_view pattern) : impl_(std::make_unique<Impl>()) {
    // The pattern text is only read while compiling.
    ok_ = impl_->p.compile(pattern);
}
Regex::Regex(Regex&&) noexcept = default;
Regex::~Regex() = default;

bool Regex::search(std::string_view text) const { return ok_ && impl_->p.search(text); }

bool regex_match(std::string_view text, std::string_view pattern) { return Regex(pattern).search(text); }

}  // namespace blit
