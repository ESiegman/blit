#include "match.hpp"

#include <bitset>
#include <cctype>
#include <vector>

#include "common.hpp"

namespace blit {

// ---------------------------------------------------------------- Lua patterns
// A port of the matcher in Lua's lstrlib.c, without captures.

namespace {

struct Lua {
    const char* text_end;
    const char* pat_end;
    int depth = 0;
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
            if (*p == '(' || *p == ')') {  // captures only mark positions
                ++p;
                continue;
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
};

}  // namespace

bool lua_match(std::string_view text, std::string_view pattern) {
    Lua m{text.data() + text.size(), pattern.data() + pattern.size()};
    const char* p = pattern.data();
    const bool anchored = !pattern.empty() && *p == '^';
    if (anchored) ++p;
    for (const char* s = text.data();; ++s) {
        if (m.match(s, p)) return true;
        if (anchored || m.unsupported || s >= m.text_end) return false;
    }
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
