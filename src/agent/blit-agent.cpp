// blit's agent: the part of blit that runs on another machine, so that blit
// can work on that machine's files as though they were its own. It reads
// and writes files, lists folders, walks a folder and searches its files
// with the finder's own code (finder.cpp), and runs programs (git, language
// servers, shells) with their input and output carried to and fro
// (protocol.hpp says how).
//
// It outlives the connection. What ssh runs is `blit-agent --attach`, which
// only carries bytes: between ssh and the agent itself, which it finds by
// its socket (in a folder of this user's that only they can enter), and
// starts if it is not running (`--serve`, in a session of its own, and on
// Linux in a systemd scope where there is one, so that neither the
// connection's end nor the login's ends it). blit names itself in its
// hello, so that when it connects again the programs it left running (a
// terminal, a task, a profiling run, the kernel: those started kKeep) are
// its own again, with what they wrote meanwhile. What else it started ends
// with the connection, as before. The agent ends once nothing holds it:
// nobody attached and nothing working for the idle time blit gives it, or
// nobody attached for the longer time it keeps what is working, or when
// blit quits with nothing working, or is told to stop. Where no socket can
// be made, it is the agent of one connection, on ssh's input and output, as
// it always was (and is, run with no argument).
//
// Built here, with the compiler blit is built with: for this machine by
// CMake (the tests' stand-in for a server, and Macs), and for Linux by
// build.sh (GCC 16 in a container, linked statically, so that it runs on
// any Linux). blit copies it to a machine the first time it goes there.
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <span>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "finder.hpp"
#include "protocol.hpp"
#include "pty.hpp"

extern char** environ;

namespace {

using namespace blit_wire;
using Clock = std::chrono::steady_clock;

const std::string kAgentVersion = std::format("blit-agent {}", kAgentLevel);

// ---------------------------------------------------------------- sending

// The main loop is woken (a byte into this pipe) when a thread has given a
// connection something to send that could not all go at once.
int wake_r = -1, wake_w = -1;

void wake_loop() {
    if (wake_w < 0) return;
    const char b = 1;
    [[maybe_unused]] const ssize_t n = ::write(wake_w, &b, 1);  // (full: it is awake already)
}

struct Session;

// A blit at the other end: through blit-agent --attach, on the socket; or,
// for the agent of one connection, on this process's input and output.
struct Client {
    int in = -1, out = -1;
    std::string got;  // (main loop) what has come and is not yet a whole frame
    std::shared_ptr<Session> session;  // (main loop) once it has said hello (or asked for anything)
    std::vector<std::string> env;      // (main loop) from blit-agent --attach: the environment of the connection there
    // What is to go to it, from any thread: a frame goes out whole. What
    // cannot go now waits here, for the main loop to send as it can.
    std::mutex mu;
    std::string pending;
    size_t at = 0;
    bool gone = false;

    size_t waiting() {
        std::lock_guard lock(mu);
        return pending.size() - at;
    }
    // What can go without waiting (mu is held). False once it has gone.
    bool flush() {
        while (!gone && at < pending.size()) {
            const ssize_t n = ::write(out, pending.data() + at, pending.size() - at);
            if (n > 0) at += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && errno == EAGAIN) break;
            else gone = true;
        }
        if (at == pending.size() || gone) pending.clear(), at = 0;
        else if (at > (1u << 20) && at * 2 > pending.size()) pending.erase(0, at), at = 0;
        return !gone;
    }
};

void send_frame(Client& c, uint8_t op, uint32_t id, const std::string& payload) {
    std::lock_guard lock(c.mu);
    if (c.gone) return;
    const bool idle = c.at == c.pending.size();
    c.pending += frame(op, id, payload);
    c.flush();
    if (idle && c.at < c.pending.size()) wake_loop();  // (the rest goes when it can)
}

void reply(Client& c, uint32_t id, const Writer& w) { send_frame(c, kReply, id, w.b); }

// Requests answered on threads of their own (reading a big file, walking a
// folder, searching it), so that a program's output is never held up.
std::atomic<int> working_threads{0};
template <class F>
void on_its_own(F&& f) {
    ++working_threads;
    std::jthread([f = std::forward<F>(f)] {
        f();
        --working_threads;
    }).detach();
}

// ---------------------------------------------------------------- files

// ---------------------------------------------------------------- files

int64_t mtime_ns(const struct stat& st) {
#if defined(__APPLE__)
    return static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
}

// A file whole into `out` (no more than `most` bytes). The status as kRead
// has it; with text_only, a file with a zero byte in its first 8 KB is not
// read further (status 2, as for something that is not a file).
uint8_t read_file(const std::string& path, uint64_t most, std::string* out, int64_t* modified, bool text_only = false) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : 4;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return 4;
    }
    if (!S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) > most) {
        const bool file = S_ISREG(st.st_mode);
        ::close(fd);
        return file ? 3 : 2;
    }
    *modified = mtime_ns(st);
    size_t got = 0;
    bool binary = false;
    out->resize_and_overwrite(static_cast<size_t>(st.st_size), [&](char* p, size_t n) {
        constexpr size_t kFirst = 8192;
        while (got < n) {
            const ssize_t k = ::read(fd, p + got, (text_only && got < kFirst ? std::min(n, kFirst) : n) - got);
            if (k > 0) got += static_cast<size_t>(k);
            else if (k < 0 && errno == EINTR) continue;
            else break;
            if (text_only && got <= kFirst && (got == n || got == kFirst) && std::memchr(p, 0, got)) {
                binary = true;
                break;
            }
        }
        return binary ? 0 : got;
    });
    ::close(fd);
    return binary ? 2 : 0;
}

// A file being written: through a temporary file beside it, so that it is
// never seen half written; through a link to the file it names; keeping its
// mode. Its text comes whole (kWrite) or a part at a time (kWritePart).
struct Writing {
    std::string path, tmp, err;  // (err: why it failed, once it has: what comes after is dropped)
    int fd = -1;

    bool begin(std::string p) {
        char real[PATH_MAX];
        path = ::realpath(p.c_str(), real) ? std::string(real) : std::move(p);
        tmp = path + ".blit-tmp";
        struct stat st{};
        const bool had = ::stat(path.c_str(), &st) == 0;
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, had ? 0600 : 0644);
        if (fd < 0) {
            err = std::format("cannot write {}: {}", tmp, std::strerror(errno));
            return false;
        }
        if (had) ::fchmod(fd, st.st_mode & 07777);  // (the mode it is to have, before any of its text is in it)
        return true;
    }
    void add(std::string_view data) {
        for (size_t done = 0; fd >= 0 && done < data.size();) {
            const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
            if (n > 0) done += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else fail();
        }
    }
    void fail() {
        err = std::format("cannot save {}: {}", path, std::strerror(errno));
        ::close(fd);
        fd = -1;
        ::unlink(tmp.c_str());
    }
    bool finish(std::string* why, int64_t* modified) {
        if (fd >= 0 && ::close(fd) != 0) {
            fd = -1;
            fail();
        }
        fd = -1;
        if (err.empty() && ::rename(tmp.c_str(), path.c_str()) != 0) {
            err = std::format("cannot save {}: {}", path, std::strerror(errno));
            ::unlink(tmp.c_str());
        }
        if (!err.empty()) return *why = err, false;
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) *modified = mtime_ns(st);
        return true;
    }
};

bool write_file(std::string path, std::string_view data, std::string* err, int64_t* modified) {
    Writing w;
    if (w.begin(std::move(path))) w.add(data);
    return w.finish(err, modified);
}

bool make_dirs(const std::string& path, bool parents, std::string* err) {
    if (parents)
        for (size_t at = 1; (at = path.find('/', at)) != std::string::npos; ++at) ::mkdir(path.substr(0, at).c_str(), 0755);
    if (::mkdir(path.c_str(), 0755) == 0 || (parents && errno == EEXIST)) return true;
    *err = std::format("cannot make {}: {}", path, std::strerror(errno));
    return false;
}

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

// Into the freedesktop trash (~/.local/share/Trash), as file managers do,
// with a note of where it came from.
bool trash(const std::string& path, std::string* err) {
    const char* data = std::getenv("XDG_DATA_HOME");
    const std::string base = (data && *data ? std::string(data) : home() + "/.local/share") + "/Trash";
    const std::string files = base + "/files", info = base + "/info";
    make_dirs(files, true, err);
    make_dirs(info, true, err);
    const std::string name = path.substr(path.find_last_of('/') + 1);
    std::string target = name;
    for (int k = 2; ::access((files + "/" + target).c_str(), F_OK) == 0; ++k) target = std::format("{} {}", name, k);
    if (::rename(path.c_str(), (files + "/" + target).c_str()) != 0) {
        *err = std::format("cannot move {} to the trash: {}", path, std::strerror(errno));
        return false;
    }
    if (FILE* f = std::fopen((info + "/" + target + ".trashinfo").c_str(), "wb")) {
        char when[32];
        const time_t now = ::time(nullptr);
        struct tm t;
        ::localtime_r(&now, &t);
        std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &t);
        std::fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", path.c_str(), when);
        std::fclose(f);
    }
    return true;
}

std::string which(const std::string& program, const char* path) {
    if (program.contains('/')) return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
    const std::string_view dirs = path ? path : "/usr/bin:/bin";
    for (size_t at = 0; at <= dirs.size();) {
        const size_t colon = dirs.find(':', at);
        const std::string_view d = dirs.substr(at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
        const std::string full = std::format("{}/{}", d.empty() ? "." : d, program);
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(full.c_str(), X_OK) == 0) return full;
        if (colon == std::string_view::npos) break;
        at = colon + 1;
    }
    return {};
}

// The latest few folders walked, by number, for searches in them.
std::mutex walks_mu;
std::map<uint32_t, std::shared_ptr<const blit::FileIndex>> walks;
uint32_t next_walk = 1;


// ---------------------------------------------------------------- programs

// What a program has been sent and has not yet taken is held here up to
// kInMost: past that, blit is told to hold what it sends until the
// program has taken all but kInLess (kInFlow), so that what is held stays
// a few MB (what was on its way still comes).
constexpr size_t kInMost = 1u << 20, kInLess = 256u << 10;
// What a program kept running writes is kept, the latest kKeptMost of it,
// for blit to have when it comes back (kResume).
constexpr size_t kKeptMost = 4u << 20;
// No program's output is read for a blit that has more than this waiting to
// go to it already (the connection is slower than the program): the
// program waits, as one writing into a full pipe does.
constexpr size_t kOutMost = 1u << 20;

struct Child {
    pid_t pid = -1;
    int in = -1, out = -1, err = -1;  // (on a pseudo-terminal, in and out are one descriptor)
    bool pty = false;
    std::string pending;    // input not written yet (from pending_at)
    size_t pending_at = 0;
    bool in_paused = false; // blit has been told to hold its input (kInFlow)
    bool close_in = false;  // and then its input closes
    bool paused = false;    // blit has more of its output than it has taken yet: none is read until it asks again (kFlow)
    bool exited = false;
    int status = 0;
    // Kept running when blit goes (kKeep), and what it wrote: the latest
    // of it, as it came (stream, bytes), and how much of each stream there
    // has been in all.
    bool keep = false, work = false;
    std::deque<std::pair<uint8_t, std::string>> kept;
    size_t kept_bytes = 0;
    uint64_t total[3] = {0, 0, 0};
    bool behind = false;     // blit has not had what was kept since it went (until kResume)
    bool was_working = false;  // (as blit was last told, kWorking)
    // Given work (kBusy) until it writes `done`: how many pieces, and the
    // end of what it last wrote (done may come in two reads).
    std::string done, tail;
    int busy = 0;
    // Told to end (blit went, or quit): when it is killed if it has not
    // ended, and when it is let go of if something it started still holds
    // its pipes.
    bool ending = false, killed = false;
    Clock::time_point end_at{};

    void keep_output(uint8_t stream, std::string_view b) {
        total[stream] += b.size();
        if (b.size() > kKeptMost) b.remove_prefix(b.size() - kKeptMost);
        kept.emplace_back(stream, std::string(b));
        kept_bytes += b.size();
        while (kept_bytes > kKeptMost && kept.size() > 1) {
            kept_bytes -= kept.front().second.size();
            kept.pop_front();
        }
    }
    void watch_done(std::string_view b) {
        if (done.empty() || busy <= 0) return;
        std::string seen = tail;
        seen.append(b);
        for (size_t at = 0; busy > 0 && (at = seen.find(done, at)) != std::string::npos; at += done.size()) --busy;
        tail = seen.size() >= done.size() ? seen.substr(seen.size() - done.size() + 1) : seen;
    }
    // Working, as quitting asks: a task's while it runs; a shell's while a
    // program is in front of it; one given work until its done.
    bool working() const {
        if (!keep || exited || ending) return false;
        if (work || busy > 0) return true;
        if (!pty || out < 0) return false;
        const pid_t front = ::tcgetpgrp(out);
        return front > 0 && front != pid;
    }
};

// A blit, by the name it gives itself: what it started, and what it is
// writing a part at a time. One with no name (an older blit, or the agent of
// one connection) does not outlast its connection.
struct Session {
    std::string name;
    std::shared_ptr<Client> client;
    uint32_t level = 0;  // (what blit says of itself in kHello: 3 understands kInFlow, 4 kWorking)
    std::map<uint32_t, Child> children;
    std::map<uint32_t, Writing> writings;
    std::vector<std::string> env;  // the connection's environment (PATH, SSH_AUTH_SOCK), for what is started on it

    // A variable of that environment (or this process's).
    const char* var(std::string_view called) const {
        for (const std::string& e : env)
            if (e.size() > called.size() && e.starts_with(called) && e[called.size()] == '=') return e.c_str() + called.size() + 1;
        return env.empty() ? std::getenv(std::string(called).c_str()) : nullptr;
    }
};
std::vector<std::shared_ptr<Session>> sessions;

// How long the agent waits for a blit (blit says, in kHello).
std::chrono::seconds idle_for{600}, keep_for{86400};

void in_flow(Session& s, uint32_t id, Child& c, bool paused) {
    if (c.in_paused == paused || (paused && s.level < 3)) return;
    c.in_paused = paused;
    if (!s.client) return;
    Writer w;
    w.u32(id).u8(paused);
    send_frame(*s.client, kInFlow, 0, w.b);
}

// (quiet: what it writes as errors goes nowhere, rather than to blit, which would not read it.)
bool spawn(Session& s, uint32_t id, const std::string& cwd, std::vector<std::string> argv, std::span<const std::string> env, bool quiet, std::string* err) {
    int in[2], out[2], er[2] = {-1, -1};
    if (::pipe(in) != 0) return *err = "cannot make a pipe", false;
    if (::pipe(out) != 0) {
        ::close(in[0]), ::close(in[1]);
        return *err = "cannot make a pipe", false;
    }
    if (!quiet && ::pipe(er) != 0) {
        ::close(in[0]), ::close(in[1]), ::close(out[0]), ::close(out[1]);
        return *err = "cannot make a pipe", false;
    }
    for (const int fd : {in[1], out[0], er[0]}) {
        if (fd < 0) continue;
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    // Into its folder by way of a shell (posix_spawn has no portable way),
    // which then becomes the program itself.
    std::vector<std::string> words;
    if (!cwd.empty()) {
        words = {"/bin/sh", "-c", "cd \"$1\" && shift && exec \"$@\"", "sh", cwd};
        words.insert(words.end(), argv.begin(), argv.end());
    } else {
        words = std::move(argv);
    }
    std::vector<char*> args;
    for (std::string& w : words) args.push_back(w.data());
    args.push_back(nullptr);
    // The environment: the connection's (the PATH a login there has now,
    // its SSH_AUTH_SOCK), or this process's for one that did not say, and
    // what blit gives over it.
    std::vector<std::string> envs;
    auto given = [&](std::string_view var) {
        const std::string_view name = var.substr(0, var.find('=') + 1);
        return std::ranges::any_of(env, [&](const std::string& e) { return e.starts_with(name); });
    };
    if (!s.env.empty()) {
        for (const std::string& e : s.env)
            if (!given(e)) envs.push_back(e);
    } else {
        for (char** e = environ; e && *e; ++e)
            if (!given(*e)) envs.emplace_back(*e);
    }
    envs.insert(envs.end(), env.begin(), env.end());
    std::vector<char*> envp;
    for (std::string& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    if (quiet) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    else posix_spawn_file_actions_adddup2(&fa, er[1], 2);
    for (const int fd : {in[0], out[1], er[1]})
        if (fd >= 0) posix_spawn_file_actions_addclose(&fa, fd);
    // (A process group of its own: what it starts in turn, a language
    // server's worker, is ended with it when blit goes.)
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, args[0], &fa, &attr, args.data(), envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in[0]), ::close(out[1]);
    if (er[1] >= 0) ::close(er[1]);
    if (rc != 0) {
        ::close(in[1]), ::close(out[0]);
        if (er[0] >= 0) ::close(er[0]);
        *err = std::format("cannot run {}: {}", words.empty() ? "" : words[0], std::strerror(rc));
        return false;
    }
    Child& c = s.children[id];
    c.pid = pid, c.in = in[1], c.out = out[0], c.err = er[0];
    return true;
}

// Once blit has gone, what it started is told to end; whatever has not by
// kKillAfter is killed, and whatever still holds a pipe open by kLeaveAfter
// (a program that has left its group) is left.
constexpr auto kKillAfter = std::chrono::seconds(2), kLeaveAfter = std::chrono::seconds(3);

// A program's process group (its own: spawn), and the program, if it is
// still there: a terminal's hung up on, another's sent `sig`. (The group
// even when the program has exited: what it started may still be running.
// Its pid is not another program's meanwhile: finish_children.)
void signal_child(Child& c, int sig) {
    if (c.pid <= 0) return;
    const int s = c.pty && sig == SIGTERM ? SIGHUP : sig;  // (a shell does not end on SIGTERM; a terminal that hangs up ends it)
    // (And the program in front of a terminal's shell, which a shell with
    // job control puts in a group of its own: hung up on, as a terminal does.)
    if (c.pty && c.out >= 0)
        if (const pid_t front = ::tcgetpgrp(c.out); front > 0 && front != c.pid) ::kill(-front, s);
    ::kill(-c.pid, s);
    if (!c.exited) ::kill(c.pid, s);
}

void end_child(Child& c) {
    if (c.ending) return;
    c.ending = true;
    c.paused = false;  // (what it still writes is read, so that it can end)
    c.end_at = Clock::now();
    signal_child(c, SIGTERM);
}

// The ending programs that have not ended in time: killed, and then let go of.
void hurry_children(Session& s) {
    const Clock::time_point now = Clock::now();
    for (auto it = s.children.begin(); it != s.children.end();) {
        Child& c = it->second;
        if (c.ending && !c.killed && now - c.end_at >= kKillAfter) {
            c.killed = true;
            signal_child(c, SIGKILL);
        }
        if (c.ending && now - c.end_at >= kLeaveAfter) {
            for (int* fd : {&c.out, &c.err})
                if (*fd >= 0) ::close(*fd), *fd = -1;
            if (c.in >= 0 && c.in != c.out) ::close(c.in);
            c.in = -1;
            if (c.pid > 0 && !c.exited) {
                ++it;  // (killed: it is reaped when it goes)
                continue;
            }
            int st = 0;
            if (c.pid > 0) ::waitpid(c.pid, &st, WNOHANG);
            it = s.children.erase(it);
            continue;
        }
        ++it;
    }
}

// A program's end is told once all it wrote has gone, and it has exited.
// (It is waited for only then: until that its pid, and so its process
// group's, is not given to another program, which signal_child would
// otherwise reach.) One kept running while blit is away keeps its end for
// when blit comes back.
void finish_children(Session& s) {
    for (auto it = s.children.begin(); it != s.children.end();) {
        Child& c = it->second;
        if (!c.exited && c.pid > 0) {
            siginfo_t si{};
            if (::waitid(P_PID, static_cast<id_t>(c.pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == c.pid)
                c.exited = true, c.status = si.si_code == CLD_EXITED ? si.si_status : 128 + si.si_status;
        }
        const bool told_later = c.keep && !c.ending && (!s.client || c.behind);
        if (c.exited && c.out < 0 && c.err < 0 && !told_later) {
            int st = 0;
            if (c.pid > 0) ::waitpid(c.pid, &st, WNOHANG);  // (now let go of)
            if (c.in >= 0) ::close(c.in);
            if (s.client && !c.ending) {
                Writer w;
                w.u32(it->first).u32(static_cast<uint32_t>(c.status));
                send_frame(*s.client, kExit, 0, w.b);
            }
            it = s.children.erase(it);
        } else {
            ++it;
        }
    }
}

// What a program kept running wrote that blit has not had (from `had` of
// each stream), sent; where some of that was let go of, from the first
// line after the gap (a terminal's screen, the kernel's messages: a line
// cut in two would be wrong).
void send_kept(Client& cl, uint32_t id, Child& c, const uint64_t had[3]) {
    uint64_t at[3];
    for (int s = 1; s <= 2; ++s) at[s] = c.total[s];
    for (const auto& [s, b] : c.kept) at[s] -= b.size();  // (where what is kept starts, of each stream)
    bool cut[3] = {false, had[1] < at[1], had[2] < at[2]};
    for (const auto& [s, b] : c.kept) {
        const uint64_t from = at[s];
        at[s] += b.size();
        if (at[s] <= had[s]) continue;
        std::string_view part(b);
        if (had[s] > from) part.remove_prefix(static_cast<size_t>(had[s] - from));
        if (cut[s]) {
            const size_t nl = part.find('\n');
            if (nl == std::string_view::npos) continue;
            part.remove_prefix(nl + 1);
            cut[s] = false;
        }
        if (part.empty()) continue;
        Writer w;
        w.u32(id).u8(s).str(part.data(), part.size());
        send_frame(cl, kOutput, 0, w.b);
    }
}

// blit has gone from a session: what it started ends, but what it asked to
// be kept running (if it is a session that outlasts its connection).
void detach(Session& s) {
    s.client.reset();
    for (auto& [id, c] : s.children) {
        c.paused = false;  // (read now as it comes: kept, or so that it can end)
        if (c.keep && !s.name.empty()) c.behind = true;
        else end_child(c);
    }
    for (auto& [n, w] : s.writings)
        if (w.fd >= 0) w.fail();  // (half written: let go of)
    s.writings.clear();
}

void end_session(Session& s) {
    for (auto& [id, c] : s.children) end_child(c);
}

// ---------------------------------------------------------------- requests

// The daemon is to end: everything it runs, and then it (the main loop).
bool ending_all = false;
Clock::time_point ending_since{};
bool quick = false;  // a blit quit and left nothing working: the agent ends as soon as nothing holds it

void stop_listening();

void end_all() {
    if (ending_all) return;
    ending_all = true;
    stop_listening();
    ending_since = Clock::now();
    for (const std::shared_ptr<Session>& s : sessions) end_session(*s);
}

std::shared_ptr<Session> session_named(const std::string& name) {
    if (!name.empty())
        for (const std::shared_ptr<Session>& s : sessions)
            if (s->name == name) return s;
    auto s = std::make_shared<Session>();
    s->name = name;
    sessions.push_back(s);
    return s;
}

void close_client(Client& c) {
    std::lock_guard lock(c.mu);
    if (c.gone && c.in < 0) return;
    c.gone = true;
    c.pending.clear(), c.at = 0;
}

bool listening = false;  // (a daemon: blits come and go; else the agent of one connection)
// Its socket, and the lock that says it is the one serving it: let go of
// as soon as it is ending, so that a blit coming meanwhile starts another.
std::string sock_path;
int listen_fd = -1, lock_fd = -1;

void stop_listening() {
    if (listen_fd < 0) return;
    ::close(listen_fd);
    listen_fd = -1;
    ::unlink(sock_path.c_str());
    ::unlink((sock_path + ".lock").c_str());  // (one that opened it meanwhile serves a socket nobody finds, and ends idle)
    ::close(lock_fd);
    lock_fd = -1;
}

void handle(const std::shared_ptr<Client>& cl, uint8_t op, uint32_t id, std::string_view body) {
    Reader r(body.data(), body.size());
    Writer w;
    if (op == kProxyEnv) {
        cl->env.resize(r.u32());
        for (std::string& e : cl->env) e = r.str();
        return;
    }
    if (op == kHello) {
        r.u32();
        const uint32_t level = r.n >= 4 ? r.u32() : 0;  // (an older blit says only the version)
        std::string name;
        if (r.n > 0) {
            name = r.str();
            const uint32_t idle = r.u32(), keep = r.u32();
            if (r.ok && idle) idle_for = std::chrono::seconds(idle);
            if (r.ok && keep) keep_for = std::chrono::seconds(keep);
        }
        if (!listening) name.clear();  // (the agent of one connection keeps nothing)
        if (cl->session && cl->session->client == cl) detach(*cl->session);
        std::shared_ptr<Session> s = session_named(name);
        if (s->client && s->client != cl) {  // (the connection it came on before, not yet seen to have gone)
            close_client(*s->client);
            s->client->session.reset();
            detach(*s);
        }
        s->client = cl;
        s->level = level;
        s->env = cl->env;
        cl->session = s;
        quick = false;
        struct utsname u;
        const std::string sys = ::uname(&u) == 0 ? std::format("{} {}", u.sysname, u.machine) : "unknown";
        w.str(std::string(kAgentVersion)).str(home()).str(sys).u32(static_cast<uint32_t>(::getpid())).u8(!name.empty());
        return reply(*cl, id, w);
    }
    if (!cl->session) {  // (asked for something before hello: a session of its own, that does not last)
        cl->session = session_named("");
        cl->session->client = cl;
    }
    Session& s = *cl->session;
    switch (op) {
        case kStat: {
            const std::string path = r.str();
            struct stat st;
            if (::stat(path.c_str(), &st) != 0) w.u8(0).u64(0).i64(0).u32(0);
            else w.u8(S_ISREG(st.st_mode) ? 1 : S_ISDIR(st.st_mode) ? 2 : 3).u64(static_cast<uint64_t>(st.st_size)).i64(mtime_ns(st)).u32(st.st_mode);
            return reply(*cl, id, w);
        }
        case kRead: {
            std::string path = r.str();
            const uint64_t most = r.u64();
            return on_its_own([cl, id, path = std::move(path), most] {
                std::string data;
                int64_t m = 0;
                const uint8_t status = read_file(path, most, &data, &m);
                Writer out;
                out.u8(status).str(data).i64(m);
                reply(*cl, id, out);
            });
        }
        case kWrite: {
            const std::string path = r.str(), data = r.str();
            std::string err;
            int64_t m = 0;
            const bool ok = write_file(path, data, &err, &m);
            w.u8(ok).str(err).i64(m);
            return reply(*cl, id, w);
        }
        case kWritePart: {
            const uint32_t number = r.u32();
            const uint8_t part = r.u8();
            std::string path = r.str();
            const uint32_t k = r.u32();
            const std::string_view data(r.p, std::min<size_t>(k, r.n));  // (as it is in the frame: not copied)
            Writing& f = s.writings[number];
            if (part & kFirst) f.begin(std::move(path));
            f.add(data);
            if (!(part & kLast)) return;
            std::string err;
            int64_t m = 0;
            const bool ok = f.finish(&err, &m);
            s.writings.erase(number);
            w.u8(ok).str(err).i64(m);
            return reply(*cl, id, w);
        }
        case kList: {
            const std::string path = r.str();
            DIR* d = ::opendir(path.c_str());
            if (!d) {
                w.u8(0).u32(0);
                return reply(*cl, id, w);
            }
            std::vector<std::pair<std::string, uint8_t>> items;
            while (const dirent* e = ::readdir(d)) {
                const std::string_view name = e->d_name;
                if (name == "." || name == "..") continue;
                uint8_t kind = e->d_type == DT_DIR ? 2 : e->d_type == DT_REG ? 1 : 0;
                if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
                    struct stat st;
                    if (::stat(std::format("{}/{}", path, name).c_str(), &st) == 0) kind = S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
                }
                if (kind) items.emplace_back(name, kind);
            }
            ::closedir(d);
            w.u8(1).u32(static_cast<uint32_t>(items.size()));
            for (const auto& [name, kind] : items) w.str(name).u8(kind);
            return reply(*cl, id, w);
        }
        case kMkdir: {
            const std::string path = r.str();
            const bool parents = r.u8() != 0;
            std::string err;
            const bool ok = make_dirs(path, parents, &err);
            w.u8(ok).str(err);
            return reply(*cl, id, w);
        }
        case kCreate: {
            const std::string path = r.str();
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd >= 0) ::close(fd);
            w.u8(fd >= 0).str(fd >= 0 ? std::string() : std::format("cannot make {}: {}", path, std::strerror(errno)));
            return reply(*cl, id, w);
        }
        case kRename: {
            const std::string from = r.str(), to = r.str();
            const bool ok = ::rename(from.c_str(), to.c_str()) == 0;
            w.u8(ok).str(ok ? std::string() : std::format("cannot rename {}: {}", from, std::strerror(errno)));
            return reply(*cl, id, w);
        }
        case kTrash: {
            std::string err;
            const bool ok = trash(r.str(), &err);
            w.u8(ok).str(err);
            return reply(*cl, id, w);
        }
        case kRealpath: {
            const std::string path = r.str();
            char real[PATH_MAX];
            const bool ok = ::realpath(path.c_str(), real) != nullptr;
            w.u8(ok).str(ok ? std::string(real) : path);
            return reply(*cl, id, w);
        }
        case kWhich: {
            w.str(which(r.str(), s.var("PATH")));
            return reply(*cl, id, w);
        }
        case kWalk: {
            std::string root = r.str();
            blit::FinderOptions o;
            o.gitignore = r.u8() != 0;
            o.hidden = r.u8() != 0;
            o.max_files = r.u32();
            o.ignore.resize(r.u32());
            for (std::string& rule : o.ignore) rule = r.str();
            return on_its_own([cl, id, root = std::move(root), o = std::move(o)] {
                std::shared_ptr<const blit::FileIndex> ix = blit::Finder::walk(root, o);
                uint32_t number;
                {
                    std::lock_guard lock(walks_mu);
                    number = next_walk++;
                    walks[number] = ix;
                    while (walks.size() > 4) walks.erase(walks.begin());
                }
                Writer out;
                out.u32(number).u8(ix->truncated).u32(static_cast<uint32_t>(ix->size()));
                for (size_t i = 0; i < ix->size(); ++i) {
                    const std::string_view p = ix->path(i);
                    out.str(p.data(), p.size());
                }
                reply(*cl, id, out);
            });
        }
        case kGrep: {
            const uint32_t number = r.u32();
            std::vector<uint32_t> only(r.u32());
            for (uint32_t& f : only) f = r.u32();
            const std::string typed = r.str();
            const uint64_t most_bytes = r.u64();
            const uint32_t most_lines = r.u32();
            std::shared_ptr<const blit::FileIndex> ix;
            {
                std::lock_guard lock(walks_mu);
                if (auto it = walks.find(number); it != walks.end()) ix = it->second;
            }
            return on_its_own([cl, id, ix, only = std::move(only), typed, most_bytes, most_lines] {
                const std::vector<blit::FuzzyTerm> terms = blit::fuzzy_terms(typed);
                std::vector<blit::FindHit> hits;
                std::vector<uint32_t> hit_files;
                std::atomic<bool> full{false};
                if (ix && !terms.empty()) {
                    // Several threads, each taking the next file (as the finder does).
                    const size_t count = only.empty() ? ix->size() : only.size();
                    std::atomic<size_t> next{0};
                    std::mutex m;
                    auto work = [&] {
                        std::string text;
                        std::vector<blit::FindHit> found;
                        for (size_t k; !full.load() && (k = next.fetch_add(1)) < count;) {
                            const uint32_t file = only.empty() ? static_cast<uint32_t>(k) : only[k];
                            if (file >= ix->size()) continue;
                            int64_t mt = 0;
                            if (read_file(std::format("{}/{}", ix->root, ix->path(file)), most_bytes, &text, &mt, true) != 0 || text.empty()) continue;
                            found.clear();
                            if (!blit::Finder::grep(text, terms, file, most_lines, &found) || found.empty()) continue;
                            std::lock_guard lock(m);
                            hit_files.push_back(file);
                            for (blit::FindHit& h : found) hits.push_back(std::move(h));
                            if (hits.size() >= most_lines) full.store(true);
                        }
                    };
                    {
                        std::vector<std::jthread> helpers;
                        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
                        for (unsigned t = 1; t < threads; ++t) helpers.emplace_back(work);
                        work();
                    }
                }
                std::ranges::sort(hits, [](const blit::FindHit& a, const blit::FindHit& b) { return a.file != b.file ? a.file < b.file : a.line < b.line; });
                if (hits.size() > most_lines) hits.resize(most_lines);
                std::ranges::sort(hit_files);
                Writer out;
                out.u8(full.load()).u32(static_cast<uint32_t>(hits.size()));
                for (const blit::FindHit& h : hits) out.u32(h.file).u32(h.line).u32(h.col).str(h.text);
                out.u32(static_cast<uint32_t>(hit_files.size()));
                for (const uint32_t f : hit_files) out.u32(f);
                reply(*cl, id, out);
            });
        }
        case kSpawn: {
            const uint32_t proc = r.u32();
            const std::string cwd = r.str();
            std::vector<std::string> argv(r.u32());
            for (std::string& a : argv) a = r.str();
            std::vector<std::string> env(r.u32());
            for (std::string& e : env) e = r.str();
            const uint8_t flags = r.n > 0 ? r.u8() : 0;  // (none from a blit before them)
            std::string done = r.n > 0 ? r.str() : std::string();
            std::string err = argv.empty() ? "nothing to run" : "";
            const bool ok = !argv.empty() && spawn(s, proc, cwd, std::move(argv), env, (flags & kQuiet) != 0, &err);
            if (ok) {
                Child& c = s.children[proc];
                c.keep = (flags & kKeep) != 0, c.work = (flags & kWork) != 0;
                c.done = std::move(done);
            }
            w.u8(ok).str(err);
            return reply(*cl, id, w);
        }
        case kSpawnPty: {
            const uint32_t proc = r.u32();
            const std::string cwd = r.str();
            const uint32_t rows = r.u32(), cols = r.u32();
            std::vector<std::string> argv(r.u32());
            for (std::string& a : argv) a = r.str();
            std::vector<std::string> env(r.u32());
            for (std::string& e : env) e = r.str();
            const uint8_t flags = r.n > 0 ? r.u8() : 0;
            blit::Pty p;
            std::string err;
            const bool ok = blit::pty_spawn(argv, cwd, env, rows, cols, &p, &err, s.env.empty() ? nullptr : &s.env);  // (in the connection's environment)
            if (ok) {
                Child& c = s.children[proc];
                c.pid = p.pid, c.in = p.fd, c.out = p.fd, c.pty = true;
                c.keep = (flags & kKeep) != 0, c.work = (flags & kWork) != 0;
            }
            w.u8(ok).str(err);
            return reply(*cl, id, w);
        }
        case kResize: {
            const uint32_t proc = r.u32(), rows = r.u32(), cols = r.u32();
            const uint32_t px_w = r.n >= 8 ? r.u32() : 0, px_h = r.n >= 4 ? r.u32() : 0;  // (in pixels: from blit 3)
            if (auto it = s.children.find(proc); it != s.children.end() && it->second.pty && it->second.out >= 0) blit::pty_resize(it->second.out, rows, cols, px_w, px_h);
            return;
        }
        case kStdin: {
            const uint32_t proc = r.u32();
            const std::string data = r.str();
            if (auto it = s.children.find(proc); it != s.children.end()) {
                Child& c = it->second;
                if (data.empty()) c.close_in = true;
                else c.pending += data;
                if (c.pending.size() - c.pending_at > kInMost) in_flow(s, proc, c, true);
            }
            return;
        }
        case kKill: {
            const uint32_t proc = r.u32();
            const int sig = static_cast<int>(r.u32());
            if (auto it = s.children.find(proc); it != s.children.end() && it->second.pid > 0 && !it->second.exited) ::kill(it->second.pid, sig);
            return;
        }
        case kFlow: {
            const uint32_t proc = r.u32();
            const bool paused = r.u8() != 0;
            if (auto it = s.children.find(proc); it != s.children.end()) it->second.paused = paused;
            return;
        }
        case kBusy: {
            if (auto it = s.children.find(r.u32()); it != s.children.end()) ++it->second.busy;
            return;
        }
        case kResume: {
            std::map<uint32_t, std::array<uint64_t, 3>> had;
            for (uint32_t n = r.u32(); n > 0 && r.ok; --n) {
                const uint32_t proc = r.u32();
                had[proc] = {0, r.u64(), r.u64()};
            }
            // What blit no longer knows of is of no use to anyone: it ends.
            for (auto& [proc, c] : s.children)
                if (c.behind && !had.contains(proc)) c.behind = false, end_child(c);
            w.u32(static_cast<uint32_t>(had.size()));
            for (const auto& [proc, n] : had) {
                const auto it = s.children.find(proc);
                w.u32(proc).u8(it != s.children.end() && !it->second.ending);
            }
            reply(*cl, id, w);
            for (const auto& [proc, n] : had) {
                auto it = s.children.find(proc);
                if (it == s.children.end() || !it->second.behind) continue;
                send_kept(*cl, proc, it->second, n.data());
                it->second.behind = false;
                it->second.was_working = !it->second.working();  // (told again below)
            }
            return;
        }
        case kGoodbye: {
            const bool keep = r.u8() != 0;
            for (auto& [proc, c] : s.children)
                if (!keep || !c.working()) end_child(c);
                else c.behind = true;  // (nobody will come for it: it runs to its end)
            w.u8(1);
            reply(*cl, id, w);
            quick = true;
            return;
        }
        case kStop: {
            end_all();
            return;
        }
        default: {
            w.u8(0).str("unknown request");
            return reply(*cl, id, w);
        }
    }
}

// ---------------------------------------------------------------- the loop

// Told to end (SIGTERM; and SIGHUP for the agent of one connection, which
// is what ssh here is sent): as when blit goes.
volatile std::sig_atomic_t told_to_end = 0;

// Runs until nothing holds it: listening on listen_fd, the socket blits
// come in by; or (none) the agent of one connection, `first`, which ends
// when that does.
void run(std::shared_ptr<Client> first) {
    listening = listen_fd >= 0;
    std::vector<std::shared_ptr<Client>> clients;
    if (first) clients.push_back(first);
    char buf[65536];
    Clock::time_point idle_since = Clock::now(), alone_since = Clock::now(), looked{};
    for (;;) {
        const Clock::time_point now = Clock::now();
        if (told_to_end) end_all();
        // Who has gone.
        for (auto it = clients.begin(); it != clients.end();) {
            Client& c = **it;
            bool gone;
            {
                std::lock_guard lock(c.mu);
                gone = c.gone;
            }
            if (!gone) {
                ++it;
                continue;
            }
            if (c.session && c.session->client == *it) detach(*c.session);
            c.session.reset();
            if (c.in >= 0) ::close(c.in);
            if (c.out >= 0 && c.out != c.in) ::close(c.out);
            c.in = c.out = -1;
            it = clients.erase(it);
        }
        if (!listening && clients.empty()) end_all();  // (the agent of one connection: its blit has gone)
        // What holds the agent: a blit attached, or something working.
        bool working = false;
        const bool look = now - looked >= std::chrono::milliseconds(500);  // (each terminal's front program, now and then)
        if (look) looked = now;
        for (const std::shared_ptr<Session>& s : sessions) {
            hurry_children(*s);
            finish_children(*s);
            for (auto& [id, c] : s->children) {
                if (!c.keep) continue;
                const bool w = look ? c.working() : c.was_working;
                working |= w;
                if (w != c.was_working && s->client && !c.behind && s->level >= 4) {
                    Writer m;
                    m.u32(id).u8(w);
                    send_frame(*s->client, kWorking, 0, m.b);
                }
                c.was_working = w;
            }
        }
        std::erase_if(sessions, [](const std::shared_ptr<Session>& s) { return !s->client && s->children.empty(); });
        if (!clients.empty()) alone_since = now;
        if (!clients.empty() || working) idle_since = now;
        if (listening && !ending_all && clients.empty()) {
            if (!working && now - idle_since >= (quick ? std::chrono::seconds(0) : idle_for)) end_all();
            if (now - alone_since >= keep_for) end_all();
        }
        if (ending_all) {
            bool left = false;
            for (const std::shared_ptr<Session>& s : sessions) left |= !s->children.empty();
            if (!left || now - ending_since >= kLeaveAfter) return;
            for (const std::shared_ptr<Client>& c : clients) close_client(*c);
        }
        // What to wait for: blits coming and what they send, each program's output and input.
        std::vector<pollfd> fds;
        std::vector<std::pair<Child*, int>> who;  // for each program's: (it, 0 input / 1 output / 2 errors)
        std::vector<Session*> whose;
        fds.push_back({wake_r, POLLIN, 0});
        const size_t first_client = fds.size();
        if (listen_fd >= 0) fds.push_back({listen_fd, POLLIN, 0});
        const size_t clients_at = fds.size();
        for (const std::shared_ptr<Client>& c : clients) fds.push_back({c->in, static_cast<short>(POLLIN | (c->waiting() ? POLLOUT : 0)), 0});
        const size_t children_at = fds.size();
        for (const std::shared_ptr<Session>& s : sessions) {
            const bool backed = s->client && s->client->waiting() > kOutMost;
            for (auto& [id, c] : s->children) {
                // (A paused program's output waits in its pipe, and once that is full, the program waits.)
                const bool read = !backed && !(c.paused && s->client);
                if (c.out >= 0 && read) fds.push_back({c.out, POLLIN, 0}), who.emplace_back(&c, 1), whose.push_back(s.get());
                if (c.err >= 0 && read) fds.push_back({c.err, POLLIN, 0}), who.emplace_back(&c, 2), whose.push_back(s.get());
                if (c.in >= 0 && c.pending_at < c.pending.size()) fds.push_back({c.in, POLLOUT, 0}), who.emplace_back(&c, 0), whose.push_back(s.get());
                else if (c.in >= 0 && c.close_in && !c.pty) ::close(c.in), c.in = -1;  // (a terminal's input is its output: it stays)
            }
        }
        if (::poll(fds.data(), fds.size(), 50) < 0 && errno != EINTR) return;
        if (fds[0].revents & POLLIN)
            while (::read(wake_r, buf, sizeof buf) > 0) {}
        if (listening && listen_fd >= 0 && fds.size() > first_client && fds[first_client].fd == listen_fd && (fds[first_client].revents & POLLIN)) {
            for (;;) {
                const int c = ::accept(listen_fd, nullptr, nullptr);
                if (c < 0) break;
                ::fcntl(c, F_SETFD, FD_CLOEXEC);
                ::fcntl(c, F_SETFL, ::fcntl(c, F_GETFL) | O_NONBLOCK);
#if defined(__linux__)
                ucred cred{};
                socklen_t len = sizeof cred;
                const bool mine = ::getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == ::getuid();
#else
                uid_t uid = 0;
                gid_t gid = 0;
                const bool mine = ::getpeereid(c, &uid, &gid) == 0 && uid == ::getuid();
#endif
                if (!mine) {  // (only this user's: the folder is theirs alone, but even so)
                    ::close(c);
                    continue;
                }
                auto cl = std::make_shared<Client>();
                cl->in = cl->out = c;
                clients.push_back(cl);
            }
        }
        // What blits sent; and what waits to go to them, sent as it can.
        for (size_t i = 0; clients_at + i < children_at && i < clients.size(); ++i) {
            const std::shared_ptr<Client>& cl = clients[i];
            const short ev = fds[clients_at + i].revents;
            if (ev & POLLOUT) {
                std::lock_guard lock(cl->mu);
                cl->flush();
            }
            if (!(ev & (POLLIN | POLLHUP | POLLERR))) continue;
            for (;;) {
                const ssize_t got = ::read(cl->in, buf, sizeof buf);
                if (got > 0) {
                    cl->got.append(buf, static_cast<size_t>(got));
                    continue;
                }
                if (got == 0 || (errno != EAGAIN && errno != EINTR)) close_client(*cl);
                break;
            }
            size_t at = 0;  // each whole frame there is
            while (cl->got.size() - at >= 4) {
                const uint32_t size = Reader(cl->got.data() + at, 4).u32();
                if (size < 5 || cl->got.size() - at - 4 < size) break;
                Reader f(cl->got.data() + at + 4, size);
                const uint8_t op = f.u8();
                const uint32_t id = f.u32();
                handle(cl, op, id, std::string_view(cl->got).substr(at + 9, size - 5));
                at += 4 + size;
            }
            cl->got.erase(0, at);
        }
        // What the programs wrote, and what they are to be given.
        for (size_t i = 0; children_at + i < fds.size(); ++i) {
            if (!fds[children_at + i].revents) continue;
            Child& c = *who[i].first;
            Session& s = *whose[i];
            const uint32_t id = [&] {
                for (const auto& [k, v] : s.children)
                    if (&v == &c) return k;
                return 0u;
            }();
            if (who[i].second == 0) {
                if (c.in < 0) continue;
                const ssize_t put = ::write(c.in, c.pending.data() + c.pending_at, c.pending.size() - c.pending_at);
                if (put > 0) c.pending_at += static_cast<size_t>(put);
                else if (put < 0 && errno != EAGAIN && errno != EINTR) c.pending.clear(), c.pending_at = 0, c.close_in = !c.pty;
                if (c.pending_at == c.pending.size()) c.pending.clear(), c.pending_at = 0;
                else if (c.pending_at > (64u << 10) && c.pending_at * 2 > c.pending.size()) c.pending.erase(0, c.pending_at), c.pending_at = 0;  // (taken: let go of)
                if (c.pending.size() - c.pending_at <= kInLess) in_flow(s, id, c, false);
                continue;
            }
            int& fd = who[i].second == 1 ? c.out : c.err;
            if (fd < 0) continue;
            const ssize_t got = ::read(fd, buf, sizeof buf);
            if (got > 0) {
                const std::string_view b(buf, static_cast<size_t>(got));
                if (c.keep) {
                    c.keep_output(static_cast<uint8_t>(who[i].second), b);
                    c.watch_done(b);
                }
                if (s.client && !c.behind && !c.ending) {
                    Writer w;
                    w.u32(id).u8(static_cast<uint8_t>(who[i].second)).str(buf, static_cast<size_t>(got));
                    send_frame(*s.client, kOutput, 0, w.b);
                }
            } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                ::close(fd);
                fd = -1;
                if (c.pty) c.in = -1, c.pending.clear(), c.pending_at = 0, in_flow(s, id, c, false);  // (the same descriptor)
            }
        }
    }
}

// ---------------------------------------------------------------- where it lives

// The folder the agent's socket is in: this user's, which only they can
// enter (none if it is not so: then there is no lasting agent).
std::string socket_dir() {
    const char* run = ::getenv("BLIT_AGENT_RUN");  // (the tests': a folder of their own, or one that will not do)
    const std::string dir = run && *run ? std::string(run) : std::format("/tmp/blit-{}", ::getuid());
    ::mkdir(dir.c_str(), 0700);
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077) != 0) return {};
    return dir;
}

// The agent's socket: one for each agent (each version, each place it was
// copied to: the tests' homes are places of their own).
std::string socket_path(const char* self) {
    char real[PATH_MAX];
    const std::string me = ::realpath(self, real) ? std::string(real) : std::string(self);
    uint64_t h = 1469598103934665603ull;
    for (const char c : me) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    const std::string dir = socket_dir();
    if (dir.empty()) return {};
    const std::string path = std::format("{}/{:016x}", dir, h);
    return path.size() < sizeof(sockaddr_un{}.sun_path) ? path : std::string();
}

int connect_to(const std::string& path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) return -1;
    if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(c);
        return -1;
    }
    ::fcntl(c, F_SETFD, FD_CLOEXEC);
    return c;
}

// Whether a program can be run in a systemd scope of this user's (where
// logind ends what a login started when it ends, that is what lasts).
bool scope_works() {
#if defined(__linux__)
    if (!::getenv("XDG_RUNTIME_DIR") && ::access(std::format("/run/user/{}/bus", ::getuid()).c_str(), F_OK) != 0) return false;
    const char* argv[] = {"systemd-run", "--user", "--scope", "--quiet", "--collect", "true", nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (const int fd : {0, 1, 2}) posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, argv[0], &fa, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#else
    return false;
#endif
}

// The lasting agent, started: in a session of its own, away from this
// connection's (whose end, a hangup, is not its end), and in a scope of
// its own where systemd would end it with the login.
void start_daemon(const char* self, const std::string& path) {
    const pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid > 0) {
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        return;
    }
    ::setsid();
    if (::fork() != 0) std::_Exit(0);
    const int null = ::open("/dev/null", O_RDWR);
    for (const int fd : {0, 1, 2}) ::dup2(null, fd);
    for (int fd = 3; fd < 1024; ++fd) ::close(fd);
    if (const char* h = ::getenv("HOME"); h && *h) [[maybe_unused]] const int rc = ::chdir(h);
    if (scope_works()) {
        const char* argv[] = {"systemd-run", "--user", "--scope", "--quiet", "--collect", self, "--serve", path.c_str(), nullptr};
        ::execvp(argv[0], const_cast<char* const*>(argv));
    }
    const char* argv[] = {self, "--serve", path.c_str(), nullptr};
    ::execv(self, const_cast<char* const*>(argv));
    std::_Exit(1);
}

void make_wake_pipe() {
    int p[2];
    if (::pipe(p) != 0) return;
    for (const int fd : p) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    wake_r = p[0], wake_w = p[1];
}

// The agent of one connection: on this process's input and output.
int serve_here() {
    for (const int sig : {SIGTERM, SIGHUP}) std::signal(sig, [](int) { told_to_end = 1; });
    make_wake_pipe();
    auto cl = std::make_shared<Client>();
    cl->in = 0, cl->out = 1;
    for (const int fd : {0, 1}) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    run(cl);
    {
        std::lock_guard lock(cl->mu);  // (what is left to say, said if it can be)
        ::fcntl(1, F_SETFL, ::fcntl(1, F_GETFL) & ~O_NONBLOCK);
        cl->flush();
    }
    std::_Exit(0);  // (no waiting for the finder's threads)
}

// The lasting agent: blits come to it by its socket.
int serve(const std::string& path) {
    std::signal(SIGHUP, SIG_IGN);  // (a hangup is a connection's end, not its)
    std::signal(SIGTERM, [](int) { told_to_end = 1; });
    const std::string lock_path = path + ".lock";
    const int lock = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0) return 0;  // (one is running: it serves)
    ::unlink(path.c_str());
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const mode_t was = ::umask(077);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(fd, 16) == 0;
    ::umask(was);
    if (!bound) return 1;
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    make_wake_pipe();
    sock_path = path, listen_fd = fd, lock_fd = lock;
    run(nullptr);
    stop_listening();  // (before the lock goes: a new one makes its own)
    std::_Exit(0);
}

// What ssh runs: the bytes between ssh and the lasting agent, which is
// started if it is not running. Where there can be none, this is the agent
// of this connection.
int attach(const char* self) {
    const std::string path = socket_path(self);
    if (path.empty()) return serve_here();
    int fd = -1;
    for (int tries = 0; fd < 0 && tries < 250; ++tries) {  // (5 s: another may be starting, or one ending)
        fd = connect_to(path);
        if (fd >= 0) break;
        if (tries % 50 == 0) start_daemon(self, path);
        ::usleep(20'000);
    }
    if (fd < 0) return serve_here();
    // This connection's environment (the PATH the bootstrap found, its
    // SSH_AUTH_SOCK), for the programs started on it.
    Writer env;
    uint32_t n = 0;
    Writer vars;
    for (char** e = environ; e && *e; ++e) vars.str(*e, std::strlen(*e)), ++n;
    env.u32(n);
    env.b += vars.b;
    const std::string f = frame(kProxyEnv, 0, env.b);
    if (::write(fd, f.data(), f.size()) != static_cast<ssize_t>(f.size())) return 1;
    // Then each way, as it comes, until either end goes.
    char buf[65536];
    pollfd p[2] = {{0, POLLIN, 0}, {fd, POLLIN, 0}};
    auto pass = [&](int from, int to) {
        const ssize_t k = ::read(from, buf, sizeof buf);
        if (k < 0 && errno == EINTR) return true;
        if (k <= 0) return false;
        for (ssize_t done = 0; done < k;) {
            const ssize_t w = ::write(to, buf + done, static_cast<size_t>(k - done));
            if (w > 0) done += w;
            else if (w < 0 && errno == EINTR) continue;
            else return false;
        }
        return true;
    };
    for (;;) {
        if (::poll(p, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && !pass(0, fd)) break;
        if ((p[1].revents & (POLLIN | POLLHUP | POLLERR)) && !pass(fd, 1)) break;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "--attach") return attach(argv[0]);
    if (mode == "--serve" && argc > 2) return serve(argv[2]);
    return serve_here();
}
