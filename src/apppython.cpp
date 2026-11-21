// The project's Python: one interpreter for each project folder, and for
// each subproject in it (a folder under it with a pyproject.toml, a .venv,
// a venv or a uv.lock of its own: a file takes the nearest at or above its
// folder). It is found on a worker by a small shell script run in the
// folder (for one on another machine, run there through blit's agent): the
// one chosen for the project (from the status bar, kept in its session),
// else blit.opt.python.interpreter, else its .venv's or venv's, uv's for a
// uv project (a uv.lock, or uv named in pyproject.toml), an active conda
// environment's, else python3 on the PATH; and that Python is asked its
// version, its own path and whether it has debugpy. What was found is kept,
// by folder, and looked at again when something changes on disk in the
// folder (a venv made or taken away), when blit comes to the front, and
// when asked ("Find the project's Python again"). The Python console, the
// debugger's debugpy, a notebook's kernel and pytest all take it, and the
// status bar names it for a Python file ("Python 3.12 (.venv)"; a click chooses
// another). Nothing on the main thread waits for it: what needs it before
// it is known starts once it comes.
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>

#include "app.hpp"
#include "fsio.hpp"
#include "remote.hpp"

extern char** environ;

namespace blit {

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 44;
    SDL_PushEvent(&ev);
}

// What a Python is asked (python -c this): its version, its own path, and
// whether it has debugpy, a line each.
constexpr const char* kProbe = R"py(import sys
try:
    import importlib.util as u
    d = u.find_spec("debugpy") is not None
except Exception:
    d = False
print(sys.version.split()[0])
print(sys.executable)
print(int(d)))py";

// A folder's Python found: run in any folder, with the folder, the
// project's (where the walk up for the nearest subproject stops), the
// interpreter the settings pin ("" none), what is known of the machine's
// roots already ("root|from|python", a line each: a root found again as it
// was is only said to be "same", and its interpreter is not asked again),
// kProbe, then the folders an interpreter was chosen for, each with it.
// Says root=, from=, python=, console=, sig=, then the interpreter's
// version=, exe= and debugpy=.
constexpr const char* kFind = R"sh(d=$1 top=$2 pin=$3 known=$4 probe=$5
shift 5
case $d/ in "$top"/*) ;; *) top=$d ;; esac
while [ "$d" != "$top" ] && [ ! -e "$d/pyproject.toml" ] && [ ! -e "$d/.venv" ] && [ ! -e "$d/venv" ] && [ ! -e "$d/uv.lock" ]; do d=${d%/*}; done
cd "$d" 2>/dev/null || exit 0
chosen=
while [ $# -ge 2 ]; do [ "$1" = "$d" ] && chosen=$2; shift 2; done
con=
if [ -n "$chosen" ]; then py=$chosen from=chosen
elif [ -n "$pin" ]; then py=$pin from=pinned
elif [ -x .venv/bin/python ]; then py=$d/.venv/bin/python from=.venv
elif [ -x venv/bin/python ]; then py=$d/venv/bin/python from=venv
elif [ -e uv.lock ] || grep -q -e uv_build -e '\[tool\.uv' pyproject.toml 2>/dev/null; then py='uv run python' from=uv
elif [ -n "${CONDA_PREFIX:-}" ] && [ -x "$CONDA_PREFIX/bin/python" ]; then py=$CONDA_PREFIX/bin/python from="conda ${CONDA_DEFAULT_ENV:-${CONDA_PREFIX##*/}}"
else py=python3 from=PATH; fi
case $py in "~/"*) py=$HOME/${py#??} ;; /*) ;; */*) [ -x "$d/$py" ] && py=$d/$py ;; esac
case $py in /*/bin/*) e=${py%/bin/*}; { [ "$from" = .venv ] || [ "$from" = venv ] || [ -e "$e/pyvenv.cfg" ] || [ -d "$e/conda-meta" ]; } && [ -x "$e/bin/ipython" ] && con=$e/bin/ipython ;; esac
if [ "$from" = PATH ]; then for e in .venv venv; do [ -x "$e/bin/ipython" ] && { con=$d/$e/bin/ipython; break; }; done; fi
sig="$d|$from|$py"
case "
$known" in *"
$sig
"*) printf 'same\nroot=%s\n' "$d"; exit 0 ;; esac
if [ -x "$py" ]; then set -- "$py"
elif [ "$from" = uv ]; then set -- "$(uv python find 2>/dev/null)"; [ -n "$1" ] || set -- python3
else set -- $py; fi
info=$("$@" -c "$probe" 2>/dev/null </dev/null)
printf 'root=%s\nfrom=%s\npython=%s\nconsole=%s\nsig=%s\n' "$d" "$from" "$py" "$con" "$sig"
printf '%s\n' "$info" | sed -n -e '1s/^/version=/p' -e '2s/^/exe=/p' -e '3s/^/debugpy=/p'
)sh";

// The Pythons to choose from for a folder: its venvs (any folder in it with
// a pyvenv.cfg), uv's for a uv project, conda's (the active one's base and
// its envs), pyenv's, those on the PATH and those uv has installed. A line
// each: kind, name, path, version, and the file it is in the end (two that
// are one file are listed once; a venv is its own). Run with the folder and
// kProbe.
constexpr const char* kList = R"sh(d=$1 probe=$2
cd "$d" 2>/dev/null || exit 0
say() {
    [ -x "$3" ] || return 0
    v=$("$3" -c "$probe" 2>/dev/null </dev/null | sed -n 1p)
    [ -n "$v" ] || return 0
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$v" "$(realpath "$3" 2>/dev/null || echo "$3")"
}
for e in */ .*/; do
    e=${e%/}
    case $e in .|..|'*'|'.*') continue ;; esac
    [ -f "$e/pyvenv.cfg" ] && say venv "$e" "$d/$e/bin/python"
done
if [ -e uv.lock ] || grep -q -e uv_build -e '\[tool\.uv' pyproject.toml 2>/dev/null; then
    p=$(uv python find 2>/dev/null) && [ -n "$p" ] && v=$("$p" -c "$probe" 2>/dev/null </dev/null | sed -n 1p) && [ -n "$v" ] && printf 'uv\tuv\tuv run python\t%s\t-\n' "$v"
fi
if [ -n "${CONDA_PREFIX:-}" ]; then
    base=${CONDA_PREFIX%/envs/*}
    say conda "conda base" "$base/bin/python"
    for e in "$base"/envs/*/bin/python; do n=${e%/bin/python}; say conda "conda ${n##*/}" "$e"; done
fi
for e in "${PYENV_ROOT:-$HOME/.pyenv}"/versions/*/bin/python; do n=${e%/bin/python}; say pyenv "pyenv ${n##*/}" "$e"; done
IFS=:
for p in $PATH; do
    unset IFS
    for f in "$p"/python3 "$p"/python3.[0-9] "$p"/python3.[0-9][0-9]; do say PATH PATH "$f"; done
done
unset IFS
if command -v uv >/dev/null 2>&1; then
    uv python list --only-installed 2>/dev/null | while read -r name p rest; do say uv uv "$p"; done
fi
)sh";

// A program run here in `dir` (found on the PATH), what it printed
// collected (its errors too, with `errors`); its exit status, or -1 if it
// could not be run or took longer than `ms`.
int run_here(const std::vector<std::string>& argv, const std::string& dir, std::string* out, int ms, bool errors = false) {
    out->clear();
    int pipe_out[2];
    if (::pipe(pipe_out) != 0) return -1;
    ::fcntl(pipe_out[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipe_out[1], 1);
    if (errors) posix_spawn_file_actions_adddup2(&fa, pipe_out[1], 2);
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, pipe_out[1]);
    if (!dir.empty()) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
        posix_spawn_file_actions_addchdir(&fa, dir.c_str());
#else
        posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
#endif
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
    std::vector<std::string> words = argv;
    std::vector<char*> args;
    for (std::string& w : words) args.push_back(w.data());
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pipe_out[1]);
    if (rc != 0) {
        ::close(pipe_out[0]);
        return -1;
    }
    const u64 until = now_ns() + static_cast<u64>(ms) * 1'000'000;
    char buf[16384];
    for (bool late = false; !late;) {
        const u64 now = now_ns();
        if (now >= until) break;
        pollfd p{pipe_out[0], POLLIN, 0};
        const int r = ::poll(&p, 1, static_cast<int>(std::min<u64>((until - now) / 1'000'000 + 1, 1000)));
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        const ssize_t k = ::read(pipe_out[0], buf, sizeof buf);
        if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) break;
        if (k > 0) out->append(buf, static_cast<size_t>(k));
        late = out->size() > (16u << 20);
    }
    ::close(pipe_out[0]);
    int st = 0;
    while (::waitpid(pid, &st, WNOHANG) == 0) {
        if (now_ns() >= until) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, 0);
            return -1;
        }
        ::usleep(2000);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// A script run by sh in a folder (one on another machine, there), with
// these as its $1, $2...; what it printed.
bool run_in(const std::string& dir, const char* script, const std::vector<std::string>& args, std::string* out, int ms) {
    if (is_remote(dir)) {  // (by sh there, not the login shell that runs it, which may be zsh: its own words and globs)
        std::string command = "exec /bin/sh -c " + sh_word(std::string(script)) + " sh";
        for (const std::string& a : args) command += a.empty() ? std::string(" ''") : " " + sh_word(a);
        return fs_run(dir, command, out, ms);
    }
    std::vector<std::string> argv = {"/bin/sh", "-c", script, "sh"};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_here(argv, {}, out, ms) == 0;
}

// The value of a "name=value" line of what a script said ("" if none).
std::string said(std::string_view out, std::string_view name) {
    for (size_t at = 0; at < out.size();) {
        const size_t nl = std::min(out.find('\n', at), out.size());
        const std::string_view line = out.substr(at, nl - at);
        if (line.size() > name.size() && line.starts_with(name) && line[name.size()] == '=') return std::string(line.substr(name.size() + 1));
        at = nl + 1;
    }
    return {};
}

// A path as a folder there names it (one here, as it is); and the machine's prefix ("" here).
std::string there_of(const std::string& path) {
    std::string host, there;
    return split_remote(path, &host, &there) ? there : path;
}
std::string prefix_of(const std::string& path) {
    std::string host, there;
    return split_remote(path, &host, &there) ? remote_prefix(host) : std::string();
}
bool under(const std::string& path, const std::string& folder) { return path == folder || (path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/'); }

// One of the Pythons to choose from.
struct Candidate {
    std::string kind, name, path, version;
};

}  // namespace

struct App::PyUi {
    std::mutex mu;
    std::condition_variable_any cv;  // (work for the workers; and a Python found, for threads waiting for one)
    struct Job {
        enum Kind : u8 { kFind, kList } kind = kFind;
        std::string dir, top;  // the folder, and the project's
        bool full = true;      // (kFind: the interpreter asked again even if it is the one it was)
    };
    std::deque<Job> local, others;  // (the worker for this machine's folders, and the one for the others')
    std::string pin;              // blit.opt.python.interpreter
    std::map<std::string, std::string> chosen;  // the Python chosen for a root, by it
    std::unordered_map<std::string, std::string> roots;  // the root a folder asked about takes
    std::unordered_map<std::string, PyFound> found;      // each root's Python
    std::unordered_map<std::string, std::string> sigs;   // and what the script said of it (kFind's sig)
    std::set<std::string> asked;   // folders being looked at
    std::set<std::string> failed;  // folders that could not be (another machine not reached): python3 for now
    std::vector<std::string> news;  // folders looked at since the main thread last took them
    // The Pythons to choose from, once found: for which root.
    bool listing = false;
    std::optional<std::pair<std::string, std::vector<Candidate>>> listed;
    // The main thread's: what waits for a folder's Python.
    std::vector<std::pair<std::string, std::function<void()>>> waiting;
    std::jthread here_worker, there_worker;  // (last: joined first)

    void queue(Job j) {  // (mu held)
        (is_remote(j.dir) ? others : local).push_back(std::move(j));
        cv.notify_all();
    }
    bool busy() {  // (mu held)
        return !asked.empty() || listing || !news.empty() || listed.has_value();
    }
    void work(std::deque<Job>& q, std::stop_token stop);
    void find(const Job& j);
    void list(const Job& j);
};

void App::PyUi::work(std::deque<Job>& q, std::stop_token stop) {
    for (;;) {
        Job j;
        {
            std::unique_lock lock(mu);
            if (!cv.wait(lock, stop, [&] { return !q.empty(); })) return;
            j = std::move(q.front());
            q.pop_front();
        }
        if (j.kind == Job::kFind) find(j);
        else list(j);
        cv.notify_all();
        wake();
    }
}

void App::PyUi::find(const Job& j) {
    std::vector<std::string> args = {there_of(j.dir), there_of(j.top), {}, {}, kProbe};
    {
        std::lock_guard lock(mu);
        args[2] = pin;
        if (!j.full)  // (what is known of that machine's roots: one of them found again is not asked again)
            for (const auto& [root, sig] : sigs)
                if (prefix_of(root) == prefix_of(j.dir)) args[3] += sig + "\n";
        for (const auto& [root, command] : chosen)
            if (under(root, j.top) && prefix_of(root) == prefix_of(j.dir)) args.push_back(there_of(root)), args.push_back(command);
    }
    std::string out;
    const bool ran = run_in(j.dir, kFind, args, &out, 30000);
    const std::string root = said(out, "root");
    std::lock_guard lock(mu);
    asked.erase(j.dir);
    if (!ran || root.empty()) {
        failed.insert(j.dir);
        news.push_back(j.dir);
        return;
    }
    const std::string prefix = prefix_of(j.dir);
    failed.erase(j.dir);
    if (out.starts_with("same\n")) {  // (a root known already: the folder takes it)
        std::string& r = roots[j.dir];
        if (r != prefix + root && found.contains(prefix + root)) r = prefix + root, news.push_back(j.dir);
        if (r.empty()) roots.erase(j.dir), failed.insert(j.dir), news.push_back(j.dir);  // (not found after all: the script and what is kept disagree)
        return;
    }
    PyFound f;
    f.root = prefix + root;
    f.from = said(out, "from");
    const std::string py = said(out, "python"), console = said(out, "console");
    f.command = py.starts_with('/') ? sh_word(py) : py;
    f.console = console.empty() ? f.command : sh_word(console);
    f.exe = said(out, "exe");
    f.version = said(out, "version");
    if (f.version.empty() || f.version.find_first_not_of("0123456789.+abrc") != std::string::npos) f.version.clear(), f.exe.clear();
    f.debugpy = said(out, "debugpy") == "1";
    sigs[f.root] = said(out, "sig");
    const auto was = roots.find(j.dir);
    if (was != roots.end() && was->second == f.root && found.contains(f.root) && found[f.root] == f) return;  // (as it was)
    roots[j.dir] = f.root;
    found[f.root] = std::move(f);
    news.push_back(j.dir);
}

void App::PyUi::list(const Job& j) {
    std::string out;
    run_in(j.dir, kList, {there_of(j.dir), kProbe}, &out, 60000);
    std::vector<Candidate> all;
    std::set<std::string> paths, files;  // (each once; two that are one file, once, but each venv is its own)
    for (size_t at = 0; at < out.size();) {
        const size_t nl = std::min(out.find('\n', at), out.size());
        const std::string line = out.substr(at, nl - at);
        at = nl + 1;
        std::vector<std::string> f;
        for (size_t a = 0; a <= line.size();) {
            const size_t tab = std::min(line.find('\t', a), line.size());
            f.push_back(line.substr(a, tab - a));
            a = tab + 1;
        }
        if (f.size() != 5 || f[2].empty() || !paths.insert(f[2]).second) continue;
        if (f[0] != "venv" && f[4] != "-" && !files.insert(f[4]).second) continue;
        all.push_back({f[0], f[1], f[2], f[3]});
    }
    std::lock_guard lock(mu);
    listing = false;
    listed.emplace(j.dir, std::move(all));
}

std::shared_ptr<App::PyUi>& App::py_ui() {
    if (!pyui_) {
        pyui_ = std::make_shared<PyUi>();
        PyUi* u = pyui_.get();  // (alive until its workers have been joined)
        u->pin = cfg_.python_interpreter;
        u->here_worker = std::jthread([u](std::stop_token stop) { u->work(u->local, stop); });
        u->there_worker = std::jthread([u](std::stop_token stop) { u->work(u->others, stop); });
    }
    return pyui_;
}

// The folder a subproject is looked for up to: the project's, for a folder
// in it; else the folder itself.
std::string App::py_top(const std::string& dir) const {
    const std::string root = build_root();
    return !root.empty() && under(dir, root) ? root : dir;
}

std::optional<PyFound> App::py_known(const std::string& dir) {
    PyUi& u = *py_ui();
    std::lock_guard lock(u.mu);
    if (const auto r = u.roots.find(dir); r != u.roots.end())
        if (const auto f = u.found.find(r->second); f != u.found.end()) return f->second;
    if (u.failed.contains(dir)) {  // (another machine not reached: what would be run there, were it)
        PyFound f;
        f.root = dir;
        f.from.clear();  // (nothing chose it: the status bar names none)
        f.command = f.console = "python3";
        return f;
    }
    if (u.asked.insert(dir).second) u.queue({PyUi::Job::kFind, dir, py_top(dir), false});
    return std::nullopt;
}

void App::py_when(const std::string& dir, std::function<void()> then) {
    if (py_known(dir)) return then();
    pyui_->waiting.emplace_back(dir, std::move(then));
}

std::string App::py_wait(const std::shared_ptr<PyUi>& py, const std::string& dir, const std::string& top) {
    PyUi& u = *py;
    std::unique_lock lock(u.mu);
    auto known = [&]() -> const PyFound* {
        const auto r = u.roots.find(dir);
        const auto f = r == u.roots.end() ? u.found.end() : u.found.find(r->second);
        return f == u.found.end() ? nullptr : &f->second;
    };
    if (!known() && !u.failed.contains(dir) && u.asked.insert(dir).second) u.queue({PyUi::Job::kFind, dir, top, false});
    u.cv.wait_for(lock, std::chrono::seconds(90), [&] { return known() || u.failed.contains(dir) || !u.asked.contains(dir); });
    const PyFound* f = known();
    return f ? f->command : "python3";
}

// What the workers have found: the status bar shows it, and what waited
// for it goes on.
void App::py_sync() {
    if (!pyui_) return;
    PyUi& u = *pyui_;
    std::vector<std::string> news;
    std::optional<std::pair<std::string, std::vector<Candidate>>> listed;
    {
        std::lock_guard lock(u.mu);
        news.swap(u.news);
        listed.swap(u.listed);
    }
    if (!news.empty()) {
        ++py_gen_;
        dirty_ = true;
    }
    std::vector<std::function<void()>> go;
    for (size_t i = 0; i < u.waiting.size();) {
        if (py_known(u.waiting[i].first)) {
            go.push_back(std::move(u.waiting[i].second));
            u.waiting.erase(u.waiting.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (std::function<void()>& then : go) then();
    if (!listed) return;
    // The list to choose from: as it is now first, then each one found.
    const std::string root = listed->first;
    std::vector<Candidate> all = std::move(listed->second);
    std::string now;
    {
        std::lock_guard lock(u.mu);
        if (const auto c = u.chosen.find(root); c != u.chosen.end()) now = c->second;
    }
    const char* home = SDL_getenv("HOME");
    std::vector<std::string> items{"Automatic"};
    size_t at = 0;
    for (const Candidate& c : all) {
        std::string path = c.path;
        if (home && *home && !is_remote(root) && path.starts_with(std::string(home) + "/")) path = "~" + path.substr(std::strlen(home));
        if (c.path == now) at = items.size();
        items.push_back("Python " + c.version + " \xC2\xB7 " + c.name + (c.path == c.name || c.path == "uv run python" ? std::string() : " \xC2\xB7 " + path));
    }
    find_preview_item("Python for " + root.substr(root.find_last_of('/') + 1), std::move(items), at, nullptr, [this, root, all](int i) {
        if (i < 0) return;
        PyUi& v = *pyui_;
        {
            std::lock_guard lock(v.mu);
            if (i == 0) v.chosen.erase(root);
            else v.chosen[root] = all[static_cast<size_t>(i - 1)].path;
            // (Every folder that takes it looked at again.)
            for (const auto& [dir, r] : v.roots)
                if (r == root && v.asked.insert(dir).second) v.queue({PyUi::Job::kFind, dir, py_top(dir), true});
        }
        dirty_ = true;
    }, nullptr);
}

// Something changed on disk in a folder: the folders whose Python it may
// change are looked at again (a venv made or taken away, a subproject
// made); what is in a venv, with its interpreter asked again.
void App::py_heard(const std::string& dir) {
    if (!pyui_) return;
    PyUi& u = *pyui_;
    std::lock_guard lock(u.mu);
    for (const auto& [k, root] : u.roots) {
        const bool venv = under(dir, root + "/.venv") || under(dir, root + "/venv");
        if (!venv && dir != root && !(under(k, dir) && under(dir, root))) continue;
        if (u.asked.insert(k).second) u.queue({PyUi::Job::kFind, k, py_top(k), venv});
    }
}

void App::py_again(bool full) {
    if (!pyui_) return;
    PyUi& u = *pyui_;
    std::lock_guard lock(u.mu);
    std::set<std::string> dirs(u.failed.begin(), u.failed.end());
    for (const auto& [k, root] : u.roots) dirs.insert(k);
    u.failed.clear();
    for (const std::string& k : dirs)
        if (u.asked.insert(k).second) u.queue({PyUi::Job::kFind, k, py_top(k), full});
}

void App::py_pin() {
    if (!pyui_) return;
    {
        std::lock_guard lock(pyui_->mu);
        pyui_->pin = cfg_.python_interpreter;
    }
    py_again(true);
}

// The folder whose Python the status bar names: the Python file's in front's.
static std::string python_file_dir(const Doc& d, bool py) {
    const std::string& p = d.ed.path;
    if (!py || p.empty() || d.term || d.nb) return {};
    return parent_of(is_remote(p) ? p : full_path(p));
}

std::string App::py_status(const Doc& d) {
    const std::string dir = python_file_dir(d, d.lang == "python" || (d.lang.empty() && (d.ed.path.ends_with(".py") || d.ed.path.ends_with(".pyi"))));
    if (dir.empty()) return {};
    const std::optional<PyFound> f = py_known(dir);
    if (!f || f->from.empty() || (f->from == "PATH" && f->version.empty())) return {};  // (not known, or nothing to say of it)
    std::string v = f->version;
    if (const size_t dot = v.find('.'); dot != std::string::npos) v = v.substr(0, v.find('.', dot + 1));  // (3.12, of 3.12.4)
    return "Python" + (v.empty() ? std::string() : " " + v) + (f->from == "PATH" ? std::string() : " (" + f->from + ")");
}

// The status bar's list (and the command's): the Pythons there are for the
// project of the file in front, to choose from; the one chosen is kept for
// it (in its session).
void App::py_choose() {
    const Doc& d = *pane_->doc;
    std::string dir = python_file_dir(d, d.ed.path.ends_with(".py") || d.ed.path.ends_with(".pyi") || d.lang == "python");
    if (dir.empty()) dir = build_root();
    if (dir.empty()) return;
    py_when(dir, [this, dir] {
        const std::optional<PyFound> f = py_known(dir);
        PyUi& u = *pyui_;
        std::lock_guard lock(u.mu);
        if (u.listing) return;
        u.listing = true;
        u.queue({PyUi::Job::kList, f ? f->root : dir, {}, true});
    });
    notify("Looking for Pythons\xE2\x80\xA6");
}

// The Pythons chosen for the folder's projects, in its session.
std::string App::py_session_text(const std::string& folder) {
    if (!pyui_) return {};
    std::lock_guard lock(pyui_->mu);
    std::string out;
    for (const auto& [root, command] : pyui_->chosen)
        if (under(root, folder) && (root + command).find_first_of("\t\n") == std::string::npos) out += "python\t" + root + "\t" + command + "\n";
    return out;
}

void App::py_session_take(const std::vector<std::pair<std::string, std::string>>& chosen) {
    if (chosen.empty()) return;
    PyUi& u = *py_ui();
    {
        std::lock_guard lock(u.mu);
        for (const auto& [root, command] : chosen) u.chosen[root] = command;
    }
    py_again(true);
}

// ---------------------------------------------------------------- debugpy

// The program's folder's Python, for debugpy: false while it is being found
// (then `then` runs). py_adapter: the command that starts debugpy's adapter.
bool App::py_debug_ready(const std::string& dir, std::function<void()> then) {
    if (py_known(dir)) return true;
    py_when(dir, std::move(then));
    return false;
}

std::string App::py_adapter(const std::string& dir, std::string* python) {
    const std::optional<PyFound> f = py_known(dir);
    if (!f) {
        // (Not known yet: chosen by the shell, in the folder, as it would be.)
        *python = "py=$(cd " + sh_word(there_of(dir)) +
                  " 2>/dev/null && if [ -x .venv/bin/python ]; then echo \"$PWD/.venv/bin/python\"; "
                  "elif [ -x venv/bin/python ]; then echo \"$PWD/venv/bin/python\"; "
                  "elif [ -e uv.lock ] || grep -q -e uv_build -e '\\[tool\\.uv' pyproject.toml 2>/dev/null; then echo uv; fi); "
                  "if [ \"$py\" = uv ]; then exec uv run python -m debugpy.adapter; fi; "
                  "exec \"${py:-python3}\" -m debugpy.adapter";
        return *python;
    }
    *python = f->command;
    return "exec " + f->command + " -m debugpy.adapter";
}

// ---------------------------------------------------------------- for the tests

// Each folder asked about: the root it takes and its Python (from, the
// words that run it, the console's where they differ, its version, and
// "debugpy" where it has its own).
std::string App::py_dump() {
    if (!pyui_) return "none";
    PyUi& u = *pyui_;
    std::lock_guard lock(u.mu);
    std::vector<std::string> lines;
    for (const auto& [dir, root] : u.roots) {
        const auto f = u.found.find(root);
        if (f == u.found.end()) continue;
        const PyFound& p = f->second;
        lines.push_back(dir + " = " + root + " (" + p.from + ") " + p.command + (p.console != p.command ? " console " + p.console : "") + " " +
                        (p.version.empty() ? "?" : p.version) + (p.debugpy ? " debugpy" : ""));
    }
    for (const std::string& dir : u.failed) lines.push_back(dir + " = not reached");
    std::ranges::sort(lines);
    std::string out;
    for (const std::string& l : lines) out += (out.empty() ? "" : " | ") + l;
    return out.empty() ? "none" : out;
}

// Until the workers have nothing left to do, and what they found is taken in (a minute at most).
void App::py_settle() {
    if (!pyui_) return;
    const u64 until = now_ns() + 60'000'000'000ull;
    for (;;) {
        py_sync();
        bool busy;
        {
            std::lock_guard lock(pyui_->mu);
            busy = pyui_->busy();
        }
        if (!busy || now_ns() > until) break;
        SDL_Delay(2);
    }
    py_sync();
}

}  // namespace blit
