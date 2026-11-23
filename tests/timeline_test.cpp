// The timeline model (src/timeline.hpp): lanes built a change at a time,
// checked against plain lists and answers worked out from them by brute
// force. tests/wave.sh runs it.
//
//   blit_timeline_test check DIR [SEEDS]   every check
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "timeline.hpp"

using namespace blit;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    if (++failures > 40) std::exit(1);
}

std::string fmt(const Lane& l, u64 i) {
    std::string s;
    l.format(i, &s);
    return s;
}

struct RefChange {
    u64 t;
    std::string v;
};

// A lane built a change at a time against a plain list, through blocks,
// values over 64 bits, x and z arriving late, and changes at one time.
void lane_test(int seeds) {
    for (int seed = 1; seed <= seeds; ++seed) {
        std::mt19937_64 rng(static_cast<u64>(seed));
        auto rnd = [&](u64 n) { return n ? rng() % n : 0; };
        const u32 widths[] = {1, 3, 64, 65, 100};
        const u32 width = widths[seed % 5];
        Lane l(LaneKind::kBits, width);
        std::vector<RefChange> ref;
        const u32 words = (width + 63) / 64;
        u64 t = rnd(1000);
        const int n = 2000 + static_cast<int>(rnd(3000));
        for (int k = 0; k < n; ++k) {
            // Gaps of every size, 0 among them; values from a few, so that
            // some repeat and some go back at the same time.
            const u64 gaps[] = {0, 1, 2, 127, 128, 300, 16383, 16384, 1ull << 40};
            t += gaps[rnd(rnd(4) ? 4 : std::size(gaps))];
            std::vector<u64> val(words), xz(words);
            std::string text(width, '0');
            const u64 pick = rnd(6);
            for (u32 b = 0; b < width; ++b) {
                const u32 s = pick == 5 && k > n / 2 ? static_cast<u32>(rnd(4)) : static_cast<u32>((pick >> (b % 3)) & 1);
                val[b / 64] |= static_cast<u64>(s & 1) << (b % 64);
                xz[b / 64] |= static_cast<u64>(s >> 1) << (b % 64);
                text[width - 1 - b] = "01zx"[s];
            }
            l.add_bits(t, val.data(), xz.data());
            if (!ref.empty() && ref.back().v == text) continue;
            if (!ref.empty() && ref.back().t == t) {
                if (ref.size() >= 2 && ref[ref.size() - 2].v == text) ref.pop_back();
                else ref.back().v = text;
                continue;
            }
            ref.push_back({t, text});
        }
        const std::string at = "lane seed " + std::to_string(seed) + " width " + std::to_string(width);
        check(l.size() == ref.size(), at + ": " + std::to_string(l.size()) + " changes, " + std::to_string(ref.size()) + " expected");
        for (u64 i = 0; i < std::min<u64>(l.size(), ref.size()); ++i)
            if (l.time(i) != ref[i].t || fmt(l, i) != ref[i].v) {
                check(false, at + ": change " + std::to_string(i));
                break;
            }
        for (int q = 0; q < 200 && !ref.empty(); ++q) {
            const u64 x = rnd(2) ? ref[rnd(ref.size())].t + rnd(3) - 1 : rnd(t + 10);
            const u64 before = static_cast<u64>(std::lower_bound(ref.begin(), ref.end(), x, [](const RefChange& c, u64 v) { return c.t < v; }) - ref.begin());
            check(l.count_before(x) == before, at + ": count before " + std::to_string(x));
        }
        // Columns: anywhere, the whole lane, and narrow ones about where
        // one block ends and the next starts, each against a count of the list.
        auto before = [&](u64 x) {
            return static_cast<u64>(std::lower_bound(ref.begin(), ref.end(), x, [](const RefChange& c, u64 v) { return c.t < v; }) - ref.begin());
        };
        for (int q = 0; q < 30 && !ref.empty(); ++q) {
            u64 t0 = ref[rnd(ref.size())].t, t1 = t0 + rnd(t - t0 + 2);
            if (q == 0) t0 = 0, t1 = t + 1;
            if (q % 3 == 1 && ref.size() > Lane::kBlock) {
                const u64 b = Lane::kBlock * (1 + rnd((ref.size() - 1) / Lane::kBlock));
                t0 = ref[b - 1].t - std::min(ref[b - 1].t, rnd(4)), t1 = ref[b].t + 1 + rnd(4);
            }
            std::vector<LaneColumn> cols(q % 2 ? 2000 : 1 + rnd(50));
            l.columns(t0, t1, cols);
            for (u64 k = 0; k < cols.size(); ++k) {
                const u64 a = t0 + static_cast<u64>(static_cast<unsigned __int128>(t1 - t0) * k / cols.size());
                const u64 b = t0 + static_cast<u64>(static_cast<unsigned __int128>(t1 - t0) * (k + 1) / cols.size());
                if (cols[k].first != before(a) || cols[k].count != before(b) - before(a)) {
                    check(false, at + ": column " + std::to_string(k) + " of " + std::to_string(cols.size()) + " over [" + std::to_string(t0) + ", " +
                                     std::to_string(t1) + ")");
                    break;
                }
            }
        }
    }
    // Events: one a time, every one kept; reals and strings.
    Lane e(LaneKind::kEvent, 1);
    for (const u64 t : {5, 5, 9, 9, 12}) e.add_event(t);
    check(e.size() == 3 && e.time(2) == 12, "lane: events, one a time");
    Lane r(LaneKind::kReal, 64);
    for (const double d : {1.5, 1.5, 2.0}) r.add_real(3, d);
    r.add_real(7, 2.0);
    r.add_real(8, -0.25);
    check(r.size() == 2 && r.real(0) == 2.0 && r.real(1) == -0.25 && fmt(r, 1) == "-0.25", "lane: reals");
    Lane s(LaneKind::kText, 0);
    s.add_text(1, "IDLE");
    s.add_text(2, "RUN");
    s.add_text(2, "IDLE");  // (back to what it was, at the same time: no change)
    s.add_text(4, "RUN");
    check(s.size() == 2 && s.text(0) == "IDLE" && s.text(1) == "RUN" && s.time(1) == 4, "lane: strings");
    Lane empty(LaneKind::kBits, 8);
    std::vector<LaneColumn> cols(10);
    empty.columns(0, 100, cols);
    u64 t = 0;
    check(empty.at(5) == ~0ull && !empty.next_change(0, &t) && cols[9].first == 0 && cols[9].count == 0, "lane: an empty one");
}

int run_check(const std::string&, int seeds) {
    lane_test(seeds);
    if (failures) return 1;
    std::printf("timeline ok: %d seeds\n", seeds);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "check" && argc > 2) return run_check(argv[2], argc > 3 ? std::atoi(argv[3]) : 200);
    std::fprintf(stderr, "usage: blit_timeline_test check DIR [SEEDS]\n");
    return 2;
}
