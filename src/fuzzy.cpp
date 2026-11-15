#include "fuzzy.hpp"

#include <algorithm>
#include <cstring>

namespace blit {

namespace {

inline u8 low(u8 c) { return static_cast<u8>(c - 'A') < 26 ? static_cast<u8>(c + 32) : c; }
inline bool is_lower(u8 c) { return static_cast<u8>(c - 'a') < 26; }
inline bool is_upper(u8 c) { return static_cast<u8>(c - 'A') < 26; }
inline bool is_digit(u8 c) { return static_cast<u8>(c - '0') < 10; }

// The numbers are fzf's: a matched letter is worth 16, a gap costs 3 to open
// and 1 for each letter more, and where a letter stands earns a bonus.
constexpr int kMatch = 16, kGapStart = -3, kGapMore = -1;
constexpr int kAfterSlash = 9, kAfterMark = 8, kHump = 7, kInRun = 4;
constexpr int kFirstTimes = 2;  // the first letter's bonus counts double
constexpr int kInName = 40;     // a term found in the file's name alone

// What a letter earns for where it stands: at the start of the text or
// after a slash, after another mark that parts words, or at a hump (a
// capital after a small letter, a digit after a letter).
inline int place_bonus(u8 before, u8 c) {
    if (before == 0 || before == '/') return kAfterSlash;
    if (before == '_' || before == '-' || before == '.' || before == ' ') return kAfterMark;
    if ((is_lower(before) && is_upper(c)) || (!is_digit(before) && is_digit(c))) return kHump;
    return 0;
}

// How common each byte is in prose and in code, roughly: its place in this
// list, the commonest first. A byte that is not in it (a capital, most
// marks) is rarer than all that are.
constexpr std::string_view kByCommonness = " etaoinsrlcdupmhfgbyvw.,()_;=k\"x0-1/:2*q{}j<>z'&[]+!3456789|#%\\?@^`~$";
struct Rarity {
    u8 rank[256];
    constexpr Rarity() : rank{} {
        for (u8& r : rank) r = 255;
        for (size_t i = 0; i < kByCommonness.size(); ++i) rank[static_cast<u8>(kByCommonness[i])] = static_cast<u8>(i);
    }
};
constexpr Rarity kRarity;

}  // namespace

std::vector<FuzzyTerm> fuzzy_terms(std::string_view typed) {
    std::vector<FuzzyTerm> out;
    for (size_t i = 0; i < typed.size();) {
        while (i < typed.size() && typed[i] == ' ') ++i;
        size_t e = i;
        while (e < typed.size() && typed[e] != ' ') ++e;
        if (e > i) {
            FuzzyTerm t;
            t.text = std::string(typed.substr(i, e - i));
            for (const char c : t.text) t.exact_case = t.exact_case || is_upper(static_cast<u8>(c));
            out.push_back(std::move(t));
        }
        i = e;
    }
    return out;
}

int fuzzy_score(const FuzzyTerm& term, std::string_view text, std::vector<u32>* at) {
    const size_t m = term.text.size(), n = text.size();
    if (m == 0) return 0;
    if (m > n) return -1;
    const u8* p = reinterpret_cast<const u8*>(term.text.data());
    const u8* t = reinterpret_cast<const u8*>(text.data());
    const bool exact = term.exact_case;
    auto same = [&](size_t ti, size_t pi) { return (exact ? t[ti] : low(t[ti])) == p[pi]; };

    // Forward: the first place by which all the letters have been found,
    // each taken as early as it comes.
    size_t pi = 0, first = 0, end = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!same(i, pi)) continue;
        if (pi == 0) first = i;
        if (++pi == m) {
            end = i + 1;
            break;
        }
    }
    if (pi < m) return -1;
    // Backward from there: the latest place they can start and still end
    // there, which is the closest fit.
    size_t tight = first;
    pi = m;
    for (size_t i = end; i-- > first;) {
        if (same(i, pi - 1) && --pi == 0) {
            tight = i;
            break;
        }
    }
    // The score of the letters taken as early as they come from `start`.
    auto score_from = [&](size_t start, std::vector<u32>* where) {
        int score = 0, run = 0, run_bonus = 0;
        bool gap = false;
        size_t k = 0;
        for (size_t i = start; i < end && k < m; ++i) {
            if (same(i, k)) {
                int bonus = place_bonus(i ? t[i - 1] : 0, t[i]);
                if (run == 0) {
                    run_bonus = bonus;
                } else {
                    // Letters in a run share the bonus of the letter the run began with.
                    if (bonus >= kAfterMark && bonus > run_bonus) run_bonus = bonus;
                    bonus = std::max({bonus, run_bonus, kInRun});
                }
                score += kMatch + (k == 0 ? bonus * kFirstTimes : bonus);
                if (where) where->push_back(static_cast<u32>(i));
                gap = false;
                ++run;
                ++k;
            } else {
                score += gap ? kGapMore : kGapStart;
                gap = true;
                run = 0;
                run_bonus = 0;
            }
        }
        return score;
    };
    // Two ways to lay the letters on the text: from the first place the
    // first letter is (which is where a name starts, if it starts with it),
    // and the closest fit. The better one counts.
    const int close = score_from(tight, nullptr);
    const int early = tight == first ? close : score_from(first, nullptr);
    if (at) score_from(early > close ? first : tight, at);
    return std::max(early, close);
}

int fuzzy_path_score(std::span<const FuzzyTerm> terms, std::string_view path, std::vector<u32>* at) {
    const size_t slash = path.rfind('/');
    const size_t name = slash == std::string_view::npos ? 0 : slash + 1;
    int total = 0;
    for (const FuzzyTerm& term : terms) {
        int best = -1;
        // In the file's name alone, if it is all there (and no folder was typed).
        if (name > 0 && term.text.find('/') == std::string::npos) {
            const size_t mark = at ? at->size() : 0;
            const int s = fuzzy_score(term, path.substr(name), at);
            if (s >= 0) {
                best = s + kInName;
                if (at)
                    for (size_t i = mark; i < at->size(); ++i) (*at)[i] += static_cast<u32>(name);
            }
        }
        if (best < 0) {
            best = fuzzy_score(term, path, at);
            if (best < 0) return -1;
            if (name == 0) best += kInName;  // (a file at the top is all name)
        }
        total += best;
    }
    // Of equals, the one nearer the top and the shorter.
    return total - static_cast<int>(std::min<size_t>(path.size(), 240) / 16);
}

size_t find_term(std::string_view hay, const FuzzyTerm& term, size_t from) { return find_text(hay, term.text, !term.exact_case, from); }

size_t find_text(std::string_view hay, std::string_view needle, bool any_case, size_t from, size_t until) {
    const size_t m = needle.size();
    if (m == 0 || hay.size() < m) return std::string_view::npos;
    const size_t end = std::min(until, hay.size() - m + 1);  // past the last place it could start
    if (from >= end) return std::string_view::npos;
    // The text is passed over by looking for one byte of the needle, with
    // memchr, and the needle is compared only where that byte is. The byte
    // looked for is the needle's rarest (an "x" rather than an "e"): the
    // fewer places it is, the fewer comparisons and the longer memchr runs.
    auto rank = [&](u8 b) { return kRarity.rank[any_case ? low(b) : b]; };
    size_t k = 0;
    for (size_t i = 1; i < m; ++i)
        if (rank(static_cast<u8>(needle[i])) > rank(static_cast<u8>(needle[k]))) k = i;
    const u8 nk = static_cast<u8>(needle[k]);
    const char c = static_cast<char>(any_case ? low(nk) : nk);
    // (A letter of a needle that takes either case may be either in the text.)
    const char other = any_case && is_lower(static_cast<u8>(c)) ? static_cast<char>(c - 32) : c;
    const char* const base = hay.data();
    const char* const first = base + from + k;  // the first place that byte could be,
    const char* const last = base + end - 1 + k;  // and the last
    auto same = [&](const char* at) {
        if (!any_case) return std::memcmp(at, needle.data(), m) == 0;
        for (size_t i = 0; i < m; ++i)
            if (low(static_cast<u8>(at[i])) != low(static_cast<u8>(needle[i]))) return false;
        return true;
    };
    // The two cases are looked for separately, each from where it was last
    // found, so that a rare one is not searched for again and again.
    const char* a = static_cast<const char*>(std::memchr(first, c, static_cast<size_t>(last - first) + 1));
    const char* b = other != c ? static_cast<const char*>(std::memchr(first, other, static_cast<size_t>(last - first) + 1)) : nullptr;
    while (a || b) {
        const bool lower = a && (!b || a < b);
        const char* const p = lower ? a : b;
        if (same(p - k)) return static_cast<size_t>(p - k - base);
        const char* const next = p + 1;
        const char* found = next > last ? nullptr : static_cast<const char*>(std::memchr(next, lower ? c : other, static_cast<size_t>(last - next) + 1));
        (lower ? a : b) = found;
    }
    return std::string_view::npos;
}

}  // namespace blit
