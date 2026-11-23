#include "engine.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <unordered_map>

#include "job.hpp"
#include "lsp.hpp"
#include "pty.hpp"
#include "remote.hpp"

namespace blit {

namespace {

constexpr size_t kMaxLine = 16u << 20;  // bytes of a line kept (a JSON listing's can be long)
constexpr u64 kGraceNs = 300'000'000;   // between SIGTERM and SIGKILL

}  // namespace

// What a program writes, cut into lines: escape sequences dropped, a line
// drawn over with \r given as kRedrawn (on a terminal) before what is
// written over it, a line that is a JSON object given as a value where asked.
struct Engine::Lines {
    Lines(Engine* engine, Stream s, bool on_term) : e(engine), stream(s), term(on_term) {}
    Engine* e;
    Stream stream;
    bool term;
    std::string line;
    enum : u8 { kText, kEsc, kEscMore, kCsi, kString, kStringEsc } esc = kText;
    bool cr = false;

    void give(std::string_view l, Stream s) {
        if (e->json_ && e->on_json_ && s != kRedrawn && l.starts_with('{')) {
            json::Doc doc;
            if (doc.parse(std::string(l)) && doc.root().is_object()) return e->on_json_(doc.root(), s);
        }
        if (e->on_line_) e->on_line_(l, s);
    }
    void feed(std::string_view bytes) {
        for (const char ch : bytes) {
            const u8 c = static_cast<u8>(ch);
            switch (esc) {
                case kText: break;
                case kEsc:
                    esc = c == '[' ? kCsi : (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') ? kString : (c >= 0x20 && c <= 0x2F) ? kEscMore : kText;
                    continue;
                case kEscMore:
                    if (c >= 0x30) esc = kText;
                    continue;
                case kCsi:
                    if (c >= 0x40 && c <= 0x7E) esc = kText;
                    continue;
                case kString:
                    if (c == 0x07) esc = kText;
                    else if (c == 0x1B) esc = kStringEsc;
                    continue;
                case kStringEsc: esc = c == '\\' ? kText : kString; continue;
            }
            if (c == '\n') {
                give(line, stream);
                line.clear();
                cr = false;
            } else if (c == '\r') {
                cr = true;
            } else if (c == 0x1B) {
                esc = kEsc;
            } else if (c >= 0x20 || c == '\t') {
                if (cr) {
                    if (term && !line.empty()) give(line, kRedrawn);
                    line.clear();
                    cr = false;
                }
                if (line.size() < kMaxLine) line += ch;
            } else if (c == '\b') {
                if (!line.empty()) line.pop_back();
            }
        }
    }
    void finish() {
        if (!line.empty()) give(line, stream);
        line.clear();
    }
};

std::shared_ptr<Engine> Engine::run(EngineSpec spec, LineFn on_line, ExitFn on_exit, JsonFn on_json) {
    std::shared_ptr<Engine> e(new Engine());
    e->on_line_ = std::move(on_line);
    e->on_exit_ = std::move(on_exit);
    e->on_json_ = std::move(on_json);
    e->json_ = spec.json;
    if (spec.timeout_ms) e->deadline_ns_ = now_ns() + static_cast<u64>(spec.timeout_ms) * 1'000'000;
    Engine* raw = e.get();
    e->thread_ = std::jthread([raw, s = std::move(spec)]() mutable { raw->work(std::move(s)); });
    return e;
}

Engine::~Engine() {
    cancel();
    if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) thread_.detach();  // (let go of in its own callback: it ends by itself)
}

void Engine::cancel() {
    cancelled_ = true;
    cv_.notify_all();
}

bool Engine::wait(int ms) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return ended_.load(); });
}

bool Engine::stopping(const EngineSpec&, EngineExit* exit) const {
    if (cancelled_.load()) return exit->cancelled = true;
    if (deadline_ns_ && now_ns() >= deadline_ns_) return exit->timed_out = true;
    return false;
}

void Engine::work(EngineSpec spec) {
    EngineExit exit;
    if (spec.argv.empty() || spec.argv[0].empty()) exit.error = "no program to run";
    else if (!spec.host.empty()) there(spec, &exit);
    else if (spec.pty) here_pty(spec, &exit);
    else here_pipe(spec, &exit);
    if (on_exit_) on_exit_(exit);
    {
        std::lock_guard lock(mu_);
        ended_ = true;
    }
    cv_.notify_all();
}

// Here, on pipes: a job of Jobs' own, whose output comes to this run's
// lines on the job's thread; this thread watches for the end, a cancel and
// the time limit.
void Engine::here_pipe(const EngineSpec& spec, EngineExit* exit) {
    std::mutex m;
    std::condition_variable done_cv;
    bool done = false;
    JobEvent last;
    Lines out{this, kOut, false}, err{this, kErr, false};
    JobSpec js;
    js.argv = spec.argv;
    js.cwd = spec.cwd;
    js.env = spec.env;
    js.input = spec.input;
    js.stream_out = js.stream_err = true;
    js.sink = [&](JobEvent&& ev) {
        if (ev.kind == JobEvent::kOut) return out.feed(ev.out);
        if (ev.kind == JobEvent::kErr) return err.feed(ev.out);
        out.finish();
        err.finish();
        std::lock_guard lock(m);
        last = std::move(ev);
        done = true;
        done_cv.notify_all();
    };
    Jobs jobs;
    const u64 id = jobs.start(std::move(js));
    bool stopped = false;
    std::unique_lock lock(m);
    while (!done) {
        done_cv.wait_for(lock, std::chrono::milliseconds(50));
        if (!done && !stopped && stopping(spec, exit)) {
            jobs.stop(id);  // (SIGTERM; Jobs kills it if it lingers)
            stopped = true;
        }
    }
    exit->code = last.code;
    exit->started = last.code != -1 || last.err.empty();
    if (!exit->started) exit->error = last.err;
}

// Here, on a pseudo-terminal (pty.cpp): read on this thread.
void Engine::here_pty(const EngineSpec& spec, EngineExit* exit) {
    Pty pty;
    if (!pty_spawn(spec.argv, spec.cwd, spec.env, spec.rows, spec.cols, &pty, &exit->error)) return;
    exit->started = true;
    Lines out{this, kOut, true};
    char buf[65536];
    u64 term_at = 0;
    bool killed = false, reaped = false;
    int st = 0;
    auto signal_it = [&] {  // (the program and what it started: it leads a session of its own)
        const u64 now = now_ns();
        if (!term_at && stopping(spec, exit)) {
            ::kill(-pty.pid, SIGTERM);
            term_at = now;
        } else if (term_at && !killed && now - term_at > kGraceNs) {
            ::kill(-pty.pid, SIGKILL);
            killed = true;
        }
    };
    for (;;) {
        pollfd p{pty.fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 50);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            const ssize_t k = ::read(pty.fd, buf, sizeof buf);
            if (k > 0) out.feed(std::string_view(buf, static_cast<size_t>(k)));
            else if (k == 0 || (errno != EAGAIN && errno != EINTR)) break;  // (it has ended: a pseudo-terminal says EIO)
            continue;
        }
        signal_it();
        // (A program that has ended while something it started holds its terminal open: not waited for.)
        if (!reaped && ::waitpid(pty.pid, &st, WNOHANG) == pty.pid) reaped = true;
        if (reaped && r == 0) break;
    }
    out.finish();
    while (!reaped) {
        const pid_t w = ::waitpid(pty.pid, &st, WNOHANG);
        if (w == pty.pid || (w < 0 && errno != EINTR)) break;
        signal_it();
        ::usleep(10'000);
    }
    ::close(pty.fd);
    exit->code = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
}

// There: the agent runs it (kSpawn on pipes, kSpawnPty on a terminal),
// its output read here from the connection's pipes. (The agent's kKill
// signals the program's group: what it started ends with it.)
void Engine::there(const EngineSpec& spec, EngineExit* exit) {
    const std::shared_ptr<Remote> c = Remote::get(spec.host);
    const u64 connect_until = now_ns() + static_cast<u64>(kRemoteWaitMs) * 1'000'000;
    while (!c->wait_ready(100)) {
        if (c->state() == Remote::kFailed) {
            exit->error = c->error().empty() ? "cannot connect to " + spec.host : c->error();
            return;
        }
        if (stopping(spec, exit)) return;
        if (now_ns() > connect_until) {
            exit->error = "connecting to " + spec.host + " took too long";
            return;
        }
    }
    RemoteProc p;
    p.errors = !spec.pty;
    if (!c->spawn(spec.argv, spec.cwd, spec.env, &p, &exit->error, spec.pty ? spec.rows : 0, spec.pty ? spec.cols : 0)) return;
    exit->started = true;
    for (const int fd : {p.out, p.err})
        if (fd >= 0) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (!spec.pty && !spec.input.empty()) {
        size_t sent = 0;
        while (sent < spec.input.size()) {
            const ssize_t w = ::write(p.in, spec.input.data() + sent, spec.input.size() - sent);
            if (w > 0) sent += static_cast<size_t>(w);
            else if (errno != EINTR) break;
        }
    }
    if (!spec.pty) ::close(p.in), p.in = -1;
    Lines out{this, kOut, spec.pty}, err{this, kErr, false};
    u64 term_at = 0;
    bool killed = false;
    auto signal_it = [&] {
        const u64 now = now_ns();
        if (!term_at && stopping(spec, exit)) {
            c->kill(p.id, SIGTERM);
            term_at = now;
        } else if (term_at && !killed && now - term_at > kGraceNs) {
            c->kill(p.id, SIGKILL);
            killed = true;
        }
    };
    char buf[65536];
    bool out_open = p.out >= 0, err_open = p.err >= 0;
    while (out_open || err_open) {
        pollfd fds[2] = {{out_open ? p.out : -1, POLLIN, 0}, {err_open ? p.err : -1, POLLIN, 0}};
        const int r = ::poll(fds, 2, 50);
        if (r < 0 && errno != EINTR) break;
        auto drain = [&](int fd, Lines& into, bool& open) {
            const ssize_t k = ::read(fd, buf, sizeof buf);
            if (k > 0) into.feed(std::string_view(buf, static_cast<size_t>(k)));
            else if (k == 0 || (errno != EAGAIN && errno != EINTR)) open = false;
        };
        if (out_open && (fds[0].revents & (POLLIN | POLLERR | POLLHUP))) drain(p.out, out, out_open);
        if (err_open && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) drain(p.err, err, err_open);
        signal_it();
    }
    out.finish();
    err.finish();
    int code;
    while ((code = c->wait(p.id, 100)) == -2) signal_it();
    for (const int fd : {p.in, p.out, p.err})
        if (fd >= 0) ::close(fd);
    exit->code = code;
    if (code == -1) exit->error = "the connection to " + spec.host + " went";
}

// ---------------------------------------------------------------- which

namespace {
// A program found not to be there is asked about again after this long: one
// installed since its warning is found the next time it is wanted.
constexpr u64 kNotThereNs = 3'000'000'000ull;
struct Kept {
    std::string path;  // "" not there
    u64 at_ns = 0;     // when it was asked
};
std::mutex which_mu;
std::unordered_map<std::string, Kept> which_kept;  // (by host "\n" program)
std::atomic<u64> which_count{0};
}  // namespace

bool Engine::which_known(const std::string& host, const std::string& program, std::string* path) {
    std::lock_guard lock(which_mu);
    const auto it = which_kept.find(host + "\n" + program);
    if (it == which_kept.end() || (it->second.path.empty() && now_ns() - it->second.at_ns > kNotThereNs)) return false;
    if (path) *path = it->second.path;
    return true;
}

std::string Engine::which(const std::string& host, const std::string& program) {
    std::string found;
    if (which_known(host, program, &found)) return found;
    ++which_count;
    if (host.empty()) {
        found = Lsp::find_program(program);
    } else {
        const std::shared_ptr<Remote> c = Remote::get(host);
        if (!c->wait_ready(kRemoteWaitMs)) return {};  // (not kept: the machine could not be asked)
        found = c->which(program);
        if (c->state() != Remote::kReady) return {};
    }
    std::lock_guard lock(which_mu);
    which_kept[host + "\n" + program] = {found, now_ns()};
    return found;
}

void Engine::forget(const std::string& host) {
    std::lock_guard lock(which_mu);
    std::erase_if(which_kept, [&](const auto& kv) { return kv.first.starts_with(host + "\n"); });
}

u64 Engine::which_asked() { return which_count.load(); }

}  // namespace blit

#endif
