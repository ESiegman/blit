// Testing: the side panel's Tests tab lists the project's tests (CTest's,
// so GoogleTest, Catch2 and doctest tests registered with it, and
// pytest's), found on a thread of their own; each with how its last run
// went, read from the JUnit report the run writes. A run is a command in
// the output terminal, where its output shows; one test, the failed ones
// again, or all. A mark beside each test where it is defined, in its
// last result's colour, runs it when clicked; a CTest test can be debugged.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include "app.hpp"
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "tests.hpp"

namespace blit {

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 22;
    SDL_PushEvent(&ev);
}

// A name as a regular expression matches it alone (CTest's -R).
std::string exactly(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (std::string_view(".[]()*+?^$\\|{}").find(c) != std::string_view::npos) out += '\\';
        out += c;
    }
    return out;
}

bool python_path(std::string_view p) { return p.ends_with(".py"); }

// A path as a command run in a folder on another machine names it: without
// ssh://host. (One here, as it is.)
std::string on_host(const std::string& path) {
    std::string host, there;
    return split_remote(path, &host, &there) ? there : path;
}

// How a folder's Python (App::python_for: its venv's, uv's in a uv
// project) runs a module of its, pytest or coverage; with no project's
// Python, the program on the PATH, else python3's module.
std::string python_module(const std::string& python, const char* module) {
    if (python != "python3") return python + " -m " + module;
    return std::string("$(command -v ") + module + " || echo python3 -m " + module + ")";  // (unquoted: the second, three words)
}
bool cpp_path(std::string_view p) {
    for (const char* e : {".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".hh", ".cu", ".hip", ".mm"})
        if (p.ends_with(e)) return true;
    return false;
}

}  // namespace

struct App::TestUi {
    std::vector<TestCase> tests;
    std::string root;  // the folder they were found in
    std::string python;  // and its Python, as found then (App::python_for: pytest's are run with it)
    bool looked = false, looking = false;
    std::string note;  // what finding them said, when there are none
    struct Found {
        std::string root, python;
        std::vector<TestCase> tests;
        std::string note;
    };
    std::mutex mu;
    std::optional<Found> found;  // (mu)
    // The run going on: its output terminal, and the reports it writes.
    u64 term = 0;
    std::vector<std::pair<std::string, std::string>> reports;  // (each with the build folder it is CTest's for; pytest's: none)
    bool running = false;
    // The tab's rows: a group's heading (test -1) or a test; the one chosen.
    struct Row {
        int test = -1;
        std::string group;
        u32 count = 0;
        int file = -1;  // (the coverage of one: cov_files' index)
    };
    std::vector<Row> rows;
    u32 sel = 0, top = 0, body = 0;
    // Each document's tests where they are defined (line, test or -1), as
    // its text and the tests last were.
    struct Defs {
        u64 rev = ~0ull, gen = ~0ull;
        std::vector<std::pair<u32, int>> at;
    };
    std::unordered_map<u64, Defs> defs;
    u64 runs = 0;
    // A run asked for before the tests were found: the ids of those asked
    // for (empty: all), run once they are.
    std::optional<std::vector<std::string>> pending;
    bool pending_here = false;  // (or the one at the cursor)
    bool pending_coverage = false;  // (and whether with coverage)
    // Coverage: the reports a run with coverage writes (gcov's from a build
    // folder, coverage.py's), and what the last such run ran, by file.
    struct CovReport {
        std::string path, dir;
        bool python = false;
    };
    std::vector<CovReport> cov_reports;
    // On another machine: the build folders' CMakeCache.txt, read there
    // before a run with coverage starts; and the errands reading reports
    // back after a run (one for each run that has ended, until each is back).
    std::optional<std::unordered_map<std::string, std::string>> caches;
    bool preparing = false;
    u32 reading = 0;
    std::unordered_map<std::string, FileCoverage> coverage;
    std::vector<std::string> cov_files;  // (their paths, in order, for the tab)
    bool cov_shown = false;
    std::jthread worker;  // (last: joined first)
};

void App::test_create() { testui_ = std::make_shared<TestUi>(); }

// The folder's tests found again, on a thread: CTest's in every build
// folder that has been configured, pytest's where there is Python to test.
void App::test_find() {
    TestUi& u = *testui_;
    const std::string root = build_root();
    if (root.empty() || u.looking) return;
    if (u.worker.joinable()) u.worker.join();
    u.looking = true;
    ++test_gen_;
    u.worker = std::jthread([&u, root] {
        TestUi::Found f;
        f.root = root;
        auto configured = [](const std::string& d) { return fs_kind(d + "/CTestTestfile.cmake") == 1; };
        std::vector<std::string> dirs;
        if (configured(root)) dirs.push_back(root);
        bool python = false;
        for (const FsEntry& e : fs_list(root)) {
            if (!e.dir) {
                python = python || e.name == "pytest.ini" || e.name == "conftest.py" || (e.name.starts_with("test_") && e.name.ends_with(".py"));
                continue;
            }
            const std::string d = root + "/" + e.name;
            if (e.name == "tests" || e.name == "test")
                for (const FsEntry& t : fs_list(d)) python = python || (t.name.starts_with("test_") && t.name.ends_with(".py"));
            if (configured(d)) dirs.push_back(d);
            else if (e.name.starts_with("build") || e.name.starts_with("cmake-build") || e.name == "out")  // (one level in: a preset's build/debug)
                for (const FsEntry& s : fs_list(d))
                    if (s.dir && configured(d + "/" + s.name)) dirs.push_back(d + "/" + s.name);
        }
        std::string host, there;
        const bool remote = split_remote(root, &host, &there);
        for (const std::string& d : dirs) {
            std::string out;
            if (fs_run(root, "ctest --show-only=json-v1 --test-dir " + sh_word(on_host(d)), &out, 30000))
                for (TestCase& t : parse_ctest_json(out, d)) {
                    if (remote) {  // (what it runs, and where, are paths there: debugged there)
                        if (!t.command.empty() && t.command[0].starts_with('/')) t.command[0] = remote_path(host, t.command[0]);
                        if (t.cwd.starts_with('/')) t.cwd = remote_path(host, t.cwd);
                    }
                    f.tests.push_back(std::move(t));
                }
        }
        std::string said;  // (pytest's last line, or its Python's: why none were found)
        if (python) {
            f.python = App::python_for(root, false);
            std::string host, there;  // (another machine's: its path there)
            if (const size_t at = f.python.find(root); at != std::string::npos && split_remote(root, &host, &there)) f.python.replace(at, root.size(), there);
            std::string out;
            fs_run(root, python_module(f.python, "pytest") + " --collect-only -q 2>&1", &out, 60000);
            for (TestCase& t : parse_pytest_collect(out)) f.tests.push_back(std::move(t));
            while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
            said = out.substr(out.find_last_of('\n') + 1);  // (npos + 1: all of it)
        }
        if (f.tests.empty())
            f.note = dirs.empty() && !python ? "No tests found: CTest's are found in a build folder that has been configured, pytest's beside a test_*.py"
                     : said.empty()          ? "No tests found"
                                             : "No tests found: " + said;
        {
            std::lock_guard lock(u.mu);
            u.found = std::move(f);
        }
        wake();
    });
}

void App::test_sync() {
    TestUi& u = *testui_;
    std::optional<TestUi::Found> f;
    {
        std::lock_guard lock(u.mu);
        f.swap(u.found);
    }
    if (!f) return;
    u.looking = false;
    u.looked = true;
    // How the tests that are still there went last time stays.
    for (TestCase& t : f->tests)
        for (const TestCase& old : u.tests)
            if (old.runner == t.runner && old.id == t.id) {
                t.result = old.result;
                t.message = old.message;
                t.seconds = old.seconds;
                break;
            }
    u.tests = std::move(f->tests);
    u.root = f->root;
    u.python = std::move(f->python);
    u.note = std::move(f->note);
    u.sel = 0;
    ++test_gen_;
    dirty_ = true;
    if (auto ids = std::exchange(u.pending, std::nullopt)) {  // (a run asked for meanwhile)
        std::vector<size_t> which;
        for (size_t i = 0; i < u.tests.size(); ++i)
            if (std::ranges::find(*ids, u.tests[i].id) != ids->end()) which.push_back(i);
        if (ids->empty() || !which.empty()) test_run(which, std::exchange(u.pending_coverage, false));
    }
    if (std::exchange(u.pending_here, false)) test_run_here();
}

// Runs the tests given (all, if none), each runner's in one command, one
// after another in the output terminal, each writing its report; the run
// configuration's environment theirs too.
void App::test_run(const std::vector<size_t>& which, bool coverage) {
    TestUi& u = *testui_;
    if (!u.looked || u.looking) {  // (found first; then it runs)
        std::vector<std::string> ids;
        for (const size_t i : which)
            if (i < u.tests.size()) ids.push_back(u.tests[i].id);
        u.pending = std::move(ids);
        u.pending_coverage = coverage;
        test_find();
        return notify("Looking for the tests\xE2\x80\xA6");
    }
    if (u.tests.empty()) return notify(u.note.empty() ? "No tests found" : u.note);
    std::vector<size_t> pick = which;
    if (pick.empty())
        for (size_t i = 0; i < u.tests.size(); ++i) pick.push_back(i);
    std::string env;
    if (const BuildTask* c = run_config(); c && !c->env.empty()) {
        env = "env";
        for (const std::string& e : c->env) env += " " + sh_word(e);
        env += " ";
    }
    const bool all = which.empty();
    // CTest: each build folder's in one command; pytest's in one.
    std::vector<std::string> dirs;
    for (const size_t i : pick)
        if (u.tests[i].runner == TestCase::kCTest && std::ranges::find(dirs, u.tests[i].dir) == dirs.end()) dirs.push_back(u.tests[i].dir);
    // The reports are written where the tests run: on another machine, in
    // blit's cache there (read back once the run has ended, test_ended).
    std::string host, there, tmp;
    const bool remote = split_remote(u.root, &host, &there);
    if (remote) {
        if (u.preparing) return;  // (a run is on its way: pressed again meanwhile, it would run twice)
        if (coverage && !dirs.empty() && !u.caches) {  // (the folders' compilers first, read there: then it runs)
            u.preparing = true;
            remote_errand([this, dirs, which, coverage](std::stop_token stop) -> std::function<void()> {
                std::unordered_map<std::string, std::string> caches;
                std::string why;
                for (const std::string& d : dirs) fs_read_whole(d + "/CMakeCache.txt", &caches[d], &why, 16u << 20, stop);
                return [this, caches = std::move(caches), which, coverage] {
                    testui_->preparing = false;
                    testui_->caches = std::move(caches);
                    test_run(which, coverage);
                };
            });
            return;
        }
        std::string in;
        const std::shared_ptr<Remote> r = remote_for(u.root, &in);
        tmp = remote_path(host, r && !r->home().empty() ? r->home() + "/.cache/blit" : std::string("/tmp"));
    } else {
        std::error_code ec;
        tmp = std::filesystem::temp_directory_path(ec).string();
        if (tmp.empty()) tmp = "/tmp";
    }
    std::vector<std::string> parts;
    u.reports.clear();
    u.cov_reports.clear();
    auto report = [&](const std::string& of = {}) {
        u.reports.emplace_back(tmp + "/blit-tests-" + std::to_string(::getpid()) + "-" + std::to_string(++u.runs) + ".xml", of);
        return u.reports.back().first;
    };
    auto here = [](const std::string& path) { return sh_word(on_host(path)); };  // (a path as the command there names it)
    for (const std::string& d : dirs) {
        std::string names;
        for (const size_t i : pick)
            if (u.tests[i].runner == TestCase::kCTest && u.tests[i].dir == d) names += (names.empty() ? "" : "|") + exactly(u.tests[i].id);
        std::string gcov;  // (with coverage: the gcov of the compiler the folder builds with, GCC's)
        std::string compiler;  // (and where that one says its gcov is, when it is not on the PATH)
        if (coverage) {
            std::string cache;
            if (u.caches) cache = (*u.caches)[d];
            else fs_read(d + "/CMakeCache.txt", &cache);
            const size_t at = cache.find("CMAKE_CXX_COMPILER:");
            std::string cc = at == std::string::npos ? std::string() : cache.substr(cache.find('=', at) + 1, cache.find('\n', at) - cache.find('=', at) - 1);
            const size_t slash = cc.rfind('/'), gpp = cc.rfind("g++");
            if (gpp != std::string::npos && (slash == std::string::npos || gpp > slash)) {
                compiler = cc;
                gcov = cc.replace(gpp, 3, "gcov");
            } else if (cc.empty() || cc.ends_with("/c++")) gcov = "gcov";
            else notify("Coverage is read with GCC's gcov: " + d.substr(d.rfind('/') + 1) + " builds with " + cc.substr(slash + 1));
        }
        const std::string clear = gcov.empty() ? std::string() : "find " + here(d) + " -name '*.gcda' -delete; ";  // (this run's alone)
        parts.push_back(clear + env + "ctest --test-dir " + here(d) + " --output-on-failure --output-junit " + here(report(d)) + (all ? "" : " -R " + sh_word("^(" + names + ")$")));
        if (!gcov.empty()) {
            const std::string out = report();
            u.reports.pop_back();
            u.cov_reports.push_back({out, d, false});
            // (Arch's gcc15 has g++-15 but its gcov only in GCC's own folder, which -print-prog-name knows.)
            const std::string run = compiler.empty() ? sh_word(gcov) : "\"$(command -v " + sh_word(gcov) + " || " + sh_word(compiler) + " -print-prog-name=gcov)\"";
            parts.push_back("(cd " + here(d) + " && " + run + " --json-format --stdout $(find . -name '*.gcda') > " + here(out) + " 2>/dev/null)");
        }
    }
    u.caches.reset();
    std::string ids;
    for (const size_t i : pick)
        if (u.tests[i].runner == TestCase::kPytest) ids += " " + sh_word(u.tests[i].id);
    if (!ids.empty()) {
        if (coverage) {  // (through coverage.py, which writes what ran as JSON after)
            const std::string cov = python_module(u.python, "coverage");
            parts.push_back(env + cov + " run -m pytest -q --junitxml=" + here(report()) + (all ? "" : ids));
            const std::string out = report();
            u.reports.pop_back();
            u.cov_reports.push_back({out, u.root, true});
            parts.push_back(env + cov + " json -q -o " + here(out));  // (in the run configuration's environment too)
        } else {
            parts.push_back(env + python_module(u.python, "pytest") + " -q --junitxml=" + here(report()) + (all ? "" : ids));
        }
    }
    std::string cmd;
    for (const std::string& p : parts) cmd += (cmd.empty() ? "" : "; ") + p;
    u.term = build_command(u.root, cmd, pick.size() == 1 ? u.tests[pick[0]].name : "Tests");
    u.running = true;
    ++test_gen_;
    dirty_ = true;
}

void App::test_run_failed() {
    TestUi& u = *testui_;
    std::vector<size_t> failed;
    for (size_t i = 0; i < u.tests.size(); ++i)
        if (u.tests[i].result == TestCase::kFailed) failed.push_back(i);
    if (failed.empty()) return notify("No test has failed");
    test_run(failed);
}

// The run's terminal has ended: its reports say how each test went. Here
// they are read at once; from another machine, on an errand there (which
// takes them away there too), and taken in when they have come.
void App::test_ended(u64 doc) {
    TestUi& u = *testui_;
    if (!u.running || doc != u.term) return;
    u.running = false;
    std::vector<std::pair<std::string, u64>> paths;  // (the reports', then the coverage reports', with how large each may be)
    for (const auto& [r, of] : u.reports) paths.emplace_back(r, 64u << 20);
    for (const TestUi::CovReport& r : u.cov_reports) paths.emplace_back(r.path, 256u << 20);
    if (!is_remote(u.root)) {
        std::vector<std::string> texts(paths.size());
        for (size_t i = 0; i < paths.size(); ++i) {
            fs_read(paths[i].first, &texts[i], paths[i].second);
            std::error_code ec;
            std::filesystem::remove(paths[i].first, ec);
        }
        return test_take(std::move(texts));
    }
    ++u.reading;
    remote_errand([this, paths, root = u.root, run = u.runs](std::stop_token stop) -> std::function<void()> {
        std::vector<std::string> texts(paths.size());
        std::string why, gone = "rm -f", out;
        for (size_t i = 0; i < paths.size(); ++i) {
            if (!fs_read_whole(paths[i].first, &texts[i], &why, paths[i].second, stop)) texts[i].clear();
            gone += " " + sh_word(on_host(paths[i].first));
        }
        if (!paths.empty()) fs_run(root, gone, &out);
        return [this, texts = std::move(texts), run] mutable {
            --testui_->reading;
            if (testui_->runs == run) test_take(std::move(texts));  // (not if another run has started since)
        };
    });
}

// The reports' text (as test_ended read them, in its order) taken in.
void App::test_take(std::vector<std::string> texts) {
    TestUi& u = *testui_;
    std::string host, there;
    const bool remote = split_remote(u.root, &host, &there);
    u32 passed = 0, failed = 0;
    std::string uncollected;  // (the first module pytest could not collect, and why: the run stopped there)
    u32 more = 0;
    size_t k = 0;
    texts.resize(u.reports.size() + u.cov_reports.size());
    for (const auto& [r, of] : u.reports) {
        const std::string& xml = texts[k++];
        for (const TestOutcome& o : parse_junit(xml)) {
            bool known = false;
            for (TestCase& t : u.tests)
                if ((t.runner == TestCase::kCTest ? t.dir == of : of.empty()) && outcome_is(o, t)) {  // (two build folders have tests of the same name)
                    t.result = o.result;
                    t.message = o.message;
                    t.seconds = o.seconds;
                    passed += o.result == TestCase::kPassed;
                    failed += o.result == TestCase::kFailed;
                    known = true;
                    break;
                }
            if (known || !of.empty() || !o.classname.empty() || o.result != TestCase::kFailed) continue;
            if (!uncollected.empty()) {
                ++more;
                continue;
            }
            std::string file = o.name;  // ("tests.test_a": tests/test_a.py)
            std::ranges::replace(file, '.', '/');
            uncollected = "pytest could not collect " + file + ".py" + (o.message.empty() ? std::string() : ": " + o.message);
        }
    }
    u.reports.clear();
    if (more) uncollected += " (and " + std::to_string(more) + " more)";
    // What a run with coverage ran.
    if (!u.cov_reports.empty()) {
        u.coverage.clear();
        u.cov_files.clear();
        size_t hit = 0, all = 0;
        const bool python = std::ranges::all_of(u.cov_reports, &TestUi::CovReport::python);  // (coverage.py's alone: what to ask of the Python)
        for (const TestUi::CovReport& r : u.cov_reports) {
            const std::string& text = texts[k++];
            if (!text.empty())
                for (auto& [whole, c] : r.python ? parse_coverage_py(text, on_host(r.dir)) : parse_gcov_json(text, on_host(r.dir))) {
                    const std::string path = remote && whole.starts_with('/') ? remote_path(host, whole) : whole;  // (another machine's: its paths there)
                    if (!path.starts_with(u.root + "/") || path.find("/_deps/") != std::string::npos || path.find("/CMakeFiles/") != std::string::npos) continue;  // (the project's own)
                    hit += c.hit.size();
                    all += c.hit.size() + c.missed.size();
                    u.cov_files.push_back(path);
                    u.coverage[path] = std::move(c);
                }
        }
        u.cov_reports.clear();
        std::ranges::sort(u.cov_files);
        u.cov_files.erase(std::ranges::unique(u.cov_files).begin(), u.cov_files.end());
        u.cov_shown = !u.coverage.empty();
        notify(!uncollected.empty() ? uncollected
               : u.coverage.empty() ? std::string(python ? "No coverage was recorded (coverage.py installed for " + python_named(u.python, u.root) + "?)" : "No coverage was recorded (built with --coverage?)")
                                    : "Coverage: " + std::to_string(static_cast<int>(100.0 * static_cast<double>(hit) / static_cast<double>(std::max<size_t>(all, 1)) + 0.5)) + "% of the lines in " + std::to_string(u.cov_files.size()) + " files");
        ++test_gen_;
        dirty_ = true;
        return;
    }
    if (!uncollected.empty()) notify(uncollected);
    else if (passed + failed) notify(failed ? std::to_string(failed) + " failed, " + std::to_string(passed) + " passed" : "All " + std::to_string(passed) + " passed");
    ++test_gen_;
    dirty_ = true;
}

// ---------------------------------------------------------------- in the text

// The tests defined in a document, where (line, test or -1), kept until its
// text or the tests change.
const std::vector<std::pair<u32, int>>& App::test_defs(const Doc& d) {
    TestUi& u = *testui_;
    TestUi::Defs& x = u.defs[d.id];
    if (x.rev == d.ed.revision() && x.gen == test_gen_) return x.at;
    x.rev = d.ed.revision();
    x.gen = test_gen_;
    x.at.clear();
    const std::string& path = d.ed.path;
    const bool py = python_path(path);
    // (Test files are small: a larger file is not looked through, nor any
    // once the project is known to have no tests.)
    if ((!py && !cpp_path(path)) || d.ed.buf.size() > (256u << 10) || (u.looked && u.tests.empty())) return x.at;
    const std::string text = d.ed.buf.text(0, d.ed.buf.size());
    const std::string full = full_path(path);
    const std::vector<std::pair<u32, std::string>> defs = find_test_defs(text, py);
    if (!defs.empty() && !u.looked && !u.looking) test_find();  // (a file with tests in it: the project's are looked for)
    // (A parametrised pytest test is a case for each of its parameters,
    // "test_x[1-2]": each is the def's, one after another. Its id matched
    // whole: a module's test_x is not its TestK::test_x.)
    auto unparametrised = [](std::string_view id) { return id.ends_with(']') ? id.substr(0, id.find('[', id.find("::"))) : id; };
    for (const auto& [line, name] : defs) {
        const size_t before = x.at.size();
        for (size_t i = 0; i < u.tests.size(); ++i) {
            const TestCase& t = u.tests[i];
            if (py ? t.runner == TestCase::kPytest && unparametrised(t.id) == t.group + "::" + name && full.ends_with("/" + t.group)
                   : t.runner == TestCase::kCTest && (t.id == name || t.id.ends_with("." + name) || t.id.ends_with("/" + name))) {
                x.at.emplace_back(line, static_cast<int>(i));
                if (!py) break;
            }
        }
        if (x.at.size() == before) x.at.emplace_back(line, -1);
    }
    return x.at;
}

// The first gutter cell of a line a test is defined on: how its last run
// went (a tick, a cross; an open triangle, not run yet). A parametrised
// test's: a cross if a case failed, a tick if all passed.
u32 App::test_gutter(const Doc& d, u64 line) {
    if (!testui_ || d.ed.path.empty() || line > ~0u) return 0;
    const std::vector<std::pair<u32, int>>& at = test_defs(d);  // (in the order of their lines: a def's cases together)
    bool any = false, failed = false, passed = true;
    for (auto it = std::ranges::lower_bound(at, static_cast<u32>(line), {}, &std::pair<u32, int>::first); it != at.end() && it->first == line; ++it) {
        if (it->second < 0) continue;
        const TestCase& t = testui_->tests[static_cast<size_t>(it->second)];
        any = true;
        failed = failed || t.result == TestCase::kFailed;
        passed = passed && t.result == TestCase::kPassed;
    }
    if (!any) return 0;
    const u32 cp = failed ? 0x2717 : passed ? 0x2713 : 0x25B7;
    const u32 in = failed ? kInkDiagnostic : passed ? kInkAdded : kInkMuted;
    return (font_->glyph(cp) & (Font::kWide - 1)) | (in << kCellInkShift);
}

// A click on that mark runs the test (debugs it, with Option held).
bool App::test_gutter_click(const SDL_MouseButtonEvent& b) {
    if (!testui_ || testui_->tests.empty() || b.button != SDL_BUTTON_LEFT || !cfg_.line_numbers) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_), cw = font_->cell_w(), ch = font_->cell_h();
    if (px < pane_->x0 || px >= pane_->x0 + cw + look().edge || py < pane_->text_y || py >= pane_->y1) return false;
    const i64 y = py - pane_->text_y + std::llround(pane_->scroll_y);
    if (y < 0 || static_cast<u64>(y / ch) >= view_line_count()) return false;
    const u64 line = line_of_row(static_cast<u64>(y / ch));
    std::vector<size_t> which;  // (a parametrised test's: all its cases)
    for (const auto& [l, i] : test_defs(*pane_->doc))
        if (l == line && i >= 0) which.push_back(static_cast<size_t>(i));
    if (which.empty()) return false;
    if (SDL_GetModState() & SDL_KMOD_ALT) test_debug(which[0]);
    else test_run(which);
    return true;
}

// The test defined at or above the cursor, in the file in front (a
// parametrised one's cases; none: the nearest such definition is not a
// test the runner has).
std::vector<size_t> App::test_at_cursor() {
    const Doc& d = *pane_->doc;
    const u64 line = d.ed.buf.line_of(d.ed.cursor());
    std::vector<size_t> out;
    u32 at = ~0u;
    for (const auto& [l, i] : test_defs(d)) {
        if (l > line) break;
        if (l != at) out.clear(), at = l;
        if (i >= 0) out.push_back(static_cast<size_t>(i));
    }
    return out;
}

void App::test_run_here() {
    TestUi& u = *testui_;
    if (!u.looked || u.looking) {  // (found first; then it runs)
        u.pending_here = true;
        test_find();
        return notify("Looking for the tests\xE2\x80\xA6");
    }
    const std::vector<size_t> which = test_at_cursor();
    if (which.empty()) return notify("No test here");
    test_run(which);
}

void App::test_debug(size_t i) {
    TestUi& u = *testui_;
    if (i >= u.tests.size()) return;
    const TestCase& t = u.tests[i];
    if (t.runner != TestCase::kCTest || t.command.empty()) return notify("Only CTest's tests can be debugged as yet");
    std::string args;
    for (size_t k = 1; k < t.command.size(); ++k) args += (args.empty() ? "" : " ") + sh_word(t.command[k]);
    std::vector<std::string> env;
    if (const BuildTask* c = run_config()) env = c->env;
    debug_take(t.command[0], args, t.cwd.empty() ? t.dir : t.cwd, env);
    debug_start();
}

void App::test_debug_here() {
    const std::vector<size_t> which = test_at_cursor();
    if (which.empty()) return notify("No test here");
    test_debug(which[0]);
}

const FileCoverage* App::test_coverage(const Doc& d) {
    TestUi& u = *testui_;
    if (!u.cov_shown || d.ed.path.empty() || d.ed.modified()) return nullptr;
    const auto it = u.coverage.find(full_path(d.ed.path));
    return it == u.coverage.end() ? nullptr : &it->second;
}

void App::test_coverage_toggle() {
    TestUi& u = *testui_;
    if (u.coverage.empty()) return notify("No coverage yet: Run the tests with coverage");
    u.cov_shown = !u.cov_shown;
    ++test_gen_;
    dirty_ = true;
}

// For the tests: the file in front's lines run and missed, and each file's share.
std::string App::coverage_dump() {
    TestUi& u = *testui_;
    std::string out = "here:";
    if (const FileCoverage* c = test_coverage(*pane_->doc)) {
        out += " ran";
        for (const u32 l : c->hit) out += " " + std::to_string(l);
        out += ", missed";
        for (const u32 l : c->missed) out += " " + std::to_string(l);
    } else {
        out += " none";
    }
    out += " | files:";
    for (const std::string& f : u.cov_files) out += " " + f.substr(u.root.size() + 1) + "=" + std::to_string(static_cast<int>(u.coverage[f].percent + 0.5)) + "%";
    return out;
}

// ---------------------------------------------------------------- the tab

void App::test_panel_draw(TextBox& box, bool focused) {
    TestUi& u = *testui_;
    Font& font = *font_;
    const u32 cols = box.cols, rows = box.rows;
    if (!u.looked && !u.looking) test_find();
    u.rows.clear();
    for (size_t i = 0; i < u.tests.size(); ++i) {
        const TestCase& t = u.tests[i];
        if (u.rows.empty() || u.tests[i - 1].group != t.group || u.tests[i - 1].runner != t.runner)
            u.rows.push_back({-1, t.group + (t.runner == TestCase::kCTest ? " (CTest)" : " (pytest)"), 0});
        for (size_t h = u.rows.size(); h-- > 0;)
            if (u.rows[h].test < 0) {
                ++u.rows[h].count;
                break;
            }
        u.rows.push_back({static_cast<int>(i), {}, 0});
    }
    if (u.cov_shown && !u.cov_files.empty()) {  // what the last run with coverage ran: a file a row, with its share
        size_t hit = 0, all = 0;
        for (const auto& [p, c] : u.coverage) hit += c.hit.size(), all += c.hit.size() + c.missed.size();
        u.rows.push_back({-1, "Coverage " + std::to_string(static_cast<int>(100.0 * static_cast<double>(hit) / static_cast<double>(std::max<size_t>(all, 1)) + 0.5)) + "%", static_cast<u32>(u.cov_files.size())});
        for (size_t i = 0; i < u.cov_files.size(); ++i) u.rows.push_back({-1, {}, 0, static_cast<int>(i)});
    }
    if (u.tests.empty()) {
        const std::string what = u.looking ? "Looking for tests\xE2\x80\xA6" : u.note;
        u32 r = 1;
        for (size_t at = 0; at < what.size() && r < rows;) {  // (in lines that fit)
            size_t len = std::min<size_t>(what.size() - at, cols);
            if (at + len < what.size()) {
                const size_t sp = what.rfind(' ', at + len);
                if (sp != std::string::npos && sp > at) len = sp - at;
            }
            put(box, font, r++, 0, std::string_view(what).substr(at, len), ink::kMuted);
            at += len;
            while (at < what.size() && what[at] == ' ') ++at;
        }
        return;
    }
    u32 passed = 0, failed = 0;
    for (const TestCase& t : u.tests) passed += t.result == TestCase::kPassed, failed += t.result == TestCase::kFailed;
    std::string head = std::to_string(u.tests.size()) + " tests";
    if (passed + failed) head += " \xC2\xB7 " + std::to_string(passed) + " passed, " + std::to_string(failed) + " failed";
    if (u.running) head += " \xC2\xB7 running\xE2\x80\xA6";
    put(box, font, 1, 0, fit_text(head, cols), failed ? ink::kError : ink::kMuted);
    static constexpr Hint kKeys[] = {{"Enter", "run"}, {"a", "all"}, {"f", "failed"}, {"c", "coverage"}, {"d", "debug"}};
    if (u.sel >= u.rows.size() || (u.rows[u.sel].test < 0 && u.rows[u.sel].file < 0)) u.sel = u.rows.size() > 1 ? 1 : 0;
    u.body = rows > 4 ? rows - 4 : 0;
    if (u.sel < u.top) u.top = u.sel;
    if (u.body && u.sel >= u.top + u.body) u.top = u.sel - u.body + 1;
    for (u32 r = 0; r < u.body && u.top + r < u.rows.size(); ++r) {
        const TestUi::Row& row = u.rows[u.top + r];
        const u32 y = 2 + r;
        if (row.file >= 0) {
            const std::string& path = u.cov_files[static_cast<size_t>(row.file)];
            const FileCoverage& c = u.coverage[path];
            const std::string pct = std::to_string(static_cast<int>(c.percent + 0.5)) + "%";
            put(box, font, y, 0, pct, c.percent >= 80 ? ink::kAdded : c.percent >= 50 ? ink::kText : ink::kError);
            put(box, font, y, 5, fit_text(path.substr(u.root.size() + 1), cols > 5 ? cols - 5 : 1, true), ink::kText);
        } else if (row.test < 0) {
            const u32 c = put(box, font, y, 0, fit_text(row.group, cols > 4 ? cols - 4 : 1, true), ink::kHeading);
            put(box, font, y, c + 1, std::to_string(row.count), ink::kMuted);
        } else {
            // How it went (in words where there is room, not only colour), and its name.
            const TestCase& t = u.tests[static_cast<size_t>(row.test)];
            const char* mark = t.result == TestCase::kPassed ? "\xE2\x9C\x93" : t.result == TestCase::kFailed ? "\xE2\x9C\x97" : t.result == TestCase::kSkipped ? "-" : "\xC2\xB7";
            put(box, font, y, 0, mark, t.result == TestCase::kPassed ? ink::kAdded : t.result == TestCase::kFailed ? ink::kError : ink::kMuted);
            const u32 c = put(box, font, y, 2, fit_text(t.name, cols > 2 ? cols - 2 : 1), ink::kText);
            std::string after = t.result == TestCase::kFailed && !t.message.empty() ? t.message : t.seconds > 0 ? std::to_string(static_cast<int>(t.seconds * 1000 + 0.5)) + " ms" : "";
            if (c + 4 < cols && !after.empty()) put(box, font, y, c + 2, fit_text(after, cols - c - 2), t.result == TestCase::kFailed ? ink::kError : ink::kMuted);
        }
        if (u.top + r == u.sel) {
            if (focused) box.select_row(y);
            else
                for (u32 k = 0; k < cols; ++k) box.cells[static_cast<size_t>(y) * cols + k] |= kCellHover;
        }
    }
    if (rows > 3 && hints_width(kKeys, font) <= cols) put_hints(box, font, rows - 1, 0, kKeys);
}

// The tests a row stands for: a test, or every one under a heading.
std::vector<size_t> App::test_row_tests(u32 row) {
    TestUi& u = *testui_;
    std::vector<size_t> out;
    if (row >= u.rows.size()) return out;
    if (u.rows[row].test >= 0) return {static_cast<size_t>(u.rows[row].test)};
    if (u.rows[row].file >= 0) return out;
    for (u32 r = row + 1; r < u.rows.size() && u.rows[r].test >= 0; ++r) out.push_back(static_cast<size_t>(u.rows[r].test));
    return out;
}

void App::test_panel_click(int row, int col, const SDL_MouseButtonEvent& b) {
    (void)col;
    TestUi& u = *testui_;
    if (b.button != SDL_BUTTON_LEFT || row < 2 || static_cast<u32>(row - 2) >= u.body) return;
    const u32 i = u.top + static_cast<u32>(row - 2);
    if (i >= u.rows.size()) return;
    u.sel = i;
    if (b.clicks >= 2) test_run(test_row_tests(i));  // (twice: it runs)
    ++test_gen_;
    dirty_ = true;
}

bool App::test_panel_key(const SDL_KeyboardEvent& k) {
    TestUi& u = *testui_;
    const bool cmd = k.mod & kPrimaryMod;
    if (cmd && k.key == SDLK_R) {  // found again
        test_find();
        return true;
    }
    if (cmd) return false;
    const u32 n = static_cast<u32>(u.rows.size());
    switch (k.key) {
        case SDLK_UP:
            if (n) u.sel = (u.sel + n - 1) % n;
            break;
        case SDLK_DOWN:
            if (n) u.sel = (u.sel + 1) % n;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (u.sel < n && u.rows[u.sel].file >= 0) open_path(u.cov_files[static_cast<size_t>(u.rows[u.sel].file)].c_str());  // (a file's coverage: the file)
            else if (u.sel < n && !test_row_tests(u.sel).empty()) test_run(test_row_tests(u.sel));
            break;
        case SDLK_C: test_run({}, true); break;
        case SDLK_A: test_run({}); break;
        case SDLK_F: test_run_failed(); break;
        case SDLK_D:
            if (u.sel < n && u.rows[u.sel].test >= 0) test_debug(static_cast<size_t>(u.rows[u.sel].test));
            break;
        default: return false;
    }
    ++test_gen_;
    dirty_ = true;
    return true;
}

// For the tests: the tests found, by group, and how each went.
std::string App::test_dump() {
    TestUi& u = *testui_;
    if (!u.looked && !u.looking) test_find();
    for (int i = 0; i < 600 && (u.looking || u.running || u.preparing || u.reading); ++i) {  // (finding them and running them, waited for: 60 s at most)
        if (u.looking) {
            if (u.worker.joinable()) u.worker.join();
            test_sync();
        }
        if (u.preparing || u.reading) {  // (on another machine: what is asked there, waited for)
            remote_sync();
            SDL_Delay(100);
        }
        if (u.running) {
            build_wait();
            test_ended(u.term);
        }
    }
    if (u.tests.empty()) return u.note.empty() ? "none" : u.note;
    std::string here;  // (the marks in the file in front: line from 1, and how its test went)
    for (const auto& [l, i] : test_defs(*pane_->doc)) {
        if (i < 0) continue;
        const TestCase& t = u.tests[static_cast<size_t>(i)];
        here += " " + std::to_string(l + 1) + (t.result == TestCase::kPassed ? "+" : t.result == TestCase::kFailed ? "x" : ".");
    }
    std::string out, group;
    for (const TestCase& t : u.tests) {
        if (t.group != group) out += (out.empty() ? "" : " | ") + (group = t.group) + ":";
        const char* mark = t.result == TestCase::kPassed ? "+" : t.result == TestCase::kFailed ? "x" : t.result == TestCase::kSkipped ? "-" : ".";
        out += std::string(" ") + mark + t.name;
    }
    return here.empty() ? out : out + " | here:" + here;
}

}  // namespace blit
