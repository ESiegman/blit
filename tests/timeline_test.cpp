// The timeline model and the VCD, FST and ILA CSV readers (src/timeline.hpp,
// src/wave.hpp), checked against real dumps and captures (tests/wave: what
// each was made with is beside it) and against slow answers worked out from
// the same text by brute force. tests/wave.sh runs it.
//
//   blit_timeline_test check DIR [SEEDS [SCRATCH]]  every check, on the dumps in DIR (files it writes go in SCRATCH)
//   blit_timeline_test load FILE [MAX_MB]  reads a dump (VCD or FST) as blit does, on a thread, and says what came of it
//   blit_timeline_test gen FILE MB         writes a made-up dump of about MB megabytes (for speed and memory)
//   blit_timeline_test genfst FILE M       writes an FST of about M million changes with libfst's writer
//   blit_timeline_test bench FILE          how fast a dump is read, its memory a change, and the column query's cost
#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include "timeline.hpp"
#include "wave.hpp"

extern "C" {
#include "fstapi.h"
}

using namespace blit;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    if (++failures > 40) std::exit(1);
}

std::string slurp(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[1 << 16];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "FAIL: cannot read %s\n", path.c_str());
        std::exit(1);
    }
    return s;
}

// A dump read whole, in one piece or in the pieces given.
std::unique_ptr<Wave> read_vcd(std::string_view text, std::vector<size_t> cuts = {}, u64 max_bytes = 0) {
    auto w = std::make_unique<Wave>();
    VcdReader r(w.get(), max_bytes);
    size_t at = 0;
    cuts.push_back(text.size());
    for (const size_t c : cuts) {
        if (c < at) continue;
        r.feed(text.substr(at, c - at));
        at = c;
    }
    r.finish();
    return w;
}

std::string fmt(const Lane& l, u64 i) {
    std::string s;
    l.format(i, &s);
    return s;
}

// --- The slow answer: the dump's text split at spaces, each variable's
// changes as (time, value as text), by the same rules (a value the
// variable has is no change, except an event's; at one time the last
// value counts, and none if it is the one from before).

struct RefChange {
    u64 t;
    std::string v;
};

struct RefVar {
    char kind = 'b';  // b bits, r real, s string, e event
    u32 width = 1;
    std::vector<RefChange> changes;
};

std::string real_text(double d) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, d);
    return std::string(buf, r.ptr);
}

std::map<std::string, RefVar> reference(std::string_view text) {
    std::vector<std::string_view> tok;
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && std::strchr(" \t\r\n\v\f", text[i]) && text[i]) ++i;
        const size_t s = i;
        while (i < text.size() && !(std::strchr(" \t\r\n\v\f", text[i]) && text[i])) ++i;
        if (i > s) tok.push_back(text.substr(s, i - s));
    }
    std::map<std::string, RefVar> vars;
    size_t i = 0;
    for (; i < tok.size() && tok[i] != "$enddefinitions"; ++i) {
        if (tok[i] != "$var") {
            if (tok[i] == "$comment" || tok[i] == "$date" || tok[i] == "$version")
                while (i < tok.size() && tok[i] != "$end") ++i;
            continue;
        }
        const std::string_view kind = tok[i + 1];
        const std::string code(tok[i + 3]);
        if (!vars.count(code)) {
            RefVar& v = vars[code];
            v.kind = kind == "real" ? 'r' : kind == "string" ? 's' : kind == "event" ? 'e' : 'b';
            v.width = static_cast<u32>(std::atoi(std::string(tok[i + 2]).c_str()));
            if (v.kind == 'r') v.width = 64;
        }
        while (i < tok.size() && tok[i] != "$end") ++i;
    }
    ++i;  // ($end)
    u64 now = 0;
    auto set = [&](const std::string& code, std::string v) {
        RefVar& r = vars.at(code);
        auto& c = r.changes;
        if (r.kind == 'e') {
            if (c.empty() || c.back().t != now) c.push_back({now, ""});
            return;
        }
        if (r.kind == 'b') {
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (v.size() > r.width) v = v.substr(v.size() - r.width);
            if (v.size() < r.width) v = std::string(r.width - v.size(), v[0] == 'x' || v[0] == 'z' ? v[0] : '0') + v;
        }
        if (!c.empty() && c.back().v == v) return;
        if (!c.empty() && c.back().t == now) {
            if (c.size() >= 2 && c[c.size() - 2].v == v) c.pop_back();
            else c.back().v = v;
            return;
        }
        c.push_back({now, v});
    };
    for (; i < tok.size(); ++i) {
        const std::string_view t = tok[i];
        if (t == "$comment") {
            while (i < tok.size() && tok[i] != "$end") ++i;
        } else if (t[0] == '$') {
        } else if (t[0] == '#') {
            now = std::max<u64>(now, std::strtoull(std::string(t.substr(1)).c_str(), nullptr, 10));
        } else if (t[0] == 'b' || t[0] == 'r' || t[0] == 's') {
            std::string v(t.substr(1));
            if (t[0] == 'r') v = real_text(std::strtod(v.c_str(), nullptr));
            set(std::string(tok[++i]), v);
        } else {
            set(std::string(t.substr(1)), std::string(1, t[0]));
        }
    }
    return vars;
}

// --- What a dump declares, a line a variable: its path, kind, width and
// range, and which of the variables before it it shares a lane with.
std::string listing(const Wave& w) {
    std::string s;
    for (u32 v = 0; v < w.vars.size(); ++v) {
        const WaveVar& x = w.vars[v];
        s += w.path(v) + " " + std::string(var_kind_name(x.kind)) + " " + std::to_string(x.width);
        if (x.ranged) s += " [" + std::to_string(x.msb) + ":" + std::to_string(x.lsb) + "]";
        for (u32 o = 0; o < v; ++o)
            if (w.vars[o].lane == x.lane) {
                s += " = " + w.path(o);
                break;
            }
        s += "\n";
    }
    return s;
}

// Every lane against the slow answer.
void compare(const Wave& w, const std::map<std::string, RefVar>& ref, const std::string& name) {
    for (const WaveVar& v : w.vars) {
        const auto it = ref.find(v.code);
        if (it == ref.end()) {
            check(false, name + ": " + v.name + " not in the reference");
            continue;
        }
        const Lane& l = w.lanes[v.lane];
        const auto& c = it->second.changes;
        check(l.size() == c.size(), name + ": " + v.name + ": " + std::to_string(l.size()) + " changes, the reference has " + std::to_string(c.size()));
        for (u64 i = 0; i < std::min<u64>(l.size(), c.size()); ++i) {
            if (l.time(i) != c[i].t || fmt(l, i) != c[i].v) {
                check(false, name + ": " + v.name + " change " + std::to_string(i) + ": " + std::to_string(l.time(i)) + " " + fmt(l, i) +
                                 ", the reference " + std::to_string(c[i].t) + " " + c[i].v);
                break;
            }
        }
    }
}

// The answers two readings of the same dump give are the same.
bool same_wave(const Wave& a, const Wave& b) {
    if (listing(a) != listing(b) || a.lanes.size() != b.lanes.size() || a.start != b.start || a.end != b.end || a.off != b.off ||
        a.unit_mult != b.unit_mult || a.unit_exp != b.unit_exp || a.notes != b.notes || a.error != b.error)
        return false;
    for (size_t k = 0; k < a.lanes.size(); ++k) {
        const Lane &x = a.lanes[k], &y = b.lanes[k];
        if (x.size() != y.size() || x.kind() != y.kind()) return false;
        for (u64 i = 0; i < x.size(); ++i)
            if (x.time(i) != y.time(i) || fmt(x, i) != fmt(y, i)) return false;
    }
    return true;
}

// Random times against the slow answer: counts, the value in force, the
// next and previous change, and columns.
void queries(const Wave& w, const std::map<std::string, RefVar>& ref, const std::string& name, int seeds) {
    for (int seed = 1; seed <= seeds; ++seed) {
        std::mt19937_64 rng(static_cast<u64>(seed) * 7919);
        auto rnd = [&](u64 n) { return n ? rng() % n : 0; };
        const WaveVar& v = w.vars[rnd(w.vars.size())];
        const Lane& l = w.lanes[v.lane];
        const auto& c = ref.at(v.code).changes;
        const u64 lo = w.start > 3 ? w.start - 3 : 0, hi = w.end + 3;
        const std::string at = name + " seed " + std::to_string(seed) + " " + v.name;
        for (int q = 0; q < 20; ++q) {
            // (times at the changes themselves half the time)
            const u64 t = !c.empty() && rnd(2) ? c[rnd(c.size())].t + rnd(3) - 1 : lo + rnd(hi - lo + 1);
            u64 before = 0, through = 0;
            for (const RefChange& x : c) before += x.t < t, through += x.t <= t;
            check(l.count_before(t) == before && l.count_through(t) == through, at + ": counts at " + std::to_string(t));
            const u64 i = l.at(t);
            check(through ? i == through - 1 && fmt(l, i) == c[through - 1].v : i == ~0ull, at + ": value at " + std::to_string(t));
            u64 nx = 0, pv = 0;
            const bool hn = l.next_change(t, &nx), hp = l.prev_change(t, &pv);
            check(hn == (through < c.size()) && (!hn || nx == c[through].t), at + ": next change after " + std::to_string(t));
            check(hp == (before > 0) && (!hp || pv == c[before - 1].t), at + ": previous change before " + std::to_string(t));
        }
        for (int q = 0; q < 6; ++q) {
            u64 t0 = lo + rnd(hi - lo + 1), t1 = lo + rnd(hi - lo + 1);
            if (t1 < t0) std::swap(t0, t1);
            if (q == 0) t0 = lo, t1 = hi;  // (the whole dump, as 0 shows it)
            const u64 n = 1 + rnd(q == 1 ? 3000 : 300);
            std::vector<LaneColumn> cols(n);
            l.columns(t0, t1, cols);
            bool ok = true;
            for (u64 k = 0; k < n && ok; ++k) {
                const u64 a = t0 + static_cast<u64>(static_cast<unsigned __int128>(t1 - t0) * k / n);
                const u64 b = t0 + static_cast<u64>(static_cast<unsigned __int128>(t1 - t0) * (k + 1) / n);
                u64 first = 0, count = 0;
                for (const RefChange& x : c) first += x.t < a, count += x.t >= a && x.t < b;
                ok = cols[k].first == first && cols[k].count == count;
                if (!ok)
                    check(false, at + ": column " + std::to_string(k) + " of " + std::to_string(n) + " over [" + std::to_string(t0) + ", " +
                                     std::to_string(t1) + "): " + std::to_string(cols[k].first) + "+" + std::to_string(cols[k].count) + ", the reference " +
                                     std::to_string(first) + "+" + std::to_string(count));
            }
        }
    }
}

// The value of the variable at a path, at a time, as text ("-" before its first change).
std::string value_at(const Wave& w, std::string_view path, u64 t) {
    const i64 v = w.find(path);
    if (v < 0) return "(no " + std::string(path) + ")";
    const Lane& l = w.lanes[w.vars[static_cast<size_t>(v)].lane];
    const u64 i = l.at(t);
    return i == ~0ull ? "-" : fmt(l, i);
}

u64 changes_of(const Wave& w, std::string_view path) {
    const i64 v = w.find(path);
    return v < 0 ? ~0ull : w.lanes[w.vars[static_cast<size_t>(v)].lane].size();
}

void expect(const std::string& what, const std::string& got, const std::string& want) {
    check(got == want, what + ": got \"" + got + "\", want \"" + want + "\"");
}

// --- What each real dump declares and holds, as read from its text by hand.

void icarus_test(const Wave& w) {
    expect("icarus: the variables", listing(w),
           "tb.done event 1\n"
           "tb.wrap wire 1\n"
           "tb.drv wire 4 [3:0]\n"
           "tb.count wire 8 [7:0]\n"
           "tb.clk reg 1\n"
           "tb.en reg 1\n"
           "tb.nib reg 4 [3:0]\n"
           "tb.rst reg 1\n"
           "tb.wide reg 96 [95:0]\n"
           "tb.steps integer 32 [31:0]\n"
           "tb.temp real 1\n"
           "tb.u_cnt.clk wire 1 = tb.clk\n"
           "tb.u_cnt.en wire 1 = tb.en\n"
           "tb.u_cnt.rst wire 1 = tb.rst\n"
           "tb.u_cnt.wrap wire 1 = tb.wrap\n"
           "tb.u_cnt.W parameter 32\n"
           "tb.u_cnt.count reg 8 [7:0]\n");
    check(w.lanes.size() == 13 && w.scopes.size() == 3 && w.scopes[2].kind == ScopeKind::kModule && w.scopes[2].parent == 1, "icarus: lanes and scopes");
    check(w.unit_given && w.unit_mult == 1 && w.unit_exp == -12, "icarus: 1ps over three lines");
    check(w.start == 0 && w.end == 2772000, "icarus: from 0 to the last #time, with no change at it");
    check(w.off == std::vector<std::pair<u64, u64>>{{72000, 97000}}, "icarus: the $dumpoff stretch");
    check(w.date.find("2026") != std::string::npos && w.version == "Icarus Verilog", "icarus: $date and $version");
    check(w.notes.empty() && w.error.empty(), "icarus: read with nothing to say");
    expect("icarus: nib before it is set", value_at(w, "tb.nib", 31999), "xxxx");
    expect("icarus: nib with x and z", value_at(w, "tb.nib", 32000), "1x0z");
    expect("icarus: nib", value_at(w, "tb.nib", 62000), "1100");
    expect("icarus: nib while dumping is off", value_at(w, "tb.nib", 80000), "xxxx");
    expect("icarus: nib when dumping is on again", value_at(w, "tb.nib", 97000), "1100");
    expect("icarus: nib all z", value_at(w, "tb.nib", 160000), "zzzz");
    expect("icarus: drv floats at first (bz, widened with z)", value_at(w, "tb.drv", 0), "zzzz");
    expect("icarus: drv driven", value_at(w, "tb.drv", 12000), "0000");
    expect("icarus: a real", value_at(w, "tb.temp", 32000), "1.5");
    expect("icarus: a small real", value_at(w, "tb.temp", 70000), "-0.00225");
    expect("icarus: a real while dumping is off (rNaN)", value_at(w, "tb.temp", 80000), "nan");
    expect("icarus: a large real", value_at(w, "tb.temp", 2000000), "6.02e+23");
    expect("icarus: an integer", value_at(w, "tb.steps", 140000), "11111111111111111111111111111001");
    expect("icarus: a parameter (from $dumpall, not listed by $dumpoff)", value_at(w, "tb.u_cnt.W", 80000), "00000000000000000000000000001000");
    expect("icarus: 96 bits with x and z", value_at(w, "tb.wide", 40000), "10xz" + std::string(92, '0'));
    expect("icarus: 96 bits", value_at(w, "tb.wide", 62000),
           "110111101010110110111110111011110000000100100011010001010110011110001001101010111100110111101111");
    expect("icarus: 96 bits while dumping is off", value_at(w, "tb.wide", 80000), std::string(96, 'x'));
    expect("icarus: an alias reads its lane", value_at(w, "tb.u_cnt.clk", 5000), "1");
    expect("icarus: the counter", value_at(w, "tb.count", 2757000), "00010001");
    // (The event: at #0 in $dumpvars, again in $dumpon, and when it fires.)
    check(changes_of(w, "tb.done") == 3, "icarus: an event's changes, each kept");
    const Lane& clk = w.lanes[w.vars[static_cast<size_t>(w.find("tb.clk"))].lane];
    u64 t = 0;
    check(clk.next_change(5000, &t) && t == 10000 && clk.prev_change(5000, &t) && t == 0, "icarus: next and previous change of the clock");
    check(clk.next_change(72000, &t) && t == 97000, "icarus: the clock's next change after $dumpoff is at $dumpon");
}

void verilator_test(const Wave& w) {
    expect("verilator: the variables", listing(w),
           "tb.clk wire 1\n"
           "tb.rst wire 1\n"
           "tb.en wire 1\n"
           "tb.count wire 8 [7:0]\n"
           "tb.wrap wire 1\n"
           "tb.nib wire 4 [3:0]\n"
           "tb.drv wire 4 [3:0]\n"
           "tb.wide wire 96 [95:0]\n"
           "tb.temp real 64\n"
           "tb.steps wire 32 [31:0]\n"
           "tb.done event 1\n"
           "tb.u_cnt.W wire 32 [31:0]\n"
           "tb.u_cnt.clk wire 1 = tb.clk\n"
           "tb.u_cnt.rst wire 1 = tb.rst\n"
           "tb.u_cnt.en wire 1 = tb.en\n"
           "tb.u_cnt.count wire 8 [7:0] = tb.count\n"
           "tb.u_cnt.wrap wire 1 = tb.wrap\n");
    check(w.lanes.size() == 12 && w.unit_exp == -12 && w.start == 0 && w.end == 2772000 && w.off.empty(), "verilator: lanes, unit, span");
    check(w.notes.empty() && w.error.empty(), "verilator: read with nothing to say");
    // (Two states only: 4'b1x0z is 1000.)
    expect("verilator: nib", value_at(w, "tb.nib", 32000), "1000");
    expect("verilator: a real", value_at(w, "tb.temp", 32000), "1.5");
    expect("verilator: a large real", value_at(w, "tb.temp", 2000000), "6.02e+23");
    expect("verilator: 96 bits", value_at(w, "tb.wide", 70000),
           "110111101010110110111110111011110000000100100011010001010110011110001001101010111100110111101111");
    expect("verilator: a 32-bit parameter", value_at(w, "tb.u_cnt.W", 100), "00000000000000000000000000001000");
    check(changes_of(w, "tb.done") == 1, "verilator: the event fires once");
    expect("verilator: the counter", value_at(w, "tb.u_cnt.count", 2757000), "00010001");
}

void yosys_test(const Wave& w) {
    expect("yosys: the variables", listing(w),
           "counter.wrap wire 1\n"
           "counter.rst wire 1\n"
           "counter.en wire 1\n"
           "counter.count reg 8 [7:0]\n"
           "counter.clk wire 1\n");
    check(!w.unit_given && w.end == 242 && w.lanes.size() == 5 && w.notes.empty(), "yosys: no $timescale, codes of two letters");
    expect("yosys: one bit as a vector (b1)", value_at(w, "counter.rst", 5), "1");
    expect("yosys: count with one x", value_at(w, "counter.count", 20), "0000000x");
    expect("yosys: count all x", value_at(w, "counter.count", 30), "xxxxxxxx");
    expect("yosys: en never driven", value_at(w, "counter.en", 200), "x");
}

// Vivado 2026.2's ILA capture of tests/wave/make/ila_top.v on a Basys 3, in
// its VCD form (tests/wave/ila.txt): sample numbers as times, its own
// _TRIGGER, _WINDOW and _GAP, the trigger at sample 100 (count 0x40).
void ila_vcd_test(const Wave& w) {
    expect("ila_one.vcd: the variables", listing(w),
           "dut.count reg 8 [7:0]\n"
           "dut.wrap reg 1\n"
           "dut.phase reg 2 [1:0]\n"
           "dut.sum reg 12 [11:0]\n"
           "dut.sw_q reg 4 [3:0]\n"
           "dut._TRIGGER reg 1\n"
           "dut._WINDOW reg 1\n"
           "dut._GAP reg 1\n");
    check(w.version == "Vivado v2026.2 (64-bit)" && w.unit_given && w.unit_exp == -12 && w.samples, "ila_one.vcd: Vivado's, its times samples");
    check(w.start == 0 && w.end == 1023 && w.notes.empty() && w.error.empty(), "ila_one.vcd: 1024 samples, read with nothing to say");
    check(w.triggers == std::vector<u64>{100} && w.windows.empty(), "ila_one.vcd: the trigger at sample 100, one window");
    expect("ila_one.vcd: count at the trigger", value_at(w, "dut.count", 100), "01000000");
    expect("ila_one.vcd: sum a cycle behind", value_at(w, "dut.sum", 100), "000000111111");
    expect("ila_one.vcd: count at the start", value_at(w, "dut.count", 0), "11011100");
    check(w.time_text(100) == "100" && w.time_text(0) == "0", "ila_one.vcd: a sample's time as text is its number");
}

void pyvcd_test(const Wave& w) {
    expect("pyvcd: the variables", listing(w),
           "top.clk wire 1\n"
           "top.uart.tx reg 1\n"
           "top.uart.state string 1\n"
           "top.uart.volts real 64\n"
           "top.uart.sent event 1\n"
           "top.uart.fifo.data reg 16\n"
           "top.uart.fifo.level integer 8\n"
           "top.uart.fifo.bus_alias reg 16 = top.uart.fifo.data\n");
    check(w.scopes.size() == 4 && w.scopes[2].kind == ScopeKind::kTask && w.scopes[3].kind == ScopeKind::kFork && w.scopes[3].name == "fifo",
          "pyvcd: task and fork scopes");
    check(w.unit_mult == 10 && w.unit_exp == -9 && w.date == "today" && w.notes.empty(), "pyvcd: 10 ns, $date");
    check(w.off == std::vector<std::pair<u64, u64>>{{44, 51}}, "pyvcd: the $dumpoff stretch");
    expect("pyvcd: a string", value_at(w, "top.uart.state", 0), "IDLE");
    expect("pyvcd: a string changed", value_at(w, "top.uart.state", 6), "DATA0");
    expect("pyvcd: a real", value_at(w, "top.uart.volts", 2), "0.12");
    expect("pyvcd: 16 bits of z (bz)", value_at(w, "top.uart.fifo.data", 0), std::string(16, 'z'));
    expect("pyvcd: 16 bits", value_at(w, "top.uart.fifo.data", 2), "0000000001010101");
    expect("pyvcd: 16 bits of x", value_at(w, "top.uart.fifo.data", 40), std::string(16, 'x'));
    expect("pyvcd: the alias", value_at(w, "top.uart.fifo.bus_alias", 2), "0000000001010101");
    expect("pyvcd: clock x while dumping is off", value_at(w, "top.clk", 47), "x");
    expect("pyvcd: a string while dumping is off, as it was", value_at(w, "top.uart.state", 47), "IDLE");
    check(changes_of(w, "top.uart.sent") == 2, "pyvcd: an event a byte");
    check(w.time_text(5) == "50 ns" && w.time_text(150) == "1.5 us" && w.time_text(0) == "0 ns", "pyvcd: times as text in 10 ns");
}

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

// Fed in two pieces at every place it can be cut, and in bytes one at a
// time: the same as in one piece.
void pieces_test(const std::string& text, const Wave& whole, const std::string& name, size_t step) {
    for (size_t cut = 0; cut <= text.size(); cut += step) {
        const auto w = read_vcd(text, {cut});
        if (!same_wave(*w, whole)) {
            check(false, name + ": cut in two at byte " + std::to_string(cut));
            return;
        }
    }
    std::vector<size_t> bytes(text.size());
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = i;
    check(same_wave(*read_vcd(text, bytes), whole), name + ": fed a byte at a time");
}

// Cut short anywhere: what was read is what the whole dump has, up to the
// last time it reached.
void cut_short_test(const std::string& text, const Wave& whole, const std::string& name) {
    std::mt19937_64 rng(99);
    for (int k = 0; k < 300; ++k) {
        const size_t len = rng() % (text.size() + 1);
        const auto w = read_vcd(std::string_view(text).substr(0, len));
        check(w->done, name + ": cut at " + std::to_string(len) + ": done");
        if (!w->header) continue;
        bool ok = w->lanes.size() <= whole.lanes.size();
        for (size_t i = 0; ok && i < w->lanes.size(); ++i) {
            const Lane &a = w->lanes[i], &b = whole.lanes[i];
            for (u64 j = 0; ok && j < a.size() && a.time(j) < w->end; ++j) ok = j < b.size() && a.time(j) == b.time(j) && fmt(a, j) == fmt(b, j);
        }
        check(ok, name + ": cut at " + std::to_string(len) + ": the changes before its end are the whole dump's");
    }
}

// What IEEE 1364-2005 allows that the real dumps here do not show, written
// from its grammar (18.2.1), its value formats (18.2.2) and its keyword
// commands (18.2.3): a timescale in two words, task, function, begin and
// fork scopes, a range written onto the name, a bit select, codes of
// several characters, values shorter than the variable (widened with 0, x
// or z by their first digit), capital X and Z, $dumpall, $dumpoff and
// $dumpon listing values, a $comment among the values.
void spec_test() {
    const auto w = read_vcd(
        "$date\n   today\n$end\n$version\n   by hand\n$end\n$timescale\n   10 us\n$end\n"
        "$scope module top $end\n$scope task t $end\n$var reg 32 (k acc[31:0] $end\n$var integer 32 {2 index $end\n$upscope $end\n"
        "$scope function f $end\n$var trireg 1 *@ net $end\n$upscope $end\n$scope begin blk $end\n$var wire 8 # data [3] $end\n$upscope $end\n"
        "$scope fork par $end\n$var reg 1 *@ alias $end\n$upscope $end\n$upscope $end\n$enddefinitions $end\n"
        "$comment the values start at 500 $end\n#500\n$dumpvars\nX*@\nbx (k\nbX {2\nb1 #\n$end\n"
        "#505\n0*@\nb10zx1 (k\nbz1 {2\n$comment a remark $end\n#535\n$dumpall\n0*@\nb10zx1 (k\nbz1 {2\n$end\n"
        "#1000\n$dumpoff\nx*@\nbx (k\nbx {2\nbx #\n$end\n#2000\n$dumpon\nZ*@\nb0 (k\nb1 {2\nb11 #\n$end\n#2010\n1*@\n");
    expect("spec: the variables", listing(*w),
           "top.t.acc reg 32 [31:0]\ntop.t.index integer 32\ntop.f.net trireg 1\ntop.blk.data wire 8 [3:3]\ntop.par.alias reg 1 = top.f.net\n");
    check(w->scopes[2].kind == ScopeKind::kTask && w->scopes[3].kind == ScopeKind::kFunction && w->scopes[4].kind == ScopeKind::kBegin &&
              w->scopes[5].kind == ScopeKind::kFork,
          "spec: scope kinds");
    check(w->unit_mult == 10 && w->unit_exp == -6 && w->date == "today" && w->version == "by hand" && w->start == 500 && w->end == 2010 && w->notes.empty(),
          "spec: header and span");
    check(w->off == std::vector<std::pair<u64, u64>>{{1000, 2000}}, "spec: $dumpoff to $dumpon");
    expect("spec: X", value_at(*w, "top.f.net", 500), "x");
    expect("spec: x widened", value_at(*w, "top.t.acc", 500), std::string(32, 'x'));
    expect("spec: X widened", value_at(*w, "top.t.index", 500), std::string(32, 'x'));
    expect("spec: 1 widened with 0", value_at(*w, "top.blk.data", 500), "00000001");
    expect("spec: 10zx1 widened with 0", value_at(*w, "top.t.acc", 505), std::string(27, '0') + "10zx1");
    expect("spec: z1 widened with z", value_at(*w, "top.t.index", 505), std::string(31, 'z') + "1");
    check(changes_of(*w, "top.t.acc") == 4, "spec: $dumpall's values are no changes");
    expect("spec: off", value_at(*w, "top.blk.data", 1500), "xxxxxxxx");
    expect("spec: Z when on again", value_at(*w, "top.par.alias", 2000), "z");
    expect("spec: on again", value_at(*w, "top.blk.data", 2005), "00000011");
    expect("spec: last", value_at(*w, "top.f.net", 3000), "1");
    expect("spec: before the first", value_at(*w, "top.f.net", 499), "-");
}

// Not quite VCD, or not VCD at all: said, not a crash.
void malformed_test() {
    {
        const auto w = read_vcd("");
        check(w->done && w->error.find("not a VCD dump") != std::string::npos, "malformed: empty");
    }
    {
        const auto w = read_vcd("hello world\nthis is text\n");
        check(w->error.find("not a VCD dump") != std::string::npos, "malformed: plain text");
    }
    {
        // (IEEE 1364-2005 18.2.1's syntax broken: a $var short of words, a
        // time going back, a value for an undeclared code, a time that is
        // not a number, a vector of letters, a value with no code at the end.)
        const auto w = read_vcd("$timescale 3 parsecs $end\n$scope module m $end\n$var wire $end\n$var wire 1 ! a $end\n$var wire 4 \" b [3:0] $end\n"
                                "$upscope $end\n$upscope $end\n$enddefinitions $end\n#10\n1!\n#5\n0!\n1?\n#abc\nbqq \"\nb1");
        const std::string all = [&] {
            std::string s;
            for (const std::string& n : w->notes) s += n + "\n";
            return s;
        }();
        check(w->error.empty() && w->vars.size() == 2, "malformed: read on");
        for (const char* n : {"$timescale", "too few words", "before the one before", "not declared", "not a time", "not a vector value",
                              "no identifier code", "$upscope with no $scope"})
            check(all.find(n) != std::string::npos, std::string("malformed: said: ") + n + "\n" + all);
        check(all.find("line 11: a time before") != std::string::npos, "malformed: notes say the line\n" + all);
        const Lane& a = w->lanes[0];
        check(a.size() == 1 && a.time(0) == 10 && fmt(a, 0) == "0", "malformed: a time going back is taken as the last one");
    }
    {
        // No $enddefinitions; a $var not ended; a comment in the values.
        const auto w = read_vcd("$var wire 1 ! a\n$var wire 1 \" b $end\n#0\n1!\n$comment 1\" b $end\n0\"\n#3\n0!\n");
        check(w->vars.size() == 2 && w->lanes[0].size() == 2 && w->lanes[1].size() == 1 && fmt(w->lanes[1], 0) == "0", "malformed: no $enddefinitions");
    }
    {
        std::string many = "$var wire 1 ! a $end $enddefinitions $end\n";
        for (int i = 0; i < 200; ++i) many += "#x\n";
        const auto w = read_vcd(many);
        check(w->notes.size() == 51 && w->notes.back() == "and 150 more", "malformed: notes stop at fifty");
    }
}

void made_up(u64 bytes, const std::function<void(std::string_view)>& put);
void made_up_fst(const std::string& path, u64 changes);

// --- ILA captures' CSV form (tests/wave/ila.txt says how they were made).

std::unique_ptr<Wave> read_csv(std::string_view text, std::vector<size_t> cuts = {}, u64 max_bytes = 0) {
    auto w = std::make_unique<Wave>();
    IlaCsvReader r(w.get(), max_bytes);
    size_t at = 0;
    cuts.push_back(text.size());
    for (const size_t c : cuts) {
        if (c < at) continue;
        r.feed(text.substr(at, c - at));
        at = c;
    }
    r.finish();
    return w;
}

// A capture's probes as two readings have them (its VCD and its CSV): the
// same names, widths and changes, and the same triggers and windows.
bool same_capture(const Wave& vcd, const Wave& csv, std::string* why) {
    if (vcd.triggers != csv.triggers || vcd.windows != csv.windows || vcd.start != csv.start || vcd.end != csv.end) return *why = "triggers, windows or span", false;
    for (u32 v = 0; v < csv.vars.size(); ++v) {
        const i64 o = vcd.find(csv.path(v));
        if (o < 0) return *why = "no " + csv.path(v) + " in the VCD", false;
        const WaveVar &a = vcd.vars[static_cast<size_t>(o)], &b = csv.vars[v];
        if (a.width != b.width || a.ranged != b.ranged || a.msb != b.msb || a.lsb != b.lsb) return *why = b.name + ": its width", false;
        const Lane &x = vcd.lanes[a.lane], &y = csv.lanes[b.lane];
        if (x.size() != y.size()) return *why = b.name + ": " + std::to_string(x.size()) + " changes, " + std::to_string(y.size()), false;
        for (u64 i = 0; i < x.size(); ++i)
            if (x.time(i) != y.time(i) || fmt(x, i) != fmt(y, i)) return *why = b.name + ": change " + std::to_string(i), false;
    }
    return true;
}

// The CSV text with line n (from 1) replaced.
std::string with_line(const std::string& text, size_t n, const std::string& to) {
    size_t a = 0;
    for (size_t k = 1; k < n; ++k) a = text.find('\n', a) + 1;
    const size_t b = text.find('\n', a);
    return text.substr(0, a) + to + "\n" + text.substr(b + 1);
}

std::string line_of(const std::string& text, size_t n) {
    size_t a = 0;
    for (size_t k = 1; k < n; ++k) a = text.find('\n', a) + 1;
    return text.substr(a, text.find('\n', a) - a);
}

void ila_test(const std::string& dir) {
    // ila_one: one window, the trigger at sample 100; sum unsigned, sw_q binary, the rest hex.
    const std::string one = slurp(dir + "/ila_one.csv");
    const auto w = read_csv(one);
    expect("ila_one.csv: the probes", listing(*w), "dut.count reg 8 [7:0]\ndut.wrap reg 1\ndut.phase reg 2 [1:0]\ndut.sum reg 12 [11:0]\ndut.sw_q reg 4 [3:0]\n");
    check(w->done && w->error.empty() && w->samples && w->start == 0 && w->end == 1023 && w->triggers == std::vector<u64>{100} && w->windows.empty(),
          "ila_one.csv: 1024 samples, the trigger at 100: " + w->error);
    check(w->vars[0].radix == DumpRadix::kHex && w->vars[1].radix == DumpRadix::kHex && w->vars[3].radix == DumpRadix::kUnsigned && w->vars[4].radix == DumpRadix::kBinary,
          "ila_one.csv: the radix row");
    expect("ila_one.csv: count at the trigger (hex 40)", value_at(*w, "dut.count", 100), "01000000");
    expect("ila_one.csv: sum there (unsigned 63)", value_at(*w, "dut.sum", 100), "000000111111");
    expect("ila_one.csv: sw_q (binary 0000)", value_at(*w, "dut.sw_q", 100), "0000");
    std::string why;
    const auto vcd = read_vcd(slurp(dir + "/ila_one.vcd"));
    check(same_capture(*vcd, *w, &why), "ila_one: its CSV as its VCD: " + why);
    check(dump_kind(one) == DumpKind::kIlaCsv && dump_kind(slurp(dir + "/ila_one.vcd")) == DumpKind::kVcd, "ila_one: the CSV form, from its first bytes");
    // In pieces split anywhere, with CRLF line ends.
    for (size_t cut = 0; cut <= one.size(); cut += 11)
        if (const auto p = read_csv(one, {cut}); !same_wave(*p, *w) || p->triggers != w->triggers) {
            check(false, "ila_one.csv: cut in two at byte " + std::to_string(cut));
            break;
        }
    std::vector<size_t> bytes(one.size());
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = i;
    check(same_wave(*read_csv(one, bytes), *w), "ila_one.csv: fed a byte at a time");
    std::string crlf;
    for (const char c : one) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    check(same_wave(*read_csv(crlf), *w), "ila_one.csv: with CRLF");
    // On a thread, as blit reads it, the first bytes choosing the reader.
    {
        WaveLoad::ReadAt read_at;
        u64 size = 0;
        std::string err;
        check(WaveLoad::local_file(dir + "/ila_one.csv", &read_at, &size, &err), "ila_one.csv: open: " + err);
        WaveLoad load(std::make_shared<Wave>(), read_at, size, 0);
        load.wait();
        check(same_wave(*load.wave(), *w) && load.wave()->triggers == w->triggers && load.wave()->samples, "ila_one.csv: on a thread, the same");
        WaveLoad::local_file(dir + "/ila_one.vcd", &read_at, &size, &err);
        WaveLoad again(std::make_shared<Wave>(), read_at, size, 0);
        again.wait();
        check(same_wave(*again.wave(), *vcd) && again.wave()->triggers == vcd->triggers, "ila_one.vcd: on a thread, the same");
    }

    // ila_windows: four windows of 256, each triggered at its sample 32; count signed, phase octal.
    const std::string four = slurp(dir + "/ila_windows.csv");
    const auto f = read_csv(four);
    check(f->error.empty() && f->triggers == std::vector<u64>{32, 288, 544, 800} && f->windows == std::vector<u64>{256, 512, 768},
          "ila_windows.csv: a trigger a window, and where the windows start: " + f->error);
    check(f->vars[0].radix == DumpRadix::kSigned && f->vars[2].radix == DumpRadix::kOctal, "ila_windows.csv: signed and octal");
    expect("ila_windows.csv: count -1 (signed) at a trigger", value_at(*f, "dut.count", 288), "11111111");
    expect("ila_windows.csv: count -33 at the start", value_at(*f, "dut.count", 0), "11011111");
    expect("ila_windows.csv: phase 2 (octal) in the second window", value_at(*f, "dut.phase", 288), "10");
    const auto fv = read_vcd(slurp(dir + "/ila_windows.vcd"));
    check(fv->triggers == f->triggers && fv->windows == f->windows, "ila_windows.vcd: the same triggers and windows, from _TRIGGER and _WINDOW flipping");
    check(same_capture(*fv, *f, &why), "ila_windows: its CSV as its VCD: " + why);

    // Not to be read, and said: the form with no radix row (its values are
    // in each probe's radix, which it does not say), and broken ones (the
    // real file with a line changed).
    auto said = [&](const std::string& what, const std::string& text, const std::string& error, u64 end = ~0ull) {
        auto b = read_csv(text);
        check(b->done && b->error == error && (end == ~0ull || b->end == end), what + ": \"" + b->error + "\", to " + std::to_string(b->end));
        return b;
    };
    const auto legacy = said("ila_one-legacy.csv", slurp(dir + "/ila_one-legacy.csv"),
                             "no radix row (-legacy_csv_file): write it with -csv_file");
    check(legacy->vars.empty() && legacy->lanes.empty() && legacy->scopes.size() == 1, "ila_one-legacy.csv: no probes kept");
    said("a line short of a value", with_line(one, 57, "54,54,0,52,0,0"), "line 57: 6 values, where the first line names 8 columns", 53);
    check(read_csv(with_line(one, 57, "54,54,0,52,0,0"))->lanes[0].size() == 54, "a line short of a value: the samples before it kept");
    said("a value not in its radix", with_line(one, 103, "100,100,1,4g,0,0,63,0000"), "line 103: count[7:0]: not a HEX value of 8 bits: 4g", 99);
    said("a value too wide", with_line(one, 103, "100,100,1,140,0,0,63,0000"), "line 103: count[7:0]: not a HEX value of 8 bits: 140", 99);
    said("a bit's value too wide", with_line(one, 103, "100,100,1,40,2,0,63,0000"), "line 103: wrap: not a HEX value of 1 bit: 2", 99);
    said("an unsigned value too wide", with_line(one, 103, "100,100,1,40,0,0,4096,0000"), "line 103: sum[11:0]: not a UNSIGNED value of 12 bits: 4096", 99);
    said("a binary value too long", with_line(one, 103, "100,100,1,40,0,0,63,10000"), "line 103: sw_q[3:0]: not a BINARY value of 4 bits: 10000", 99);
    said("samples going back", with_line(one, 103, "99,99,1,40,0,0,63,0000"), "line 103: sample 99 after sample 99", 99);
    said("not a sample's number", with_line(one, 103, "x,100,1,40,0,0,63,0000"), "line 103: not a sample's number: x", 99);
    said("not ILA's", "Time,a,b\n0,1,2\n", "not an ILA capture's CSV: it starts with Time,a,b");
    said("a column with no name", with_line(one, 1, "Sample in Buffer,Sample in Window,TRIGGER,,wrap,phase[1:0],sum[11:0],sw_q[3:0]"),
         "line 1: a column with no name");
    said("a radix it does not read", with_line(one, 2, "Radix - UNSIGNED,UNSIGNED,UNSIGNED,HEX,HEX,HEX,UNSIGNED,ENUM"),
         "line 2: sw_q[3:0] is in a radix this does not read: ENUM");
    said("a radix short", with_line(one, 2, "Radix - UNSIGNED,UNSIGNED,UNSIGNED,HEX,HEX,HEX,UNSIGNED"), "line 2: 7 radixes, where the first line names 8 columns");
    said("empty", "", "the capture is empty");
    said("no samples", line_of(one, 1) + "\n" + line_of(one, 2) + "\n", "the capture has no samples");
    // SIGNED, as two's complement of the probe's width: -128 is 8 bits' least, -129 and 128 do not fit.
    const std::string first = line_of(four, 3);  // "0,0,0,-33,0,1,222,0000"
    auto signed_line = [&](const char* v) { return with_line(four, 3, "0,0,0," + std::string(v) + first.substr(first.find(",0,1,"))); };
    expect("signed: -128", value_at(*read_csv(signed_line("-128")), "dut.count", 0), "10000000");
    expect("signed: 127", value_at(*read_csv(signed_line("127")), "dut.count", 0), "01111111");
    expect("signed: -0", value_at(*read_csv(signed_line("-0")), "dut.count", 0), "00000000");
    said("signed: -129", signed_line("-129"), "line 3: count[7:0]: not a SIGNED value of 8 bits: -129");
    said("signed: 128", signed_line("128"), "line 3: count[7:0]: not a SIGNED value of 8 bits: 128");
    // Past the memory budget: stopped, said, and what came before kept.
    const auto small = read_csv(one, {}, 4096);
    check(small->error.starts_with("the capture needs more memory than timeline.max_mb allows: read up to sample ") && small->end > 0 && small->end < 1023,
          "ila_one.csv over the budget: " + small->error);
}

// Times read from text, in a dump's units (Wave::time_of): "@ 50000" bare,
// "93.00 ns", "0.124us", Icarus's "131500 (1ps)", in dumps of 1ps, 10 ns
// and samples.
void time_of_test(const std::string& dir) {
    const auto ps = read_vcd(slurp(dir + "/times.vcd"));
    const auto ten = read_vcd(slurp(dir + "/pyvcd.vcd"));
    const auto ila = read_vcd(slurp(dir + "/ila_one.vcd"));
    auto of = [](const Wave& w, u64 digits, u32 frac, bool unit, u32 mult, i32 exp) {
        u64 t = 0;
        return w.time_of(digits, frac, unit, mult, exp, &t) ? std::to_string(t) : std::string("none");
    };
    expect("time_of: @ 50000 in 1ps", of(*ps, 50000, 0, false, 1, 0), "50000");
    expect("time_of: 93.00 ns in 1ps", of(*ps, 9300, 2, true, 1, -9), "93000");
    expect("time_of: 0.124us in 1ps", of(*ps, 124, 3, true, 1, -6), "124000");
    expect("time_of: 131500 (1ps)", of(*ps, 131500, 0, true, 1, -12), "131500");
    expect("time_of: 3 (10ps) in 1ps", of(*ps, 3, 0, true, 10, -12), "30");
    expect("time_of: 0.5 fs in 1ps, the nearest", of(*ps, 5, 1, true, 1, -15), "0");
    expect("time_of: 1.5 ps in 1ps, the nearest", of(*ps, 15, 1, true, 1, -12), "2");
    expect("time_of: 2 s in 1ps", of(*ps, 2, 0, true, 1, 0), "2000000000000");
    expect("time_of: 30 ns in 10 ns", of(*ten, 30, 0, true, 1, -9), "3");
    expect("time_of: 34 ns in 10 ns, the nearest", of(*ten, 34, 0, true, 1, -9), "3");
    expect("time_of: 1.5 us in 10 ns", of(*ten, 15, 1, true, 1, -6), "150");
    expect("time_of: bare 12.5 in 10 ns, the nearest", of(*ten, 125, 1, false, 1, 0), "13");
    expect("time_of: past a u64 of ps", of(*ps, 99999999999, 0, true, 1, 0), "none");
    expect("time_of: a capture's sample", of(*ila, 300, 0, false, 1, 0), "300");
    expect("time_of: a capture has no unit of time", of(*ila, 5, 0, true, 1, -9), "none");
}

// Past the memory budget it stops, says so, and keeps what it read.
void budget_test() {
    std::string text;
    made_up(6 << 20, [&](std::string_view s) { text += s; });
    const auto whole = read_vcd(text);
    const auto w = read_vcd(text, {}, 1 << 20);
    check(whole->error.empty() && w->done && w->error.find("timeline.max_mb") != std::string::npos && w->end > 0 && w->end < whole->end,
          "budget: stopped and said so: " + w->error);
    check(w->bytes() < (3 << 19), "budget: kept to: " + std::to_string(w->bytes()));
}

// Read on a thread, as blit reads it, from a file.
void load_test(const std::string& path, const Wave& whole) {
    WaveLoad::ReadAt read_at;
    u64 size = 0;
    std::string err;
    check(WaveLoad::local_file(path, &read_at, &size, &err), "load: open " + path + ": " + err);
    std::atomic<int> told{0};
    WaveLoad load(std::make_shared<Wave>(), read_at, size, 0, [&] { ++told; });
    load.wait();
    check(load.finished() && load.read() == size && told >= 2 && same_wave(*load.wave(), whole), "load: on a thread, the same");
    check(!WaveLoad::local_file(path + ".none", &read_at, &size, &err) && !err.empty(), "load: a file that is not there");
}

// --- FST, through libfst.

std::unique_ptr<Wave> read_fst_file(const std::string& path, u64 max_bytes = 0) {
    auto w = std::make_unique<Wave>();
    read_fst(w.get(), path, max_bytes);
    return w;
}

void write_file(const std::string& path, std::string_view bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::fprintf(stderr, "FAIL: cannot write %s\n", path.c_str());
        std::exit(1);
    }
    std::fclose(f);
}

// The first variable before v that shares its lane, by path ("" if none).
std::string alias_of(const Wave& w, u32 v) {
    for (u32 o = 0; o < v; ++o)
        if (w.vars[o].lane == w.vars[v].lane) return w.path(o);
    return "";
}

// The FST and the VCD of one design run the same way (make/make.sh: the
// same simulator and commands, the run deterministic): the same variables,
// ranges and aliases, span, unit and $dumpoff stretches, and the same
// lanes, change by change. (Not always the same kinds: Verilator's FST says
// logic where its VCD says wire, and parameter where it says wire; nor a
// real's declared width, which Icarus's VCD gives as 1.)
void same_run(const Wave& vcd, const Wave& fst, const std::string& name) {
    check(vcd.vars.size() == fst.vars.size() && vcd.lanes.size() == fst.lanes.size(), name + ": as many variables and lanes as the VCD");
    check(vcd.start == fst.start && vcd.end == fst.end && vcd.unit_mult == fst.unit_mult && vcd.unit_exp == fst.unit_exp && vcd.off == fst.off,
          name + ": the VCD's span, unit and $dumpoff stretches");
    for (u32 v = 0; v < vcd.vars.size(); ++v) {
        const std::string p = vcd.path(v);
        const i64 f = fst.find(p);
        if (f < 0) {
            check(false, name + ": no " + p);
            continue;
        }
        const WaveVar &a = vcd.vars[v], &b = fst.vars[static_cast<size_t>(f)];
        const Lane &x = vcd.lanes[a.lane], &y = fst.lanes[b.lane];
        check(x.kind() == y.kind() && (x.kind() != LaneKind::kBits || (a.width == b.width && x.width() == y.width())) && a.ranged == b.ranged &&
                  a.msb == b.msb && a.lsb == b.lsb,
              name + ": " + p + ": the VCD's lane kind, width and range");
        check(alias_of(vcd, v) == alias_of(fst, static_cast<u32>(f)), name + ": " + p + ": the VCD's alias");
        check(x.size() == y.size(), name + ": " + p + ": " + std::to_string(y.size()) + " changes, the VCD has " + std::to_string(x.size()));
        for (u64 i = 0; i < std::min(x.size(), y.size()); ++i)
            if (x.time(i) != y.time(i) || fmt(x, i) != fmt(y, i)) {
                check(false, name + ": " + p + " change " + std::to_string(i) + ": " + std::to_string(y.time(i)) + " " + fmt(y, i) + ", the VCD " +
                                 std::to_string(x.time(i)) + " " + fmt(x, i));
                break;
            }
    }
}

void icarus_fst_test(const Wave& w) {
    // (As icarus.vcd's, but for the real's width: 8 bytes in FST, 1 in Icarus's VCD.)
    expect("icarus fst: the variables", listing(w),
           "tb.done event 1\n"
           "tb.wrap wire 1\n"
           "tb.drv wire 4 [3:0]\n"
           "tb.count wire 8 [7:0]\n"
           "tb.clk reg 1\n"
           "tb.en reg 1\n"
           "tb.nib reg 4 [3:0]\n"
           "tb.rst reg 1\n"
           "tb.wide reg 96 [95:0]\n"
           "tb.steps integer 32 [31:0]\n"
           "tb.temp real 64\n"
           "tb.u_cnt.clk wire 1 = tb.clk\n"
           "tb.u_cnt.en wire 1 = tb.en\n"
           "tb.u_cnt.rst wire 1 = tb.rst\n"
           "tb.u_cnt.wrap wire 1 = tb.wrap\n"
           "tb.u_cnt.W parameter 32\n"
           "tb.u_cnt.count reg 8 [7:0]\n");
    check(w.lanes.size() == 13 && w.scopes.size() == 3 && w.scopes[2].name == "u_cnt" && w.scopes[2].kind == ScopeKind::kModule && w.scopes[2].parent == 1,
          "icarus fst: lanes and scopes");
    check(w.unit_given && w.unit_mult == 1 && w.unit_exp == -12 && w.start == 0 && w.end == 2772000, "icarus fst: 1 ps, from 0 to 2772000");
    check(w.off == std::vector<std::pair<u64, u64>>{{72000, 97000}}, "icarus fst: the $dumpoff stretch");
    check(w.date.find("2026") != std::string::npos && w.version == "Icarus Verilog", "icarus fst: date and version: " + w.date + ", " + w.version);
    check(w.header && w.done && w.notes.empty() && w.error.empty(), "icarus fst: read with nothing to say: " + w.error);
    expect("icarus fst: nib before it is set", value_at(w, "tb.nib", 31999), "xxxx");
    expect("icarus fst: nib with x and z", value_at(w, "tb.nib", 32000), "1x0z");
    expect("icarus fst: nib while dumping is off", value_at(w, "tb.nib", 80000), "xxxx");
    expect("icarus fst: drv floats at first", value_at(w, "tb.drv", 0), "zzzz");
    expect("icarus fst: a real", value_at(w, "tb.temp", 32000), "1.5");
    expect("icarus fst: a small real", value_at(w, "tb.temp", 70000), "-0.00225");
    expect("icarus fst: a large real", value_at(w, "tb.temp", 2000000), "6.02e+23");
    expect("icarus fst: 96 bits with x and z", value_at(w, "tb.wide", 40000), "10xz" + std::string(92, '0'));
    expect("icarus fst: an alias reads its lane", value_at(w, "tb.u_cnt.clk", 5000), "1");
    expect("icarus fst: the counter", value_at(w, "tb.count", 2757000), "00010001");
    check(changes_of(w, "tb.done") == 3, "icarus fst: an event's changes, each kept");
}

void verilator_fst_test(const Wave& w) {
    // (Its kinds are its own: SystemVerilog's logic, the parameter as one.)
    expect("verilator fst: the variables", listing(w),
           "tb.clk logic 1\n"
           "tb.rst logic 1\n"
           "tb.en logic 1\n"
           "tb.count logic 8 [7:0]\n"
           "tb.wrap logic 1\n"
           "tb.nib logic 4 [3:0]\n"
           "tb.drv wire 4 [3:0]\n"
           "tb.wide logic 96 [95:0]\n"
           "tb.temp real 64\n"
           "tb.steps integer 32 [31:0]\n"
           "tb.done event 1\n"
           "tb.u_cnt.W parameter 32 [31:0]\n"
           "tb.u_cnt.clk wire 1 = tb.clk\n"
           "tb.u_cnt.rst wire 1 = tb.rst\n"
           "tb.u_cnt.en wire 1 = tb.en\n"
           "tb.u_cnt.count wire 8 [7:0] = tb.count\n"
           "tb.u_cnt.wrap wire 1 = tb.wrap\n");
    check(w.lanes.size() == 12 && w.unit_exp == -12 && w.start == 0 && w.end == 2772000 && w.off.empty(), "verilator fst: lanes, unit, span");
    check(w.version == "Generated by VerilatedFst" && w.notes.empty() && w.error.empty(), "verilator fst: read with nothing to say: " + w.error);
    expect("verilator fst: nib (two states)", value_at(w, "tb.nib", 32000), "1000");
    expect("verilator fst: 96 bits", value_at(w, "tb.wide", 70000),
           "110111101010110110111110111011110000000100100011010001010110011110001001101010111100110111101111");
    expect("verilator fst: a 32-bit parameter", value_at(w, "tb.u_cnt.W", 100), "00000000000000000000000000001000");
    check(changes_of(w, "tb.done") == 1, "verilator fst: the event fires once");
}

// Cut short at every length, and every byte changed in turn (to its
// complement, and with its lowest and its highest bit flipped), or every
// step'th: each is read to its end or stopped, and done, never a crash or
// a hang. A cut file is said to be cut, or not finished (cut where a block
// ends), or not an FST file at all; a damaged one may be read as far as
// libfst could, or said; and every file libfst opened for it is closed
// again. (libfst writes to stderr about some of them: quietened here.
// Those it would have ended the process for, or crashed on, or never
// finished, are counted.)
int open_files() {
    int n = 0;
    for (int fd = 0; fd < 4096; ++fd) n += ::fcntl(fd, F_GETFD) != -1;
    return n;
}

void damaged_fst_test(const std::string& path, const std::string& scratch, const std::string& name, size_t step) {
    const u64 t0 = now_ns();
    const std::string bytes = slurp(path);
    const std::string file = scratch + "/damaged.fst";
    std::vector<std::string> wrong;
    int stopped = 0;
    std::fflush(stderr);
    const int saved = ::dup(2), null = ::open("/dev/null", O_WRONLY);
    ::dup2(null, 2);
    const int files = open_files();
    for (size_t len = 0; len < bytes.size(); len += step) {
        write_file(file, std::string_view(bytes).substr(0, len));
        const auto w = read_fst_file(file);
        const bool said = w->error.starts_with("the FST file is cut short") || w->error.starts_with("the FST file has no hierarchy") ||
                          w->error.starts_with("not an FST file");
        if (!w->done || !w->header || !said) wrong.push_back("cut at " + std::to_string(len) + ": " + w->error);
    }
    for (size_t i = 0; i < bytes.size(); i += step)
        for (const u8 x : {0xff, 0x01, 0x80}) {
            std::string b = bytes;
            b[i] = static_cast<char>(b[i] ^ x);
            write_file(file, b);
            const auto w = read_fst_file(file);
            if (!w->done || !w->header) wrong.push_back("byte " + std::to_string(i) + " ^ " + std::to_string(x) + ": not done");
            stopped += w->error.find("libfst stopped reading it") != std::string::npos || w->error.find("libfst could not") != std::string::npos;
        }
    if (open_files() != files) wrong.push_back(std::to_string(open_files() - files) + " files left open");
    std::fflush(stderr);
    ::dup2(saved, 2);
    ::close(saved);
    ::close(null);
    for (const std::string& s : wrong) check(false, name + ": " + s);
    std::printf("%s: %zu bytes, cut short at every %zu and changed: %d stopped instead of ending the process, crashing or not finishing (%.1f s)\n",
                name.c_str(), bytes.size(), step, stopped, static_cast<double>(now_ns() - t0) / 1e9);
}

// The changes a variable is given, by the Lane's rules (a value it has is
// no change, but for an event's; at one time the last counts).
void ref_set(std::vector<RefChange>& c, u64 t, const std::string& v, bool event = false) {
    if (event) {
        if (c.empty() || c.back().t != t) c.push_back({t, ""});
        return;
    }
    if (!c.empty() && c.back().v == v) return;
    if (!c.empty() && c.back().t == t) {
        if (c.size() >= 2 && c[c.size() - 2].v == v) c.pop_back();
        else c.back().v = v;
        return;
    }
    c.push_back({t, v});
}

// An FST written here by libfst's own writer (the same commit as its
// reader, third_party/libfst), for what the simulators' files do not show:
// each of its compressions (zlib, FastLZ, LZ4) and the gzip wrapper around
// a whole file, many sections, a timescale of 10 ps and a time zero, task
// and fork scopes, a 130-bit bus, a bit select, an ascending range, an
// array's element, a string, a real, an event and an alias, values of 0 1 x
// z (an x or a z anywhere in a 64-bit word) and std_logic's h l u w -, and
// $dumpoff. Read back, every lane is the
// list of values it was written.
void writer_fst_test(const std::string& scratch) {
    struct Kind {
        enum fstWriterPackType pack;
        bool wrap;
        const char* name;
    };
    for (const Kind k : {Kind{FST_WR_PT_ZLIB, false, "zlib"}, Kind{FST_WR_PT_FASTLZ, false, "fastlz"}, Kind{FST_WR_PT_LZ4, false, "lz4"},
                         Kind{FST_WR_PT_LZ4, true, "wrapped"}}) {
        const std::string path = scratch + "/writer-" + k.name + ".fst", at = std::string("libfst ") + k.name;
        fstWriterContext* f = fstWriterCreate(path.c_str(), 1);
        fstWriterSetPackType(f, k.pack);
        fstWriterSetRepackOnClose(f, k.wrap);
        fstWriterSetTimescale(f, -11);
        fstWriterSetTimezero(f, -3);
        fstWriterSetDate(f, "today");
        fstWriterSetVersion(f, "libfst's writer");
        fstWriterSetScope(f, FST_ST_VCD_MODULE, "top", nullptr);
        const fstHandle clk = fstWriterCreateVar(f, FST_VT_VCD_WIRE, FST_VD_INPUT, 1, "clk", 0);
        const fstHandle bus = fstWriterCreateVar(f, FST_VT_SV_LOGIC, FST_VD_IMPLICIT, 130, "bus [129:0]", 0);
        const fstHandle bit = fstWriterCreateVar(f, FST_VT_VCD_REG, FST_VD_IMPLICIT, 1, "flags [3]", 0);
        const fstHandle up = fstWriterCreateVar(f, FST_VT_VCD_REG, FST_VD_IMPLICIT, 8, "up [0:7]", 0);
        const fstHandle word = fstWriterCreateVar(f, FST_VT_SV_LONGINT, FST_VD_IMPLICIT, 64, "word", 0);
        fstWriterSetScope(f, FST_ST_VCD_TASK, "t", nullptr);
        const fstHandle state = fstWriterCreateVar(f, FST_VT_GEN_STRING, FST_VD_IMPLICIT, 0, "state", 0);
        const fstHandle volts = fstWriterCreateVar(f, FST_VT_VCD_REAL, FST_VD_IMPLICIT, 8, "volts", 0);
        fstWriterSetUpscope(f);
        fstWriterSetScope(f, FST_ST_VCD_FORK, "par", nullptr);
        const fstHandle ev = fstWriterCreateVar(f, FST_VT_VCD_EVENT, FST_VD_IMPLICIT, 1, "ev", 0);
        const fstHandle mem = fstWriterCreateVar(f, FST_VT_SV_LOGIC, FST_VD_IMPLICIT, 4, "mem[3]", 0);
        fstWriterCreateVar(f, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk_in", clk);
        fstWriterSetUpscope(f);
        fstWriterSetUpscope(f);
        std::map<fstHandle, std::vector<RefChange>> ref;
        std::mt19937_64 rng(5);
        auto bits = [&](u32 n, const char* of) {
            std::string s(n, '0');
            for (char& c : s) c = of[rng() % std::strlen(of)];
            return s;
        };
        for (u64 step = 0; step < 3000; ++step) {
            const u64 t = 100 + step * 7;
            fstWriterEmitTimeChange(f, t);
            if (step == 1000) fstWriterEmitDumpActive(f, 0);
            if (step == 1100) fstWriterEmitDumpActive(f, 1);
            const std::string c = step % 2 ? "1" : "0";
            fstWriterEmitValueChange(f, clk, c.data());
            ref_set(ref[clk], t, c);
            if (step % 3 == 0) {
                const std::string v = bits(130, step % 30 == 0 ? "01xz" : "01");
                fstWriterEmitValueChange(f, bus, v.data());
                ref_set(ref[bus], t, v);
            }
            if (step % 2 == 0) {
                // (Now and then an x or a z, anywhere in it.)
                std::string v = bits(64, "01");
                if (step % 14 == 0) v[rng() % 64] = "xz"[rng() % 2];
                fstWriterEmitValueChange(f, word, v.data());
                ref_set(ref[word], t, v);
            }
            if (step % 5 == 0) {
                const std::string b = bits(1, "01xzhluw-"), u = bits(8, "01");
                fstWriterEmitValueChange(f, bit, b.data());
                fstWriterEmitValueChange(f, up, u.data());
                fstWriterEmitValueChange(f, mem, u.data() + 4);
                // (h and l are read as 1 and 0; u, w and - as x.)
                const char d = b[0] == 'h' ? '1' : b[0] == 'l' ? '0' : std::strchr("uw-", b[0]) ? 'x' : b[0];
                ref_set(ref[bit], t, std::string(1, d));
                ref_set(ref[up], t, u);
                ref_set(ref[mem], t, u.substr(4));
            }
            if (step % 50 == 0) {
                const std::string s = step % 100 ? "RUN" : step % 200 ? "IDLE" : "a state " + std::to_string(step);
                fstWriterEmitVariableLengthValueChange(f, state, s.data(), static_cast<uint32_t>(s.size()));
                ref_set(ref[state], t, s);
                const double d = static_cast<double>(step) / 8.0 - 3.3;
                fstWriterEmitValueChange(f, volts, &d);
                char buf[32];
                ref_set(ref[volts], t, std::string(buf, std::to_chars(buf, buf + sizeof buf, d).ptr));
            }
            if (step % 97 == 0) {
                fstWriterEmitValueChange(f, ev, "1");
                ref_set(ref[ev], t, "", true);
            }
            if (step % 400 == 399) fstWriterFlushContext(f);
        }
        fstWriterEmitTimeChange(f, 100 + 3000 * 7);
        fstWriterClose(f);
        const auto w = read_fst_file(path);
        check(dump_kind(slurp(path)) == DumpKind::kFst, at + ": its first bytes say FST");
        expect(at + ": the variables", listing(*w),
               "top.clk wire 1\ntop.bus logic 130 [129:0]\ntop.flags reg 1 [3:3]\ntop.up reg 8 [0:7]\ntop.word longint 64\ntop.t.state string 0\ntop.t.volts real 64\n"
               "top.par.ev event 1\ntop.par.mem[3] logic 4\ntop.par.clk_in wire 1 = top.clk\n");
        check(w->scopes.size() == 4 && w->scopes[2].kind == ScopeKind::kTask && w->scopes[3].kind == ScopeKind::kFork, at + ": task and fork scopes");
        check(w->unit_mult == 10 && w->unit_exp == -12 && w->start == 100 && w->end == 100 + 3000 * 7 && w->date == "today" && w->version == "libfst's writer",
              at + ": 10 ps, the span, date and version");
        check(w->off == std::vector<std::pair<u64, u64>>{{7100, 7800}}, at + ": the $dumpoff stretch");
        check(w->notes == std::vector<std::string>{"a time zero of -3, not added to the times"} && w->error.empty(), at + ": its notes: " + w->error);
        for (u32 v = 0; v < w->vars.size(); ++v) {
            const WaveVar& x = w->vars[v];
            const Lane& l = w->lanes[x.lane];
            const auto& c = ref[static_cast<fstHandle>(std::stoul(x.code))];
            check(l.size() == c.size(), at + ": " + x.name + ": " + std::to_string(l.size()) + " changes, " + std::to_string(c.size()) + " written");
            for (u64 i = 0; i < std::min<u64>(l.size(), c.size()); ++i)
                if (l.time(i) != c[i].t || fmt(l, i) != c[i].v) {
                    check(false, at + ": " + x.name + " change " + std::to_string(i) + ": " + std::to_string(l.time(i)) + " " + fmt(l, i) + ", written " +
                                     std::to_string(c[i].t) + " " + c[i].v);
                    break;
                }
        }
        if (k.pack == FST_WR_PT_ZLIB) damaged_fst_test(path, scratch, at, 97);
    }
}

void fst_test(const std::string& dir, const std::string& scratch) {
    struct Run {
        const char* name;
        void (*test)(const Wave&);
    };
    for (const Run r : {Run{"icarus", icarus_fst_test}, Run{"verilator", verilator_fst_test}}) {
        const std::string fst = dir + "/" + r.name + ".fst", vcd = dir + "/" + r.name + ".vcd";
        const auto w = read_fst_file(fst);
        r.test(*w);
        same_run(*read_vcd(slurp(vcd)), *w, std::string(r.name) + ".fst");
        check(dump_kind(slurp(fst)) == DumpKind::kFst && dump_kind(slurp(vcd)) == DumpKind::kVcd, std::string(r.name) + ": the format, from the first bytes");
        // On a thread, as blit reads it: the same, with progress told.
        std::atomic<int> told{0};
        WaveLoad load(std::make_shared<Wave>(), fst, 0, [&] { ++told; });
        load.wait();
        check(load.finished() && load.read() == load.size() && load.size() > 0 && told >= 2 && same_wave(*load.wave(), *w),
              std::string(r.name) + ".fst: on a thread, the same");
        damaged_fst_test(fst, scratch, std::string(r.name) + ".fst", 1);
    }
    writer_fst_test(scratch);
    // A larger one: progress told as it reads, from 0 to no more than the
    // file; past the budget it stops, says so, and keeps what it read (the
    // whole file's changes, up to where it stopped, which is where its span
    // ends); asked to stop, it stops.
    const std::string big = scratch + "/big.fst";
    made_up_fst(big, 3'000'000);
    const auto whole = read_fst_file(big);
    check(whole->error.empty() && whole->changes > 2'000'000 && whole->vars.size() == 813, "fst: a larger one, read whole: " + whole->error);
    std::vector<u64> told;
    auto part = std::make_unique<Wave>();
    read_fst(part.get(), big, 4 << 20, {}, [&](u64 at) { told.push_back(at); });
    const u64 size = slurp(big).size();
    check(told.size() > 2 && told[0] == 0 && std::ranges::is_sorted(told) && told.back() <= size, "fst: progress, from 0 to its size");
    check(part->done && part->error.starts_with("the dump needs more memory than timeline.max_mb allows: read up to ") && part->changes > 0 &&
              part->changes < whole->changes && part->bytes() < (6 << 20) && part->end > whole->start && part->end < whole->end,
          "fst: past the budget: " + part->error + ", " + std::to_string(part->bytes()) + " bytes");
    bool prefix = part->lanes.size() == whole->lanes.size();
    for (size_t i = 0; prefix && i < part->lanes.size(); ++i)
        for (u64 j = 0; prefix && j < part->lanes[i].size(); ++j)
            prefix = j < whole->lanes[i].size() && part->lanes[i].time(j) == whole->lanes[i].time(j) && fmt(part->lanes[i], j) == fmt(whole->lanes[i], j);
    check(prefix, "fst: past the budget, what it kept is the whole file's");
    std::stop_source stop;
    auto stopped = std::make_unique<Wave>();
    read_fst(stopped.get(), big, 0, stop.get_token(), [&](u64) { stop.request_stop(); });
    check(stopped->done && stopped->error == "stopped" && stopped->changes < whole->changes && stopped->end < whole->end, "fst: stopped: " + stopped->error);
    // Not FST, or not there.
    const auto vcd = read_fst_file(dir + "/icarus.vcd");
    check(vcd->done && vcd->error == "not an FST file: it does not start with an FST header", "fst: a VCD file said: " + vcd->error);
    const auto none = read_fst_file(dir + "/none.fst");
    check(none->done && none->error == "No such file or directory", "fst: a file not there: " + none->error);
}

int run_check(const std::string& dir, int seeds, std::string scratch) {
    lane_test(seeds);
    spec_test();
    malformed_test();
    budget_test();
    struct Dump {
        const char* name;
        void (*test)(const Wave&);
        size_t step;  // (cut in two at every byte for the small ones)
    };
    for (const Dump d : {Dump{"icarus.vcd", icarus_test, 7}, Dump{"verilator.vcd", verilator_test, 5}, Dump{"yosys.vcd", yosys_test, 1},
                         Dump{"pyvcd.vcd", pyvcd_test, 1}, Dump{"ila_one.vcd", ila_vcd_test, 13}}) {
        const std::string path = dir + "/" + d.name;
        const std::string text = slurp(path);
        const auto w = read_vcd(text);
        const auto ref = reference(text);
        d.test(*w);
        compare(*w, ref, d.name);
        queries(*w, ref, d.name, seeds);
        pieces_test(text, *w, d.name, d.step);
        cut_short_test(text, *w, d.name);
        if (d.test == icarus_test) load_test(path, *w);
        // CRLF line ends, as a dump written on Windows has.
        std::string crlf;
        for (const char c : text) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        check(same_wave(*read_vcd(crlf), *w), std::string(d.name) + ": with CRLF");
    }
    if (scratch.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        std::string t = std::string(tmp && *tmp ? tmp : "/tmp") + "/blit-timeline-XXXXXX";
        if (!::mkdtemp(t.data())) return std::fprintf(stderr, "FAIL: no scratch folder\n"), 1;
        scratch = t;
    }
    fst_test(dir, scratch);
    ila_test(dir);
    time_of_test(dir);
    if (failures) return 1;
    std::printf("timeline ok: %d seeds\n", seeds);
    return 0;
}

// --- A made-up dump of about `bytes`, in VCD's syntax (IEEE 1364-2005
// 18.2.1) laid out as Verilator lays it out: a clock, bits that toggle at
// random, counters of every width from 2 to 64 bits, a few 128-bit buses,
// reals. Handed to `put` a megabyte at a time.
void made_up(u64 bytes, const std::function<void(std::string_view)>& put) {
    const int kBits = 600, kBuses = 200, kWide = 8, kReals = 4;
    std::string out = "$version made by blit_timeline_test gen $end\n$timescale 1ps $end\n$scope module top $end\n";
    auto code = [](u32 n) {
        std::string c;
        do c += static_cast<char>('!' + n % 94), n /= 94;
        while (n);
        return c;
    };
    u32 n = 0;
    out += " $var wire 1 " + code(n++) + " clk $end\n";
    for (int i = 0; i < kBits; ++i) out += " $var wire 1 " + code(n++) + " b" + std::to_string(i) + " $end\n";
    for (int i = 0; i < kBuses; ++i) out += " $var wire " + std::to_string(2 + i % 63) + " " + code(n++) + " bus" + std::to_string(i) + " [" + std::to_string(1 + i % 63) + ":0] $end\n";
    for (int i = 0; i < kWide; ++i) out += " $var wire 128 " + code(n++) + " wide" + std::to_string(i) + " [127:0] $end\n";
    for (int i = 0; i < kReals; ++i) out += " $var real 64 " + code(n++) + " r" + std::to_string(i) + " $end\n";
    out += "$upscope $end\n$enddefinitions $end\n";
    u64 x = 88172645463325252ull;
    auto rnd = [&] {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        return x;
    };
    std::vector<u64> bus(kBuses);
    u64 written = 0;
    for (u64 cycle = 0; written < bytes; ++cycle) {
        out += "#" + std::to_string(cycle * 5000) + "\n" + (cycle & 1 ? "0" : "1") + code(0) + "\n";
        if (cycle & 1) {
            for (int i = 0; i < kBits; ++i)
                if (rnd() % 8 == 0) out += ((rnd() & 1) ? "1" : "0") + code(1 + static_cast<u32>(i)) + "\n";
            for (int i = 0; i < kBuses; ++i) {
                if (i % 4 != static_cast<int>((cycle / 2) % 4)) continue;
                bus[static_cast<size_t>(i)] = (bus[static_cast<size_t>(i)] + 1) & ((2ull << (1 + i % 63)) - 1);
                out += "b";
                const u64 v = bus[static_cast<size_t>(i)];
                bool lead = true;
                for (int b = 1 + i % 63; b >= 0; --b) {
                    const bool one = (v >> b) & 1;
                    if (one) lead = false;
                    if (!lead || b == 0) out += one ? '1' : '0';
                }
                out += " " + code(1 + kBits + static_cast<u32>(i)) + "\n";
            }
            if (cycle % 16 == 1)
                for (int i = 0; i < kWide; ++i) {
                    out += "b";
                    for (int b = 0; b < 128; ++b) out += (rnd() & 1) ? '1' : '0';
                    out += " " + code(1 + kBits + kBuses + static_cast<u32>(i)) + "\n";
                }
            if (cycle % 4 == 1)
                for (int i = 0; i < kReals; ++i) {
                    char buf[40];
                    std::snprintf(buf, sizeof buf, "r%.6g ", static_cast<double>(rnd() % 100000) / 7.0);
                    out += buf + code(1 + kBits + kBuses + kWide + static_cast<u32>(i)) + "\n";
                }
        }
        if (out.size() > (1 << 20)) {
            put(out);
            written += out.size();
            out.clear();
        }
    }
    put(out);
}

// --- A made-up FST of about `changes` changes, written by libfst's writer,
// of the same signals as made_up's: a clock, bits that toggle at random,
// counters of every width from 2 to 64 bits, 128-bit buses, reals; a section
// every 200,000 cycles or so, as a simulator flushes them.
void made_up_fst(const std::string& path, u64 changes) {
    const int kBits = 600, kBuses = 200, kWide = 8, kReals = 4;
    fstWriterContext* f = fstWriterCreate(path.c_str(), 1);
    fstWriterSetTimescale(f, -12);
    fstWriterSetVersion(f, "made by blit_timeline_test genfst");
    fstWriterSetScope(f, FST_ST_VCD_MODULE, "top", nullptr);
    std::vector<fstHandle> h;
    h.push_back(fstWriterCreateVar(f, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0));
    for (int i = 0; i < kBits; ++i) h.push_back(fstWriterCreateVar(f, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, ("b" + std::to_string(i)).c_str(), 0));
    for (int i = 0; i < kBuses; ++i)
        h.push_back(fstWriterCreateVar(f, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, static_cast<uint32_t>(2 + i % 63),
                                       ("bus" + std::to_string(i) + " [" + std::to_string(1 + i % 63) + ":0]").c_str(), 0));
    for (int i = 0; i < kWide; ++i) h.push_back(fstWriterCreateVar(f, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 128, ("wide" + std::to_string(i) + " [127:0]").c_str(), 0));
    for (int i = 0; i < kReals; ++i) h.push_back(fstWriterCreateVar(f, FST_VT_VCD_REAL, FST_VD_IMPLICIT, 8, ("r" + std::to_string(i)).c_str(), 0));
    fstWriterSetUpscope(f);
    u64 x = 88172645463325252ull;
    auto rnd = [&] {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        return x;
    };
    std::vector<u64> bus(kBuses);
    std::string v;
    u64 written = 0;
    for (u64 cycle = 0; written < changes; ++cycle) {
        fstWriterEmitTimeChange(f, cycle * 5000);
        fstWriterEmitValueChange(f, h[0], cycle & 1 ? "0" : "1");
        ++written;
        if (!(cycle & 1)) continue;
        for (int i = 0; i < kBits; ++i)
            if (rnd() % 8 == 0) fstWriterEmitValueChange(f, h[1 + static_cast<size_t>(i)], (rnd() & 1) ? "1" : "0"), ++written;
        for (int i = 0; i < kBuses; ++i) {
            if (i % 4 != static_cast<int>((cycle / 2) % 4)) continue;
            const int width = 2 + i % 63;
            u64& b = bus[static_cast<size_t>(i)];
            b = (b + 1) & ((2ull << (width - 1)) - 1);
            v.assign(static_cast<size_t>(width), '0');
            for (int k = 0; k < width; ++k) v[static_cast<size_t>(width - 1 - k)] = (b >> k) & 1 ? '1' : '0';
            fstWriterEmitValueChange(f, h[1 + kBits + static_cast<size_t>(i)], v.data());
            ++written;
        }
        if (cycle % 16 == 1)
            for (int i = 0; i < kWide; ++i) {
                v.assign(128, '0');
                for (char& c : v) c = (rnd() & 1) ? '1' : '0';
                fstWriterEmitValueChange(f, h[1 + kBits + kBuses + static_cast<size_t>(i)], v.data());
                ++written;
            }
        if (cycle % 4 == 1)
            for (int i = 0; i < kReals; ++i) {
                const double d = static_cast<double>(rnd() % 100000) / 7.0;
                fstWriterEmitValueChange(f, h[1 + kBits + kBuses + kWide + static_cast<size_t>(i)], &d);
                ++written;
            }
        if (cycle % 200000 == 199999) fstWriterFlushContext(f);
    }
    fstWriterClose(f);
}

int run_gen(const std::string& path, u64 mb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return 1;
    made_up(mb << 20, [&](std::string_view s) { std::fwrite(s.data(), 1, s.size(), f); });
    return std::fclose(f) == 0 ? 0 : 1;
}

// A dump read as blit reads it, on a thread: an FST by its path (as its
// first bytes say), anything else through read_at.
std::unique_ptr<WaveLoad> open_load(const std::string& path, const WaveLoad::ReadAt& read_at, u64 size, u64 max_bytes) {
    char head[16] = {};
    const i64 n = read_at(0, head, sizeof head);
    if (n > 0 && dump_kind(std::string_view(head, static_cast<size_t>(n))) == DumpKind::kFst)
        return std::make_unique<WaveLoad>(std::make_shared<Wave>(), path, max_bytes);
    return std::make_unique<WaveLoad>(std::make_shared<Wave>(), read_at, size, max_bytes);
}

int run_load(const std::string& path, u64 max_mb) {
    WaveLoad::ReadAt read_at;
    u64 size = 0;
    std::string err;
    if (!WaveLoad::local_file(path, &read_at, &size, &err)) {
        std::printf("error: %s\n", err.c_str());
        return 1;
    }
    const auto load = open_load(path, read_at, size, max_mb << 20);
    load->wait();
    const Wave& w = *load->wave();
    std::printf("vars %zu lanes %zu changes %" PRIu64 " end %s memory %zu\n", w.vars.size(), w.lanes.size(), w.changes, w.time_text(w.end).c_str(), w.bytes());
    if (!w.error.empty()) std::printf("error: %s\n", w.error.c_str());
    for (const std::string& n : w.notes) std::printf("note: %s\n", n.c_str());
    return 0;
}

int run_bench(const std::string& path) {
    WaveLoad::ReadAt read_at;
    u64 size = 0;
    std::string err;
    if (!WaveLoad::local_file(path, &read_at, &size, &err)) return 1;
    // (Once to have it in the page cache, then timed.)
    for (int pass = 0; pass < 2; ++pass) {
        const u64 t0 = now_ns();
        const auto load = open_load(path, read_at, size, 0);
        load->wait();
        const double s = static_cast<double>(now_ns() - t0) / 1e9;
        const Wave& w = *load->wave();
        if (pass == 0) continue;
        size_t lane_bytes = 0;
        for (const Lane& l : w.lanes) lane_bytes += l.bytes();
        std::printf("read %.1f MB in %.3f s: %.0f MB/s; %zu variables, %" PRIu64 " changes, lanes %.1f MB: %.2f bytes a change%s%s\n",
                    static_cast<double>(size) / 1e6, s, static_cast<double>(size) / 1e6 / s, w.vars.size(), w.changes, static_cast<double>(lane_bytes) / 1e6,
                    static_cast<double>(lane_bytes) / static_cast<double>(std::max<u64>(w.changes, 1)), w.error.empty() ? "" : "; ", w.error.c_str());
        // (Peak memory: the lanes, and what the reader held while it read.)
        struct rusage ru{};
        ::getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
        const double peak = static_cast<double>(ru.ru_maxrss) / 1e6;
#else
        const double peak = static_cast<double>(ru.ru_maxrss) * 1024 / 1e6;
#endif
        std::printf("  %.1f million changes a second; the process's peak memory %.0f MB\n", static_cast<double>(w.changes) / 1e6 / s, peak);
        // The densest lane of one bit, and the densest bus.
        const Lane* bit = nullptr;
        const Lane* bus = nullptr;
        for (const Lane& l : w.lanes) {
            if (l.kind() != LaneKind::kBits) continue;
            const Lane*& best = l.width() == 1 ? bit : bus;
            if (!best || l.size() > best->size()) best = &l;
        }
        for (const Lane* l : {bit, bus}) {
            if (!l) continue;
            std::vector<LaneColumn> cols(2000);
            const u64 reps = 200;
            // The whole dump in 2000 columns, a tenth of it, and a span of about 2000 changes.
            const u64 spans[][2] = {{w.start, w.end}, {w.start, w.start + (w.end - w.start) / 10}, {l->time(l->size() / 2), l->time(std::min(l->size() - 1, l->size() / 2 + 2000))}};
            for (const auto& sp : spans) {
                const u64 c0 = now_ns();
                u64 sum = 0;
                for (u64 r = 0; r < reps; ++r) {
                    l->columns(sp[0], sp[1], cols);
                    sum += cols[1999].first;
                }
                const double us = static_cast<double>(now_ns() - c0) / 1e3 / reps;
                std::printf("  %u-bit lane of %" PRIu64 " changes, 2000 columns over %" PRIu64 " changes: %.1f us (%.0f ns a column)%s\n", l->width(), l->size(),
                            l->count_before(sp[1]) - l->count_before(sp[0]), us, us * 1000 / 2000, sum == 1 ? " " : "");
            }
            const u64 q0 = now_ns();
            u64 sum = 0;
            for (u64 r = 0; r < 100000; ++r) sum += l->at(w.start + (r * 7919) % (w.end - w.start + 1));
            std::printf("  value at a time: %.0f ns%s\n", static_cast<double>(now_ns() - q0) / 100000, sum == 1 ? " " : "");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "check" && argc > 2) return run_check(argv[2], argc > 3 ? std::atoi(argv[3]) : 200, argc > 4 ? argv[4] : "");
    if (cmd == "gen" && argc > 3) return run_gen(argv[2], std::strtoull(argv[3], nullptr, 10));
    if (cmd == "genfst" && argc > 3) return made_up_fst(argv[2], std::strtoull(argv[3], nullptr, 10) * 1'000'000), 0;
    if (cmd == "load" && argc > 2) return run_load(argv[2], argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0);
    if (cmd == "bench" && argc > 2) return run_bench(argv[2]);
    std::fprintf(stderr, "usage: blit_timeline_test check DIR [SEEDS [SCRATCH]] | load FILE [MAX_MB] | gen FILE MB | genfst FILE M | bench FILE\n");
    return 2;
}
