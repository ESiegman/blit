// Part of the build's output read for problems (buildscan.hpp): Test frameworks' results (GoogleTest, Catch2, doctest, Unity), and what a run comes to for its task's row: the tests' count, a firmware's size, timing's slack.
//
// GoogleTest: "[ RUN      ] Suite.Name", "path:line: Failure" and the
// expectation under it to a blank line, "[       OK ]", "[  FAILED  ]" and
// "[  SKIPPED ] Suite.Name (0 ms)", and its count ("[  PASSED  ] 1 test.").
// Catch2 (v3's console reporter): a test case's heading (its name between
// lines of dashes, its place, a line of dots), "path:line: FAILED:" with the
// expression and its expansion under it, "0.000 s: name" (--durations yes)
// and "test cases: 3 | 1 passed | 1 failed | 1 skipped". doctest: "path:line:"
// over "TEST CASE:  name", "path:line: ERROR: CHECK( x ) is NOT correct!"
// (and "path(line):", as it writes it for MSVC) with its values, and its
// count. Unity: "path:line:test:PASS", ":FAIL: message", ":IGNORE" and "3
// Tests 1 Failures 1 Ignored". CTest's own lines ("1/3 Test #1: adds ...
// Passed"), when it runs them: its count is the run's, not theirs added up.
//
// Sizes: size's table ("text data bss dec hex filename", GNU's and
// arm-none-eabi's) and the linker's --print-memory-usage ("Memory region
// Used Size Region Size %age Used"). Timing: nextpnr's "Max frequency for
// clock 'clk': 48.31 MHz (PASS at 12.00 MHz)" and Vivado's timing summary
// (its "WNS(ns) TNS(ns) ..." table, "Timing constraints are not met."); a
// clock that misses is a warning at the constraints file the output named
// (Vivado's "Parsing XDC File [x]", nextpnr's --pcf as make echoes the
// command), else at the tool's name, a file that is nowhere.
#include <algorithm>
#include <cstdio>

#include "buildscan.hpp"

namespace blit {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

u32 number(std::string_view s) {
    u64 n = 0;
    for (const char c : s) {
        if (!digit(c)) break;
        n = std::min<u64>(n * 10 + static_cast<u64>(c - '0'), 0xFFFFFFFFu);
    }
    return static_cast<u32>(n);
}

// The next word of s (spaces apart), taken off it.
std::string_view word(std::string_view* s) {
    *s = trim(*s);
    const size_t sp = s->find(' ');
    const std::string_view w = s->substr(0, sp);
    *s = sp == std::string_view::npos ? std::string_view{} : s->substr(sp + 1);
    return w;
}

bool all_digits(std::string_view s) { return !s.empty() && std::ranges::all_of(s, digit); }

bool decimal(std::string_view s, double* out) {
    if (s.empty() || s.size() > 32) return false;
    char buf[40];
    s.copy(buf, s.size());
    buf[s.size()] = 0;
    char* end = nullptr;
    *out = std::strtod(buf, &end);
    return end == buf + s.size();
}

// A line of one character, at least 20 of it (Catch2's and doctest's rules).
bool rule(std::string_view s, char c) { return s.size() >= 20 && s.find_first_not_of(c) == std::string_view::npos; }

// "path:line" or "path(line)" (doctest's for MSVC): the path, and the line.
bool path_line(std::string_view s, std::string_view* path, u32* line) {
    size_t at;
    if (s.ends_with(')')) {
        at = s.rfind('(');
        if (at == std::string_view::npos || !all_digits(s.substr(at + 1, s.size() - at - 2))) return false;
        *line = number(s.substr(at + 1));
    } else {
        size_t d = s.size();
        while (d > 0 && digit(s[d - 1])) --d;
        if (d == s.size() || d < 2 || s[d - 1] != ':') return false;
        *line = number(s.substr(d));
        at = d - 1;
    }
    *path = s.substr(0, at);
    return !path->empty() && *line > 0 && path->front() != ' ' && path->find(": ") == std::string_view::npos;
}

// A C or C++ file, by its ending: what a test framework's places are in.
bool c_family(std::string_view path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view e = path.substr(dot + 1);
    for (const std::string_view k : {"c", "cc", "cpp", "cxx", "c++", "h", "hh", "hpp", "hxx", "h++", "ipp", "inl", "cu", "m", "mm"})
        if (e == k) return true;
    return false;
}

// More of a failure's message: a line of what its framework says under it.
void join(std::string& m, std::string_view line) {
    line = trim(line);
    if (line.empty() || m.size() >= 600) return;
    if (!m.empty()) m += m.back() == ':' || m.back() == '!' || m.back() == '.' ? " " : "; ";
    m.append(line.substr(0, 600 - std::min<size_t>(600, m.size())));
}

// "3 | 1 passed | 1 failed | 1 skipped" (Catch2's and doctest's).
void counts(std::string_view s, u32* passed, u32* failed, u32* skipped) {
    while (!s.empty()) {
        const size_t bar = s.find('|');
        const std::string_view f = trim(s.substr(0, bar));
        s = bar == std::string_view::npos ? std::string_view{} : s.substr(bar + 1);
        if (f.empty() || !digit(f[0])) continue;
        const u32 n = number(f);
        const std::string_view what = trim(f.substr(f.find(' ') == std::string_view::npos ? f.size() : f.find(' ')));
        if (what == "passed" || what == "failed as expected") *passed += n;
        else if (what == "failed") *failed += n;
        else if (what == "skipped") *skipped += n;
    }
}

// Bytes as a few figures: "120 B", "4.1 KB", "256 KB", "1.5 MB" (KB 1024 bytes, as ld's).
std::string bytes(u64 n) {
    if (n < 1024) return std::to_string(n) + " B";
    char buf[32];
    const bool mb = n >= 1024 * 1024;
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(n) / (mb ? 1024.0 * 1024.0 : 1024.0));
    std::string s = buf;
    if (s.ends_with(".0")) s.resize(s.size() - 2);
    return s + (mb ? " MB" : " KB");
}

// ld's "4080 B", "256 KB", "1 MB", "2 GB".
bool ld_size(std::string_view n, std::string_view unit, u64* out) {
    if (!all_digits(n)) return false;
    u64 v = 0;
    for (const char c : n) v = v * 10 + static_cast<u64>(c - '0');
    const u64 k = unit == "B" ? 1 : unit == "KB" ? 1024 : unit == "MB" ? 1024 * 1024 : unit == "GB" ? 1024ull * 1024 * 1024 : 0;
    *out = v * k;
    return k != 0;
}

}  // namespace

// ---------------------------------------------------------------- what it comes to

std::string BuildScan::summary_of(const Outcome& o, const std::vector<SizeUsed>* before) {
    std::vector<std::string> parts;
    if (o.tests) {
        std::string t = std::to_string(o.passed) + " passed";
        if (o.failed) t += ", " + std::to_string(o.failed) + " failed";
        if (o.skipped) t += ", " + std::to_string(o.skipped) + " skipped";
        parts.push_back(std::move(t));
    }
    // The memory regions where the linker gave them, else size's sections.
    const bool regions = std::ranges::any_of(o.sizes, [](const SizeUsed& s) { return s.total > 0; });
    std::string sizes;
    for (const SizeUsed& s : o.sizes) {
        if ((s.total > 0) != regions) continue;
        if (!sizes.empty()) sizes += ", ";
        sizes += s.name + " " + bytes(s.used);
        if (s.total) {
            char pct[32];
            std::snprintf(pct, sizeof pct, " (%.1f%%)", 100.0 * static_cast<double>(s.used) / static_cast<double>(s.total));
            sizes += " / " + bytes(s.total) + pct;
        }
        if (before)
            for (const SizeUsed& b : *before)
                if (b.name == s.name && b.total == s.total && b.used != s.used)
                    sizes += s.used > b.used ? " +" + bytes(s.used - b.used) : " -" + bytes(b.used - s.used);
    }
    if (!sizes.empty()) parts.push_back(std::move(sizes));
    if (!o.timing.empty()) parts.push_back(o.timing);
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : " \xC2\xB7 ") + p;
    return out;
}

void BuildScan::outcome_changed() {
    outcome_.tests = ctest_ || counted_;
    outcome_.passed = ctest_ ? ctest_passed_ : counted_passed_;
    outcome_.failed = ctest_ ? ctest_failed_ : counted_failed_;
    outcome_.skipped = ctest_ ? ctest_skipped_ : counted_skipped_;
    // Timing: each clock nextpnr gave, else Vivado's summary.
    std::string& t = outcome_.timing;
    t.clear();
    for (const Clock& c : clocks_) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s %.1f MHz, slack %+.2f ns", c.name.c_str(), c.mhz, c.wanted > 0 && c.mhz > 0 ? 1000.0 / c.wanted - 1000.0 / c.mhz : 0.0);
        t += (t.empty() ? "" : "; ") + std::string(buf);
    }
    if (t.empty() && vivado_read_) {
        char buf[120];
        std::snprintf(buf, sizeof buf, "WNS %+.3f ns, TNS %+.3f ns, WHS %+.3f ns", wns_, tns_, whs_);
        t = buf;
    }
    summary_ = summary_of(outcome_);
    ++outcome_gen_;
}

void BuildScan::tests_counted(u32 passed, u32 failed, u32 skipped) {
    counted_ = true;
    counted_passed_ += passed;
    counted_failed_ += failed;
    counted_skipped_ += skipped;
    outcome_changed();
}

// ---------------------------------------------------------------- the tests

void BuildScan::test_result(std::string_view framework, std::string_view name, u8 result, std::string_view message) {
    if (name.empty() || results_.size() >= 10000) return;
    results_.push_back({std::string(name), std::string(framework), result, std::string(message.substr(0, 600))});
}

// A failure's place: what its framework says under it is read to the
// blank line that ends it.
void BuildScan::test_failure(std::string_view path, u32 line, u8 kind, std::string_view first) {
    test_failure_end();
    fail_ = true;
    fail_kind_ = kind;
    fail_path_ = std::string(path);
    fail_line_ = line;
    fail_lines_ = 0;
    fail_message_.clear();
    join(fail_message_, first);
}

void BuildScan::test_failure_end() {
    if (!fail_) return;
    fail_ = false;
    if (fail_kind_ == kCatchSkip) {  // (a skip, and why: no problem)
        if (case_result_ != 2) case_result_ = 3, case_message_ = fail_message_;
        return;
    }
    const bool warning = fail_kind_ == kDoctestWarning;
    if (!warning) {
        if (case_result_ != 2) case_message_ = fail_message_;
        case_result_ = 2;
    }
    std::string m = test_name_.empty() ? std::string() : test_name_ + ": ";
    m += fail_message_.empty() ? std::string("failed") : fail_message_;
    add(fail_path_, fail_line_, 0, warning ? 2 : 1, m);
}

// The test the lines were about has ended: how it went, where its
// framework says no more (Catch2 and doctest name a case only when it
// fails, or asked to say each assertion).
void BuildScan::test_case_end() {
    test_failure_end();
    if (!test_name_.empty() && case_result_ && case_framework_ != "GoogleTest") test_result(case_framework_, test_name_, case_result_, case_result_ == 1 ? std::string_view{} : case_message_);
    test_name_.clear();
    case_framework_.clear();
    case_result_ = 0;
    case_message_.clear();
}

void BuildScan::test_case(std::string_view framework, std::string_view name) {
    test_case_end();
    test_name_ = std::string(name.substr(0, 300));
    case_framework_ = std::string(framework);
}

// GoogleTest's lines in brackets (and doctest's count).
bool BuildScan::gtest_line(std::string_view text) {
    if (text.starts_with("[doctest] ")) {
        if (text.substr(10).starts_with("test cases:")) {
            test_case_end();
            u32 p = 0, f = 0, s = 0;
            counts(text.substr(21), &p, &f, &s);
            tests_counted(p, f, s);
        }
        return true;
    }
    if (text.size() < 13 || text[11] != ']' || text[12] != ' ') return false;
    const std::string_view tag = text.substr(0, 13);
    std::string_view rest = text.substr(13);
    if (tag == "[ RUN      ] ") {
        test_case("GoogleTest", rest);
        return true;
    }
    if (tag == "[==========] " || tag == "[----------] ") return true;
    const bool ok = tag == "[       OK ] ", failed = tag == "[  FAILED  ] ", skipped = tag == "[  SKIPPED ] ", passed = tag == "[  PASSED  ] ";
    if (!ok && !failed && !skipped && !passed) return false;
    // Its count: "[  PASSED  ] 3 tests.", "[  FAILED  ] 1 test, listed below:".
    if (!rest.empty() && digit(rest[0]) && (rest.ends_with(" test.") || rest.ends_with(" tests.") || rest.ends_with(", listed below:"))) {
        const u32 n = number(rest);
        tests_counted(passed ? n : 0, failed ? n : 0, skipped ? n : 0);
        return true;
    }
    // A test's result, with the time it took ("Suite.Name (0 ms)"; listed
    // again at the end without it, and not taken again).
    if (passed || !rest.ends_with(" ms)")) return true;
    rest = rest.substr(0, rest.rfind(" ("));
    if (const size_t where = rest.find(", where "); where != std::string_view::npos) rest = rest.substr(0, where);  // (a parameterised test's: ", where GetParam() = 3")
    test_failure_end();
    const bool its = rest == test_name_;
    test_result("GoogleTest", rest, ok ? 1 : failed ? 2 : 3, its && !ok ? std::string_view(case_message_) : std::string_view{});
    if (its) test_name_.clear(), case_message_.clear(), case_result_ = 0;
    return true;
}

bool BuildScan::test_line(std::string_view text) {
    // What a failure goes on to say, to the blank line that ends it (or a
    // line that is plainly something else: at most 40 of them).
    if (fail_) {
        const bool other = text.starts_with('[') || rule(text, '=') || rule(text, '-') || ++fail_lines_ > 40;
        if (!text.empty() && !other) {
            join(fail_message_, text);
            return true;
        }
        test_failure_end();
        if (text.empty()) return true;
    }
    if (skip_) {  // (GoogleTest's reason for a skip)
        if (!text.empty() && !text.starts_with('[') && ++fail_lines_ <= 20) return true;
        skip_ = false;
        if (text.empty()) return true;
    }
    if (text.empty()) return false;
    switch (text[0]) {
        case '[': return gtest_line(text);
        case '=':
            if (rule(text, '=')) test_case_end();  // (doctest's between its test cases, Catch2's over its count)
            break;
        case '-':
            if (rule(text, '-')) {  // (Catch2's over and under a test case's name)
                heading_ = heading_ == 2 ? 3 : 1;
                return false;
            }
            break;
        case '.':
            if (rule(text, '.') && heading_ == 4) {  // (and under its place)
                heading_ = 0;
                test_case("Catch2", heading_name_);
                return false;
            }
            break;
        case 'T':
            if (text.starts_with("TEST CASE:  ")) {  // doctest's, under its place
                test_case("doctest", trim(text.substr(11)));
                return true;
            }
            break;
        case 't':
        case 'A':
            if (text.starts_with("test cases: ")) {  // Catch2's count
                test_case_end();
                u32 p = 0, f = 0, s = 0;
                counts(text.substr(12), &p, &f, &s);
                tests_counted(p, f, s);
                return true;
            }
            if (text.starts_with("All tests passed (")) {  // (all of them: "(3 assertions in 2 test cases)")
                test_case_end();
                const size_t in = text.find(" in ");
                tests_counted(in == std::string_view::npos ? 0 : number(text.substr(in + 4)), 0, 0);
                return true;
            }
            if (text.starts_with("assertions: ")) return true;
            break;
        default: break;
    }
    if (digit(text[0]) || text[0] == ' ') {
        const std::string_view t = trim(text);
        // Catch2's time for a test (--durations yes): "0.000 s: name", after what it said of it.
        if (const size_t s = t.find(" s: "); s != std::string_view::npos && s > 2 && t[s - 4] == '.' && digit(t[0])) {
            const std::string_view name = t.substr(s + 4);
            if (name == test_name_ && case_framework_ == "Catch2") {
                if (!case_result_) case_result_ = 1;
                test_case_end();
            } else {
                test_result("Catch2", name, 1);
            }
            return true;
        }
        // Unity's count: "3 Tests 1 Failures 1 Ignored".
        if (digit(t[0])) {
            std::string_view r = t;
            const std::string_view n0 = word(&r), w0 = word(&r), n1 = word(&r), w1 = word(&r), n2 = word(&r), w2 = word(&r);
            if (w0 == "Tests" && w1 == "Failures" && w2 == "Ignored" && all_digits(n0) && all_digits(n1) && all_digits(n2)) {
                const u32 all = number(n0), failed = number(n1), ignored = number(n2);
                tests_counted(all - std::min(all, failed + ignored), failed, ignored);
                return true;
            }
        }
        // CTest's: "1/3 Test #1: adds ......   Passed    0.05 sec", and its count.
        if (const size_t slash = t.find('/'); slash != std::string_view::npos && slash < 8 && all_digits(t.substr(0, slash))) {
            std::string_view r = t.substr(slash + 1);
            const std::string_view of = word(&r);
            if (all_digits(of) && word(&r) == "Test") {
                const std::string_view num = word(&r);
                if (num.starts_with('#') && num.ends_with(':')) {
                    const std::string_view name = word(&r);
                    r = trim(r);
                    while (!r.empty() && (r.front() == '.' || r.front() == ' ')) r.remove_prefix(1);
                    if (r.starts_with("***")) r.remove_prefix(3);
                    const u8 result = r.starts_with("Passed") ? 1 : r.starts_with("Skipped") || r.starts_with("Not Run") ? 3 : 2;
                    std::string_view status = r.substr(0, r.find("  "));
                    ctest_ = true;
                    (result == 1 ? ctest_passed_ : result == 2 ? ctest_failed_ : ctest_skipped_) += 1;
                    test_result("CTest", name, result, result == 1 ? std::string_view{} : status);
                    outcome_changed();
                    return true;
                }
            }
        }
        if (digit(t[0]) && t.find("% tests passed, ") != std::string_view::npos) {  // (quiet, with no line for each: the count from this)
            if (!ctest_) {
                const size_t f = t.find(", ") + 2, out = t.find(" out of ");
                if (out != std::string_view::npos) {
                    const u32 failed = number(t.substr(f)), all = number(t.substr(out + 8));
                    ctest_ = true;
                    ctest_failed_ = failed;
                    ctest_passed_ = all - std::min(all, failed);
                    outcome_changed();
                }
            }
            return true;
        }
    }
    if (size_line(text) || timing_line(text)) return true;
    // Catch2's heading: the test case's name (a section's under it, two
    // spaces in), then (under the dashes) its place.
    if (heading_ == 1) {
        heading_name_ = std::string(text.substr(0, 300));
        heading_ = 2;
        return false;
    }
    if (heading_ == 2 && !text.starts_with("  ")) heading_ = 0;
    if (heading_ == 3) {
        std::string_view path;
        u32 line = 0;
        heading_ = path_line(text, &path, &line) ? 4 : 0;
        if (heading_) return true;
    }
    // A place, and after it what the frameworks say there: the colons after
    // a line number (or doctest's "path(line):").
    for (size_t c = text.find(':'); c != std::string_view::npos && c < 2048; c = text.find(':', c + 1)) {
        if (c == 0 || (!digit(text[c - 1]) && text[c - 1] != ')')) continue;
        std::string_view path;
        u32 line = 0;
        if (!path_line(text.substr(0, c), &path, &line)) continue;
        const std::string_view rest = text.substr(c + 1);
        if (rest.empty()) return true;  // (doctest's over a TEST CASE line)
        if (rest == " Failure") {  // GoogleTest's
            test_failure(path, line, kGoogleTest, {});
            return true;
        }
        if (rest == " Skipped") {
            skip_ = true;
            fail_lines_ = 0;
            return true;
        }
        if (rest == " FAILED:" || rest == " SKIPPED:") {  // Catch2's
            test_failure(path, line, rest == " FAILED:" ? kCatch : kCatchSkip, {});
            return true;
        }
        if (rest == " PASSED:") {  // (Catch2 told to say each, -s: what passed, read as a failure's would be, and dropped)
            if (!case_result_) case_result_ = 1;
            skip_ = true;
            fail_lines_ = 0;
            return true;
        }
        if ((rest.starts_with(" ERROR: ") || rest.starts_with(" FATAL ERROR: ") || rest.starts_with(" WARNING: ")) && c_family(path)) {  // doctest's (Yosys's look the same, in a .v)
            const bool warn = rest.starts_with(" W");
            test_failure(path, line, warn ? kDoctestWarning : kDoctest, rest.substr(rest.find(": ") + 2));
            return true;
        }
        if (rest.starts_with(" SUCCESS: ")) {  // (doctest told to say each, -s)
            if (!case_result_) case_result_ = 1;
            skip_ = true;
            fail_lines_ = 0;
            return true;
        }
        // Unity's: "test_name:PASS", "test_name:FAIL: message", "test_name:IGNORE".
        size_t n = 0;
        while (n < rest.size() && (digit(rest[n]) || (rest[n] >= 'a' && rest[n] <= 'z') || (rest[n] >= 'A' && rest[n] <= 'Z') || rest[n] == '_')) ++n;
        if (n == 0 || n >= rest.size() || rest[n] != ':' || digit(rest[0])) continue;
        const std::string_view name = rest.substr(0, n), status = rest.substr(n + 1);
        const std::string_view said = status.find(": ") == std::string_view::npos ? std::string_view{} : trim(status.substr(status.find(": ") + 2));
        if (status == "PASS") {
            test_result("Unity", name, 1);
        } else if (status == "FAIL" || status.starts_with("FAIL: ")) {
            test_result("Unity", name, 2, said);
            add(path, line, 0, 1, std::string(name) + ": " + (said.empty() ? std::string("failed") : std::string(said)));
        } else if (status == "IGNORE" || status.starts_with("IGNORE: ")) {
            test_result("Unity", name, 3, said);
        } else {
            continue;
        }
        return true;
    }
    return false;
}

void BuildScan::test_end() {
    test_case_end();
    skip_ = false;
    heading_ = 0;
    size_table_ = 0;
    vivado_ = 0;
    // A clock nextpnr could not make as fast as it was asked for.
    for (const Clock& c : clocks_) {
        if (c.pass) continue;
        char buf[200];
        std::snprintf(buf, sizeof buf, "Timing not met: clock '%s' at %.2f MHz, %.2f MHz asked for (slack %+.2f ns)", c.name.c_str(), c.mhz, c.wanted,
                      c.mhz > 0 && c.wanted > 0 ? 1000.0 / c.wanted - 1000.0 / c.mhz : 0.0);
        add(constraints_.empty() ? std::string_view("nextpnr") : std::string_view(constraints_), 1, 0, c.error ? 1 : 2, buf);
    }
}

// ---------------------------------------------------------------- sizes

bool BuildScan::size_line(std::string_view text) {
    const std::string_view t = trim(text);
    // size's (Berkeley) table: "text data bss dec hex filename", a row for each file.
    if (t.starts_with("text ") && t.find(" bss ") != std::string_view::npos && t.ends_with("filename")) {
        size_table_ = 1;
        return true;
    }
    if (size_table_ == 1) {
        std::string_view r = t;
        const std::string_view text_n = word(&r), data_n = word(&r), bss_n = word(&r), dec_n = word(&r), hex_n = word(&r);
        if (all_digits(text_n) && all_digits(data_n) && all_digits(bss_n) && all_digits(dec_n) && !hex_n.empty() && !trim(r).empty()) {
            std::erase_if(outcome_.sizes, [](const SizeUsed& s) { return s.total == 0; });  // (the last file's: or the totals, -t)
            outcome_.sizes.push_back({"text", number(text_n), 0});
            outcome_.sizes.push_back({"data", number(data_n), 0});
            outcome_.sizes.push_back({"bss", number(bss_n), 0});
            outcome_changed();
            return true;
        }
        size_table_ = 0;
    }
    // The linker's: "Memory region  Used Size  Region Size  %age Used", then "FLASH:  4080 B  256 KB  1.56%".
    if (text.starts_with("Memory region ") && text.find("Used Size") != std::string_view::npos) {
        size_table_ = 2;
        std::erase_if(outcome_.sizes, [](const SizeUsed& s) { return s.total > 0; });  // (another link's)
        return true;
    }
    if (size_table_ == 2) {
        std::string_view r = t;
        const std::string_view name = word(&r), used = word(&r), used_unit = word(&r), total = word(&r), total_unit = word(&r), pct = word(&r);
        SizeUsed s;
        if (name.size() > 1 && name.ends_with(':') && pct.ends_with('%') && ld_size(used, used_unit, &s.used) && ld_size(total, total_unit, &s.total) && s.total > 0) {
            s.name = std::string(name.substr(0, name.size() - 1));
            outcome_.sizes.push_back(std::move(s));
            outcome_changed();
            return true;
        }
        size_table_ = 0;
    }
    return false;
}

// ---------------------------------------------------------------- timing

bool BuildScan::timing_line(std::string_view text) {
    // nextpnr's: "Info: Max frequency for clock 'clk$SB_IO_IN_$glb_clk': 48.31
    // MHz (PASS at 12.00 MHz)", once placed and again routed (the last is
    // the one); "ERROR: ..." when it fails, "Warning: ..." when let fail.
    static constexpr std::string_view kMax = "Max frequency for clock '";
    for (const std::string_view head : {"Info: ", "ERROR: ", "Warning: "}) {
        if (!text.starts_with(head) || !text.substr(head.size()).starts_with(kMax)) continue;
        std::string_view r = text.substr(head.size() + kMax.size());
        const size_t q = r.find("': ");
        if (q == std::string_view::npos) return false;
        const std::string_view full = r.substr(0, q);
        r = r.substr(q + 3);
        double mhz = 0, wanted = 0;
        if (!decimal(word(&r), &mhz) || word(&r) != "MHz") return false;
        const std::string_view verdict = word(&r);
        word(&r);  // ("at")
        decimal(word(&r), &wanted);
        Clock c;
        c.name = std::string(full.substr(0, std::min(full.find('$'), full.size())));  // (its net, made global: "clk$SB_IO_IN_$glb_clk" is clk)
        c.mhz = mhz;
        c.wanted = wanted;
        c.pass = verdict != "(FAIL";
        c.error = head == "ERROR: ";  // (nextpnr stopped for it: not let fail)
        const auto it = std::ranges::find(clocks_, c.name, &Clock::name);
        if (it != clocks_.end()) *it = std::move(c);
        else if (clocks_.size() < 32) clocks_.push_back(std::move(c));
        outcome_changed();
        return true;
    }
    // The constraints file the output names: Vivado's (not an IP's own, "for
    // cell"), nextpnr's as make echoes its command.
    if (text.starts_with("Parsing XDC File [")) {
        const size_t end = text.find(']');
        if (constraints_.empty() && end != std::string_view::npos && text.find("] for cell") == std::string_view::npos) constraints_ = std::string(text.substr(18, end - 18));
        return false;
    }
    const std::string_view first = text.substr(0, text.find(' '));  // (the command: "nextpnr-ice40", "./nextpnr-ice40", "/usr/bin/nextpnr-ecp5")
    if (first.substr(first.rfind('/') + 1).starts_with("nextpnr")) {
        for (const std::string_view opt : {" --sdc ", " --pre-pack ", " --pcf ", " --lpf ", " --cst "})
            if (const size_t at = text.find(opt); at != std::string_view::npos) {
                std::string_view r = text.substr(at + opt.size());
                constraints_ = std::string(word(&r));
                break;
            }
        return false;
    }
    // Vivado's timing summary: "WNS(ns)  TNS(ns)  TNS Failing Endpoints ...
    // WHS(ns) ...", dashes under it, and its figures.
    const std::string_view t = trim(text);
    if (t.starts_with("WNS(ns) ") && t.find("WHS(ns)") != std::string_view::npos) {
        vivado_ = 1;
        return true;
    }
    if (vivado_ == 1 && t.starts_with("-------")) {
        vivado_ = 2;
        return true;
    }
    if (vivado_ == 2) {
        vivado_ = 0;
        std::string_view r = t;
        double f[5] = {};
        bool ok = true;
        for (double& v : f) ok = ok && decimal(word(&r), &v);
        if (ok) {
            wns_ = f[0], tns_ = f[1], failing_ = static_cast<u32>(f[2]), whs_ = f[4];
            vivado_read_ = true;
            outcome_changed();
            return true;
        }
    }
    if (t == "Timing constraints are not met.") {
        char buf[160];
        if (vivado_read_) std::snprintf(buf, sizeof buf, "Timing constraints are not met: WNS %+.3f ns, TNS %+.3f ns (%u failing endpoints)", wns_, tns_, failing_);
        else std::snprintf(buf, sizeof buf, "Timing constraints are not met");
        add(constraints_.empty() ? std::string_view("Vivado") : std::string_view(constraints_), 1, 0, 2, buf);
        return true;
    }
    return t == "All user specified timing constraints are met.";
}

}  // namespace blit
