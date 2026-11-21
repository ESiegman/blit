// Part of the build's output read for problems (buildscan.hpp): init.lua's own patterns (blit.build.matcher), compiled into a table of native patterns, tried before the built-in kinds.
#include "buildmatch.hpp"

#include <cctype>

#include "buildscan.hpp"
#include "match.hpp"

namespace blit {

namespace {

constexpr size_t kMaxMessage = 600;  // bytes of a problem's message kept (as the built-in kinds')
constexpr u32 kMaxMore = 16;         // lines after its first that one problem takes

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

u32 number(std::string_view s) {
    u64 n = 0;
    for (size_t i = 0; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) n = std::min<u64>(n * 10 + static_cast<u64>(s[i] - '0'), 0xFFFFFFFFu);
    return static_cast<u32>(n);
}

// Whether a line could be one the pattern takes: it has the pattern's text.
bool could_be(const BuildPattern& p, std::string_view text) {
    if (p.literal.empty()) return true;
    return p.at_start ? text.starts_with(p.literal) : text.find(p.literal) != std::string_view::npos;
}

// What the pattern's captures took into a problem: what it has not got yet
// (a line after the first fills in its place), the message after its own.
void take(const BuildPattern& p, const std::string_view* caps, int n, std::string_view whole, bool first, BuildProblem* to) {
    auto cap = [&](u8 i) { return i && i <= n ? trimmed(caps[i - 1]) : std::string_view(); };
    if (const std::string_view f = cap(p.file); !f.empty() && to->path.empty()) to->path = f;
    if (const u32 l = number(cap(p.line)); l && !to->line) to->line = l;
    if (const u32 c = number(cap(p.col)); c && !to->col) to->col = c;
    if (const std::string_view c = cap(p.code); !c.empty() && to->code.empty()) to->code = c;
    const std::string_view m = p.message ? cap(p.message) : first ? trimmed(whole) : std::string_view();
    if (m.empty() || to->message.size() >= kMaxMessage) return;
    if (!to->message.empty()) to->message += ' ';
    to->message.append(m.substr(0, kMaxMessage - to->message.size()));
}

}  // namespace

bool build_pattern(std::string pattern, BuildPattern* out, std::string* why) {
    const int captures = lua_pattern_check(pattern, why);
    if (captures < 0) return false;
    out->captures = static_cast<u8>(captures);
    // The text it has to have: the longest run of plain characters each
    // taken once (one taken one or more times ends a run, after it), or
    // the run it starts with when it is anchored.
    const std::string_view p = pattern;
    const bool anchored = p.starts_with('^');
    std::string best, run, prefix;
    bool prefix_done = !anchored;
    auto end_run = [&] {
        if (!prefix_done) prefix = run, prefix_done = true;
        if (run.size() > best.size()) best = run;
        run.clear();
    };
    for (size_t i = anchored ? 1 : 0; i < p.size();) {
        const char c = p[i];
        if (c == '(' || c == ')') {  // (captures take no text of their own)
            ++i;
            continue;
        }
        if (c == '$' && i + 1 == p.size()) break;
        size_t e = i + 1;
        bool plain = false;
        char ch = c;
        if (c == '%') {
            ch = p[i + 1];
            e = i + 2;
            plain = !std::isalnum(static_cast<unsigned char>(ch));  // (%. is a dot; %a, %1 are not one character)
        } else if (c == '[') {
            size_t j = i + 1;
            if (j < p.size() && p[j] == '^') ++j;
            do {  // (the first character in a set is itself, ] included)
                if (p[j++] == '%') ++j;
            } while (j < p.size() && p[j] != ']');
            e = j + 1;
        } else {
            plain = c != '.';
        }
        const char q = e < p.size() ? p[e] : '\0';
        const bool repeated = q == '?' || q == '*' || q == '-' || q == '+';
        if (plain && !repeated) {
            run += ch;
            i = e;
            continue;
        }
        if (plain && q == '+') run += ch;
        end_run();
        i = repeated ? e + 1 : e;
    }
    end_run();
    out->at_start = anchored && !prefix.empty();
    out->literal = out->at_start ? prefix : best;
    out->pattern = std::move(pattern);
    return true;
}

u8 build_severity(std::string_view word) {
    std::string w;
    for (const char c : trimmed(word)) w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "e" || w == "f") return 1;
    if (w == "w") return 2;
    if (w.find("error") != std::string::npos || w.find("fatal") != std::string::npos || w.find("fail") != std::string::npos) return 1;
    if (w.find("warn") != std::string::npos) return 2;
    return 0;
}

void BuildScan::use_matchers(std::shared_ptr<const BuildMatchers> m) { matchers_ = std::move(m); }

bool BuildScan::matcher_line(std::string_view text) {
    if (!matchers_) return false;
    if (matchers_->none) return true;  // (nothing in it is a problem)
    std::string_view caps[kMaxLuaCaptures];
    if (match_held_) {
        const BuildPattern& next = match_held_->next;
        if (match_more_ < kMaxMore && could_be(next, text))
            if (const int n = lua_find(text, next.pattern, caps); n >= 0) {
                ++match_more_;
                take(next, caps, n, text, false, &match_problem_);
                return true;
            }
        matcher_end();
    }
    for (const BuildMatcher& m : matchers_->list) {
        if (!could_be(m.first, text)) continue;
        const int n = lua_find(text, m.first.pattern, caps);
        if (n < 0) continue;
        const std::string_view said = m.first.severity && m.first.severity <= n ? caps[m.first.severity - 1] : std::string_view();
        const u8 severity = said.empty() ? m.severity : build_severity(said);
        if (!severity) continue;  // (a note, or information: no problem, and the other kinds may read it)
        BuildProblem p;
        p.path = m.file;
        p.line = 0;
        p.severity = severity;
        take(m.first, caps, n, text, true, &p);
        if (!m.next.pattern.empty()) {  // (added when a line does not go on with it)
            match_held_ = &m;
            match_problem_ = std::move(p);
            match_more_ = 0;
            return true;
        }
        if (!p.path.empty())
            if (BuildProblem* added = add(p.path, p.line ? p.line : 1, p.col, p.severity, p.message)) added->code = std::move(p.code);
        return true;
    }
    return false;
}

void BuildScan::matcher_end() {
    if (!match_held_) return;
    match_held_ = nullptr;
    BuildProblem& p = match_problem_;
    if (p.path.empty()) return;  // (no line named its place)
    if (BuildProblem* added = add(p.path, p.line ? p.line : 1, p.col, p.severity, p.message)) added->code = std::move(p.code);
}

}  // namespace blit
