#include "remote.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "agent/protocol.hpp"
#include "finder.hpp"

extern char** environ;

namespace blit {

namespace {

using namespace blit_wire;

// The agents blit carries, one for each kind of machine. Linux's are built
// by agent/build.sh and kept with the sources; this machine's own kind by
// CMake, with blit.
#if __has_embed("agent/bin/blit-agent-linux-x86_64")
constexpr unsigned char kLinuxX64[] = {
#embed "agent/bin/blit-agent-linux-x86_64"
};
#endif
#if __has_embed("agent/bin/blit-agent-linux-aarch64")
constexpr unsigned char kLinuxArm[] = {
#embed "agent/bin/blit-agent-linux-aarch64"
};
#endif
#if defined(BLIT_AGENT_HERE) && __has_embed(BLIT_AGENT_HERE)
constexpr unsigned char kHere[] = {
#embed BLIT_AGENT_HERE
};
#endif

struct AgentBinary {
    std::string_view system;  // as `uname -s` and `uname -m` say it there
    std::string_view bytes;
};

std::vector<AgentBinary> agent_binaries() {
    std::vector<AgentBinary> out;
#if __has_embed("agent/bin/blit-agent-linux-x86_64")
    out.push_back({"Linux x86_64", {reinterpret_cast<const char*>(kLinuxX64), sizeof kLinuxX64}});
#endif
#if __has_embed("agent/bin/blit-agent-linux-aarch64")
    out.push_back({"Linux aarch64", {reinterpret_cast<const char*>(kLinuxArm), sizeof kLinuxArm}});
    out.push_back({"Linux arm64", {reinterpret_cast<const char*>(kLinuxArm), sizeof kLinuxArm}});
#endif
#if defined(BLIT_AGENT_HERE) && __has_embed(BLIT_AGENT_HERE)
#if defined(__APPLE__) && defined(__aarch64__)
    out.push_back({"Darwin arm64", {reinterpret_cast<const char*>(kHere), sizeof kHere}});
#elif defined(__APPLE__)
    out.push_back({"Darwin x86_64", {reinterpret_cast<const char*>(kHere), sizeof kHere}});
#endif
#endif
    return out;
}

// A name for an agent that changes when the agent does (so that an old one
// on a machine is never used by a newer blit, nor the other way round).
std::string agent_name(std::string_view bytes, std::string_view system) {
    u64 h = 1469598103934665603ull;
    for (const char c : bytes) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    std::string sys(system);
    for (char& c : sys)
        if (c == ' ') c = '-';
    return std::format("blit-agent-{}-{:016x}", sys, h);
}

// What ssh runs there (as one command, in the user's shell, whatever shell
// that is: so it is a plain sh script, and has no ' in it). It finds the
// PATH a login shell would have (where language servers and git usually
// are), says what the machine is, takes the agent's name, and runs it,
// first copying it from blit if it is not there.
constexpr std::string_view kBootstrap =
    "p=$(\"${SHELL:-/bin/sh}\" -lc \"printf \\\"\\\\nBLIT-PATH=%s\\\\n\\\" \\\"\\$PATH\\\"\" </dev/null 2>/dev/null | sed -n \"s/^BLIT-PATH=//p\" | tail -n 1); "
    "[ -n \"$p\" ] && PATH=\"$p\" && export PATH; "
    "d=\"$HOME/.cache/blit\"; echo \"BLIT $(uname -s) $(uname -m)\"; read f; f=\"$d/$f\"; "
    "if [ -x \"$f\" ]; then echo BLIT-HAVE; else echo BLIT-NEED; read n; "
    "mkdir -p \"$d\" && head -c \"$n\" > \"$f.tmp.$$\" && chmod +x \"$f.tmp.$$\" && mv \"$f.tmp.$$\" \"$f\" || { rm -f \"$f.tmp.$$\"; echo BLIT-FAILED; exit 1; }; fi; "
    "echo BLIT-START; exec \"$f\" --attach";

std::mutex registry_mu;
std::map<std::string, std::shared_ptr<Remote>> registry;
std::function<void()> notify_fn;
std::string askpass_program;  // (see Remote::set_askpass)

bool write_all(int fd, const char* p, size_t n);

// ssh's questions, waiting for their answers (see AskServer).
struct AskSlot {
    RemoteAsk ask;
    bool done = false, cancelled = false;
    std::string answer;
};
std::mutex asks_mu;
std::condition_variable asks_cv;
std::map<u64, AskSlot*> pending_asks;
u64 next_ask = 1;

void wipe(std::string& s) {  // (an answer is not left lying in memory)
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

// Where ssh asks its questions while one connection is being made: a
// socket in a folder only this user can enter, which the askpass program
// (blit, see remote_askpass) connects to with each question. Gone when
// the connection is made, or has failed.
class AskServer {
public:
    AskServer(const std::string& host, std::function<void()> told) : host_(host), told_(std::move(told)) {
        const char* tmp = std::getenv("TMPDIR");
        std::string dir = std::string(tmp && *tmp ? tmp : "/tmp");
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        dir += "/blit-ask-XXXXXX";
        if (!::mkdtemp(dir.data())) return;
        dir_ = dir;
        path_ = dir_ + "/s";
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (path_.size() >= sizeof a.sun_path) return;
        std::memcpy(a.sun_path, path_.c_str(), path_.size() + 1);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return;
        ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 4) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        thread_ = std::jthread([this](std::stop_token stop) { serve(stop); });
    }
    ~AskServer() {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) ::close(fd_);
        if (!path_.empty()) ::unlink(path_.c_str());
        if (!dir_.empty()) ::rmdir(dir_.c_str());
    }
    bool ok() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

private:
    // Who is at the other end of a local socket (-1 if that cannot be told):
    // BSD's call on macOS, the socket's credentials on Linux.
    static uid_t peer_uid(int c) {
#if defined(__linux__)
        ucred cred{};
        socklen_t len = sizeof cred;
        return ::getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 ? cred.uid : static_cast<uid_t>(-1);
#else
        uid_t uid = 0;
        gid_t gid = 0;
        return ::getpeereid(c, &uid, &gid) == 0 ? uid : static_cast<uid_t>(-1);
#endif
    }
    void serve(std::stop_token stop) {
        while (!stop.stop_requested()) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            ::fcntl(c, F_SETFD, FD_CLOEXEC);
            one(c, stop);
            ::close(c);
        }
    }
    // One question: read it, wait for the answer, send it back.
    void one(int c, std::stop_token& stop) {
        if (peer_uid(c) != ::getuid()) return;  // (only this user's programs are answered)
        std::string prompt;
        char buf[512];
        for (;;) {
            pollfd p{c, POLLIN, 0};
            if (::poll(&p, 1, 5000) <= 0) return;
            const ssize_t k = ::read(c, buf, sizeof buf);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            prompt.append(buf, static_cast<size_t>(k));
            if (prompt.size() > 4096) return;
        }
        while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r' || prompt.back() == ' ')) prompt.pop_back();
        AskSlot slot;
        slot.ask.host = host_;
        slot.ask.prompt = prompt;
        std::string lower = prompt;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        slot.ask.secret = lower.find("yes/no") == std::string::npos;
        {
            std::lock_guard lock(asks_mu);
            slot.ask.id = next_ask++;
            pending_asks[slot.ask.id] = &slot;
        }
        if (told_) told_();
        {
            std::unique_lock lock(asks_mu);
            while (!slot.done && !stop.stop_requested()) asks_cv.wait_for(lock, std::chrono::milliseconds(100));
            pending_asks.erase(slot.ask.id);
        }
        if (told_) told_();  // (the box asking it goes, if it was not answered there)
        if (slot.done && !slot.cancelled) {
            slot.answer += '\n';
            write_all(c, slot.answer.data(), slot.answer.size());
        }
        wipe(slot.answer);
    }

    std::string host_, dir_, path_;
    std::function<void()> told_;
    int fd_ = -1;
    std::jthread thread_;
};

// This program, by its full path (what ssh is told to run to ask).
std::string this_program() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return {};
    char real[PATH_MAX];
    return ::realpath(buf, real) ? real : buf;
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
#endif
}

bool write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t k = ::write(fd, p, n);
        if (k > 0) p += k, n -= static_cast<size_t>(k);
        else if (k < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

// Reads exactly n bytes (false on the end, or an error).
bool read_all(int fd, char* p, size_t n) {
    while (n > 0) {
        const ssize_t k = ::read(fd, p, n);
        if (k > 0) p += k, n -= static_cast<size_t>(k);
        else if (k < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

// A frame's head: its length (counting `more` bytes that follow the payload,
// sent after it as they are), operation, number, and the payload.
std::string frame_head(u8 op, u32 id, const std::string& payload, size_t more) {
    Writer w;
    w.u32(static_cast<u32>(5 + payload.size() + more)).u8(op).u32(id);
    w.b += payload;
    return std::move(w.b);
}

// A program's output held here for its reader: past kHoldMost the agent is
// told to stop reading it (kFlow), and under kHoldLess to read on. What was
// already on its way still comes; past kHoldHard (or past kHoldMost, from an
// agent that cannot be told) no more frames are read until its reader has
// taken some, which holds up the whole connection: the last resort.
constexpr size_t kHoldMost = 4u << 20, kHoldLess = 1u << 20, kHoldHard = 16u << 20;
// What a program writes to its input waits to go, at most this much at a time
// (more waits for room); a request is never kept waiting for room.
constexpr size_t kSendMost = 4u << 20;
// What is big goes to the agent behind what is small (keys, requests): a
// file saved, in parts of kPart, and what programs that are not terminals
// are sent (a language server's text); no more than kBulkMost of it waits at
// once, so that a key waits behind no more than a part of it.
constexpr size_t kPart = 256u << 10, kBulkMost = 1u << 20;

}  // namespace

#if !defined(BLIT_NO_REMOTE)
bool is_remote(std::string_view path) { return path.starts_with("ssh://") || path.starts_with("docker://"); }

// (A container's "host" is "docker:NAME": a name ssh never has.)
bool split_remote(std::string_view path, std::string* host, std::string* there) {
    if (!is_remote(path)) return false;
    const bool docker = path.starts_with("docker://");
    path.remove_prefix(docker ? 9 : 6);
    const size_t slash = path.find('/');
    *host = std::string(path.substr(0, slash));
    *there = slash == std::string_view::npos ? std::string("/") : std::string(path.substr(slash));
    if (host->empty() || host->front() == '-') return false;  // (never one ssh or docker would take for an option)
    if (docker) *host = "docker:" + *host;
    return true;
}
#endif

std::string remote_prefix(std::string_view host) {
    return host.starts_with("docker:") ? std::format("docker://{}", host.substr(7)) : std::format("ssh://{}", host);
}

std::string remote_path(std::string_view host, std::string_view there) {
    return std::format("{}{}{}", remote_prefix(host), there.starts_with('/') ? "" : "/", there);
}

std::string program_path() { return this_program(); }

int remote_askpass(const char* socket_path, const char* prompt) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (std::strlen(socket_path) >= sizeof a.sun_path) return 1;
    std::strcpy(a.sun_path, socket_path);
    const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0 || ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return 1;
    write_all(c, prompt, std::strlen(prompt));
    ::shutdown(c, SHUT_WR);
    std::string answer;
    char buf[512];
    for (ssize_t k; (k = ::read(c, buf, sizeof buf)) != 0;) {
        if (k > 0) answer.append(buf, static_cast<size_t>(k));
        else if (errno != EINTR) break;
    }
    ::close(c);
    if (answer.empty()) return 1;  // not answered: ssh gives up
    const bool ok = write_all(1, answer.data(), answer.size());
    wipe(answer);
    return ok ? 0 : 1;
}

std::shared_ptr<Remote> remote_for(std::string_view path, std::string* there, int wait_ms) {
    std::string host;
    if (!split_remote(path, &host, there)) return nullptr;
    std::shared_ptr<Remote> r = Remote::get(host);
    if (r->state() == Remote::kReady || (wait_ms > 0 && r->wait_ready(wait_ms))) return r;
    return nullptr;
}

std::string remote_error(std::string_view path) {
    std::string host, there;
    if (!split_remote(path, &host, &there)) return "not on another machine: " + std::string(path);
    std::shared_ptr<Remote> r;
    {
        std::lock_guard lock(registry_mu);
        if (auto it = registry.find(host); it != registry.end()) r = it->second;
    }
    if (!r) return std::format("not connected to {}", host);
    switch (r->state()) {
        case Remote::kConnecting: return std::format("still connecting to {}", host);
        case Remote::kFailed: return r->error();
        default: return std::format("no answer from {}", host);
    }
}

// ---------------------------------------------------------------- the connection

namespace {
// How long the agent there waits with nothing to do and nobody attached,
// and with something kept running and nobody attached (Remote::set_agent_times).
u32 agent_idle_s = 600, agent_keep_s = 86400;
// A connection that goes is made again: first at once, then waiting longer
// each time; but no more than kTriesMost connections are begun in kTriesNs
// (a firewall's limit on new connections counts the agent's too: ufw's
// LIMIT blocks after six in 30 s).
constexpr int kTriesMost = 4;
constexpr u64 kTriesNs = 30'000'000'000ull;
constexpr u64 kBackoffMs[] = {500, 2000, 5000, 10'000, 20'000, 30'000, 60'000};
// A worker that wants the connection waits for it while it is being made
// again, for this long after it went (a save just after a drop goes once
// the connection is back).
constexpr u64 kQueueNs = 10'000'000'000ull;

u64 wall_ns() { return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

// What ssh says when it was refused, rather than not reaching the host:
// such a one is not tried again by itself (a login node that asks for a
// second factor is not to be asked over and over).
bool refused(std::string_view said) {
    for (const std::string_view s : {"Permission denied", "Authentication failed", "Too many authentication failures", "Host key verification failed"})
        if (said.find(s) != std::string_view::npos) return true;
    return false;
}

std::string new_session_name() {
    std::random_device rd;
    return std::format("{:08x}{:08x}{:08x}{:08x}", rd(), rd(), static_cast<u32>(::getpid()), static_cast<u32>(now_ns()));
}
}  // namespace

struct Remote::Impl {
    Remote* self = nullptr;
    mutable std::mutex mu;
    std::condition_variable cv;
    // kConnecting, kReady (from the agent's start: calls may go; up once
    // it has said hello, and taken back what it kept), kFailed (the
    // connection went, or could not be made).
    State state = kConnecting;
    bool up = false;
    bool gone = false;  // this connection has gone (or could not be made): the connection's thread lets go of it
    bool dropped = false;  // it went after it was up (and has not been up since)
    u64 failed_ns = 0;  // when it went
    std::string error, progress, home, system;
    std::string agent_file;  // the agent's name there, in ~/.cache/blit
    std::string ssh_errors;  // the last of what ssh said on its error stream
    pid_t ssh = -1;
    int to = -1, from = -1, errs = -1;  // ssh's input (written here), output and errors (read here)
    u32 agent_level = 0;  // what the agent can do (protocol.hpp's kAgentLevel, as it says it)
    bool lasting = false; // the agent outlives the connection (it says so in hello)
    u32 next_id = 1;
    // Making it again. The connection's own thread (live) makes it, waits for
    // it to go, and makes it again when it is asked to (get, reconnect,
    // nudge) or by itself, while it is wanted.
    const std::string session = new_session_name();  // blit's name for itself there, the same each time
    bool closing = false;      // closed: made no more
    bool ever_up = false;      // it has been up: once it goes, it is made again by itself
    bool refused_once = false; // ssh was refused: not made again until asked to by hand
    bool stopped = false;      // its agent was told to stop: likewise
    bool asked = false;        // a new connection is wanted now
    u64 retry_ns = 0;          // when it is to be made again by itself (0: it is not)
    int backoff = 0;
    std::deque<u64> tries;     // when the last connections were begun
    std::atomic<bool> wanted{false};  // something is open there (Remote::set_wanted)
    // What is to go to ssh's input, in order, written by a thread of its own:
    // nobody waits for a long write (a big file being saved) but whoever
    // waits for its answer. Its own lock, never taken with mu's held the
    // other way round (mu first, then this one, if both).
    struct Out {
        std::string head, tail;  // (tail: a file's text, sent after its request's head without being copied into it)
    };
    std::mutex out_mu;
    std::condition_variable out_cv;  // something to write, or room for more
    std::deque<Out> outq, bulkq;     // (bulkq: what is big, written when nothing waits in outq)
    size_t out_bytes = 0, bulk_bytes = 0;
    u32 next_write = 1;  // (a file written in parts: its number, out_mu)
    bool out_done = false;    // nothing more goes: the writer lets go of ssh's input (and closes it)
    bool out_finish = false;  // what waits still goes, and then the writer closes it (quitting: the goodbye)
    std::jthread writer;
    struct Slot {
        bool done = false, failed = false;
        std::string body;
    };
    std::map<u32, Slot*> pending;
    // The programs running there.
    struct Proc {
        int out_w = -1, err_w = -1, in_r = -1;  // this end of the pipes blit's code was given the other end of
        std::deque<std::pair<int, std::string>> queue;  // output to put into the pipes (stream, bytes); stream 0: the end
        size_t held = 0;      // bytes of output in the queue, or being written into a pipe
        bool paused = false;  // the agent has been told to stop reading its output (kFlow)
        bool in_paused = false;  // and it has told blit to hold what goes to its input (kInFlow)
        bool quiet = false;   // its errors are not wanted (an older agent sends them anyway: they are dropped)
        bool keep = false;    // it outlives the connection (kKeep)
        bool working = false; // as the agent counts it (kWorking)
        u64 had[3] = {0, 0, 0};  // how much of each stream has come (what the agent is told in kResume)
        bool exited = false, ended = false;
        int status = -1;
        std::jthread pump_in, pump_out;
    };
    std::map<u32, std::unique_ptr<Proc>> procs;
    u32 next_proc = 1;
    std::condition_variable room;  // a program's held output has been taken (for the reader, when it waits for that)
    size_t held_most = 0;
    std::jthread live_thread, reader, err_reader;

    // Made again now (mu is held): connecting from here on, as far as anyone
    // asking can tell (the connection's thread is woken to do it).
    void again_now() {
        asked = true;
        state = kConnecting;
        progress = std::format("{} to {}", ever_up ? "reconnecting" : "connecting", self->host_);
    }

    // Whether it is to be made again by itself when it goes (mu is held).
    bool again_by_itself() const {
        if (!ever_up || refused_once || stopped || closing) return false;
        for (const auto& [id, p] : procs)
            if (p->keep && !p->exited) return true;
        return wanted.load();
    }

    // The connection has gone, or could not be made. What waits for an
    // answer fails; what it was running fails with it, but what the agent
    // keeps running, which waits for the connection to come back.
    void fail(const std::string& why) {
        {
            std::lock_guard lock(mu);
            if (gone) return;
            state = kFailed;
            // (While it is being made again after it went, why it went is
            // what is said, until it is refused.)
            if (up) dropped = true;
            if (up || !dropped || refused_once) error = why;
            up = false;
            gone = true;
            failed_ns = now_ns();
            retry_ns = again_by_itself() ? ~0ull : 0;  // (when: decided once the connection has been let go of)
            for (auto& [id, slot] : pending) slot->done = slot->failed = true;
            for (auto& [id, p] : procs)
                if (!p->exited && !(p->keep && lasting && !closing && !stopped)) {
                    p->exited = true;
                    p->status = -1;
                    p->queue.push_back({0, {}});
                }
        }
        cv.notify_all();
        room.notify_all();
        stop_writing();
        if (notify_fn) notify_fn();
    }

    // Nothing more is written: what waits to go is dropped (but when it is
    // to finish: quitting's goodbye), and the writer closes ssh's input once
    // what it is writing now has gone (or failed). True if there is a
    // writer to do that.
    bool stop_writing() {
        bool writing;
        {
            std::lock_guard lock(out_mu);
            out_done = true;
            if (!out_finish) {
                outq.clear();
                bulkq.clear();
                out_bytes = bulk_bytes = 0;
            }
            writing = writer.joinable();
        }
        out_cv.notify_all();
        return writing;
    }

    void write_frames() {
        bool broken = false;  // (ssh has gone: the reader says so; until then what comes is dropped)
        for (;;) {
            Out o;
            bool bulk;
            {
                std::unique_lock lock(out_mu);
                out_cv.wait(lock, [&] { return out_done || !outq.empty() || !bulkq.empty(); });
                if (outq.empty() && bulkq.empty()) break;
                if (out_done && !out_finish) break;
                bulk = outq.empty();  // (what is small first)
                std::deque<Out>& q = bulk ? bulkq : outq;
                o = std::move(q.front());
                q.pop_front();
            }
            if (!broken) broken = !write_all(to, o.head.data(), o.head.size()) || !write_all(to, o.tail.data(), o.tail.size());
            {
                std::lock_guard lock(out_mu);
                size_t& bytes = bulk ? bulk_bytes : out_bytes;
                bytes -= std::min(bytes, o.head.size() + o.tail.size());
            }
            out_cv.notify_all();
        }
        ::close(to);
    }

    // A frame, to go when those before it in its queue have (tail: more of
    // it, after head; bulk: the queue of what is big, which goes when nothing
    // else waits). may_wait: the caller waits while much is waiting to go
    // already (a program's input, a file's parts); a request, or anything from
    // the main thread, never does. False if nothing more goes on this connection.
    bool enqueue(std::string head, std::string tail = {}, bool may_wait = false, bool bulk = false) {
        {
            std::unique_lock lock(out_mu);
            size_t& bytes = bulk ? bulk_bytes : out_bytes;
            if (may_wait) out_cv.wait(lock, [&] { return out_done || bytes < (bulk ? kBulkMost : kSendMost); });
            if (out_done) return false;
            bytes += head.size() + tail.size();
            (bulk ? bulkq : outq).push_back({std::move(head), std::move(tail)});
        }
        out_cv.notify_all();
        return true;
    }

    void set_progress(std::string what) {
        {
            std::lock_guard lock(mu);
            progress = std::move(what);
        }
        if (notify_fn) notify_fn();
    }

    // A line of the bootstrap's text (not the newline), waiting no longer than ms.
    bool line(std::string* out, int ms) {
        out->clear();
        for (;;) {
            pollfd p{from, POLLIN, 0};
            if (::poll(&p, 1, ms) <= 0) return false;
            char c;
            const ssize_t k = ::read(from, &c, 1);  // (one at a time: what follows the last line is the agent's)
            if (k <= 0) return false;
            if (c == '\n') return true;
            *out += c;
        }
    }

    // The connection's own thread: it makes the connection, waits for it to
    // go, and makes it again (when asked, or by itself while it is wanted).
    void live() {
        for (;;) {
            // No more than kTriesMost begun in kTriesNs.
            {
                std::unique_lock lock(mu);
                for (;;) {
                    while (!tries.empty() && now_ns() - tries.front() >= kTriesNs) tries.pop_front();
                    if (closing || static_cast<int>(tries.size()) < kTriesMost) break;
                    cv.wait_for(lock, std::chrono::nanoseconds(tries.front() + kTriesNs - now_ns()));
                }
                if (closing) return;
                tries.push_back(now_ns());
                state = kConnecting;
                up = false;
                gone = false;
                retry_ns = 0;
                asked = false;
                ssh_errors.clear();
            }
            {
                std::lock_guard lock(out_mu);
                out_done = false;
                outq.clear();
                bulkq.clear();
                out_bytes = bulk_bytes = 0;
            }
            if (notify_fn) notify_fn();
            connect();
            {
                std::unique_lock lock(mu);
                cv.wait(lock, [&] { return gone; });
            }
            let_go();
            // Again: when asked, or by itself after a wait that grows.
            std::unique_lock lock(mu);
            if (closing) return;
            const bool by_itself = again_by_itself();
            retry_ns = 0;
            if (by_itself) {
                const u64 wait = kBackoffMs[std::min<size_t>(static_cast<size_t>(backoff), std::size(kBackoffMs) - 1)];
                ++backoff;
                retry_ns = now_ns() + wait * 1'000'000;
            }
            progress = by_itself ? std::format("reconnecting to {}", self->host_) : std::string();
            lock.unlock();
            if (notify_fn) notify_fn();
            lock.lock();
            // (A machine that slept: the wall clock has gone on where this one
            // did not. Woken, it tries again at once.)
            u64 wall = wall_ns(), mono = now_ns();
            while (!closing && !asked && !(retry_ns && now_ns() >= retry_ns)) {
                cv.wait_for(lock, std::chrono::seconds(1));
                const u64 w2 = wall_ns(), m2 = now_ns();
                if (retry_ns && static_cast<i64>(w2 - wall) - static_cast<i64>(m2 - mono) > 5'000'000'000ll) break;
                wall = w2, mono = m2;
            }
            if (closing) return;
        }
    }

    // What one connection had: ssh, and the threads that served it.
    void let_go() {
        pid_t pid;
        {
            std::lock_guard lock(mu);
            pid = ssh;
            ssh = -1;
        }
        if (pid > 0) {
            for (int i = 0; i < 20 && ::waitpid(pid, nullptr, WNOHANG) == 0; ++i) ::usleep(10'000);
            if (::waitpid(pid, nullptr, WNOHANG) == 0) {
                ::kill(pid, SIGTERM);
                ::waitpid(pid, nullptr, 0);
            }
        }
        if (writer.joinable()) writer.join();
        else if (to >= 0) ::close(to);
        if (reader.joinable()) reader.join();
        if (err_reader.joinable()) err_reader.join();
        if (from >= 0) ::close(from);
        if (errs >= 0) ::close(errs);
        to = from = errs = -1;
        std::lock_guard lock(out_mu);
        out_finish = false;
    }

    void connect() {
        const std::string& host = self->host_;
        // ssh, with its input, output and errors on pipes here.
        int in[2], out[2], er[2];
        if (::pipe(in) || ::pipe(out) || ::pipe(er)) return fail("cannot make a pipe");
        for (const int fd : {in[1], out[0], er[0]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        const char* ssh_env = std::getenv("BLIT_SSH");  // (the tests' stand-in for ssh)
        // ssh's questions (a password, a passphrase, a new host's key) come
        // to blit, which shows them; with nobody to ask, ssh asks nothing.
        std::unique_ptr<AskServer> asking;
        std::string program;
        {
            std::lock_guard lock(registry_mu);
            program = askpass_program;
        }
        if (!program.empty()) {
            asking = std::make_unique<AskServer>(host, notify_fn);
            if (!asking->ok()) asking.reset();
        }
        std::vector<std::string> words = {ssh_env && *ssh_env ? ssh_env : "ssh", "-T", "-o", asking ? "BatchMode=no" : "BatchMode=yes", "-o", "ServerAliveInterval=15",
                                          "-o", "ServerAliveCountMax=4", "-o", "ConnectTimeout=20", "--", host,
                                          std::format("exec /bin/sh -c '{}'", kBootstrap)};
        if (host.starts_with("docker:")) {  // a container: the same, through docker exec (nothing to ask)
            const char* docker_env = std::getenv("BLIT_DOCKER");  // (the tests' stand-in for docker)
            words = {docker_env && *docker_env ? docker_env : "docker", "exec", "-i", host.substr(7), "/bin/sh", "-c", std::string(kBootstrap)};
            asking.reset();
        }
        std::vector<char*> argv;
        for (std::string& w : words) argv.push_back(w.data());
        argv.push_back(nullptr);
        std::vector<std::string> env_words;
        for (char** e = environ; e && *e; ++e) {
            const std::string_view v(*e);
            if (asking && (v.starts_with("SSH_ASKPASS=") || v.starts_with("SSH_ASKPASS_REQUIRE=") || v.starts_with("BLIT_ASKPASS_SOCKET="))) continue;
            env_words.emplace_back(v);
        }
        if (asking) {
            env_words.push_back("SSH_ASKPASS=" + program);
            env_words.push_back("SSH_ASKPASS_REQUIRE=force");
            env_words.push_back("BLIT_ASKPASS_SOCKET=" + asking->path());
        }
        std::vector<char*> envp;
        for (std::string& e : env_words) envp.push_back(e.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_adddup2(&fa, er[1], 2);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__)
        posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif
        pid_t pid = -1;
        const int rc = ::posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&fa);
        ::close(in[0]), ::close(out[1]), ::close(er[1]);
        {
            std::lock_guard lock(mu);
            ssh = rc == 0 ? pid : -1;
        }
        to = in[1], from = out[0], errs = er[0];
        if (rc != 0) return fail(std::format("cannot run ssh: {}", std::strerror(rc)));
        // What ssh says goes nowhere until it is needed to say why something failed.
        err_reader = std::jthread([this] {
            char buf[4096];
            for (ssize_t k; (k = ::read(errs, buf, sizeof buf)) > 0;) {
                std::lock_guard lock(mu);
                ssh_errors.append(buf, static_cast<size_t>(k));
                if (ssh_errors.size() > 4096) ssh_errors.erase(0, ssh_errors.size() - 4096);
            }
        });
        auto why = [&](std::string_view what) {
            ::usleep(100'000);  // (ssh's own reason may come just after)
            std::lock_guard lock(mu);
            std::string said = ssh_errors;
            if (refused(said)) refused_once = true;
            while (!said.empty() && (said.back() == '\n' || said.back() == '\r')) said.pop_back();
            if (const size_t nl = said.rfind('\n'); nl != std::string::npos) said = said.substr(nl + 1);
            return said.empty() ? std::string(what) : std::format("{}: {}", what, said);
        };
        set_progress(std::format("{} to {}", ever_up ? "reconnecting" : "connecting", host));
        std::string l;
        do {  // (a person typing a password may take a while)
            if (!line(&l, asking ? 600'000 : 30'000)) return fail(why(std::format("cannot connect to {}", host)));
        } while (!l.starts_with("BLIT "));
        asking.reset();  // (in: ssh has nothing more to ask)
        const std::string sys = l.substr(5);
        const AgentBinary* agent = nullptr;
        const std::vector<AgentBinary> agents = agent_binaries();
        for (const AgentBinary& a : agents)
            if (a.system == sys) agent = &a;
        if (!agent) return fail(std::format("blit has no agent for {} ({})", host, sys));
        const std::string name = agent_name(agent->bytes, agent->system) + "\n";
        {
            std::lock_guard lock(mu);
            agent_file = name.substr(0, name.size() - 1);
        }
        if (!write_all(to, name.data(), name.size()) || !line(&l, 30'000)) return fail(why(std::format("{} stopped answering", host)));
        if (l == "BLIT-NEED") {
            set_progress(std::format("copying blit's agent to {}", host));
            const std::string size = std::format("{}\n", agent->bytes.size());
            if (!write_all(to, size.data(), size.size()) || !write_all(to, agent->bytes.data(), agent->bytes.size()) || !line(&l, 120'000))
                return fail(why(std::format("cannot copy blit's agent to {}", host)));
        } else if (l == "BLIT-HAVE" && !line(&l, 30'000)) {
            return fail(why(std::format("{} stopped answering", host)));
        }
        if (l != "BLIT-START") return fail(why(std::format("cannot start blit's agent on {}", host)));
        reader = std::jthread([this] { read_frames(); });
        {
            std::lock_guard lock(out_mu);
            if (!out_done) writer = std::jthread([this] { write_frames(); });  // (from here on, only it writes to ssh)
        }
        // Hello: the agent's version, and where home is there; and blit's
        // name for itself there, which the agent keeps what it ran under.
        std::string body;
        Writer w;
        u32 idle = agent_idle_s;
        if (const char* e = std::getenv("BLIT_AGENT_IDLE"); e && *e) idle = static_cast<u32>(std::max(1, std::atoi(e)));  // (the tests')
        w.u32(kVersion).u32(kBlitLevel).str(session).u32(idle).u32(agent_keep_s);
        {
            std::lock_guard lock(mu);
            if (gone) return;  // (closed meanwhile)
            state = kReady;  // (for the call: set back below if it fails)
        }
        if (!call(kHello, w.b, &body, 30'000)) return fail(why(std::format("blit's agent on {} did not answer", host)));
        Reader r(body.data(), body.size());
        const std::string version = r.str();  // "blit-agent 2"
        u32 level = static_cast<u32>(std::atoi(version.c_str() + std::min(version.size(), version.find_last_of(' ') + 1)));
        // (BLIT_AGENT_LEVEL: the tests' way to meet an agent from before what a level brings.)
        if (const char* as = std::getenv("BLIT_AGENT_LEVEL"); as && *as) level = std::min(level, static_cast<u32>(std::atoi(as)));
        const std::string home_there = r.str(), system_there = r.str();
        r.u32();
        const bool lasts = level >= 4 && r.n > 0 && r.u8() != 0;
        // What it kept running, taken back: what each wrote meanwhile comes
        // after the answer. (One it does not have is gone: the machine
        // restarted, or it was the agent of one connection.)
        std::vector<u32> kept;
        {
            std::lock_guard lock(mu);
            for (const auto& [id, p] : procs)
                if (p->keep && !p->exited) kept.push_back(id);
        }
        std::vector<u32> lost;
        if (lasts) {
            Writer k;
            k.u32(static_cast<u32>(kept.size()));
            {
                std::lock_guard lock(mu);
                for (const u32 id : kept) k.u32(id).u64(procs[id]->had[1]).u64(procs[id]->had[2]);
            }
            std::string answer;
            if (!call(kResume, k.b, &answer, 30'000)) return fail(why(std::format("blit's agent on {} did not answer", host)));
            Reader a(answer.data(), answer.size());
            for (u32 n = a.u32(); n > 0 && a.ok; --n) {
                const u32 id = a.u32();
                if (!a.u8()) lost.push_back(id);
            }
        } else {
            lost = kept;
        }
        {
            std::lock_guard lock(mu);
            for (const u32 id : lost)
                if (auto it = procs.find(id); it != procs.end() && !it->second->exited) {
                    it->second->exited = true;
                    it->second->status = -1;
                    it->second->queue.push_back({0, {}});
                }
            agent_level = level;
            lasting = lasts;
            home = home_there;
            system = system_there;
            progress.clear();
            up = true;
            ever_up = true;
            dropped = false;
            error.clear();
            backoff = 0;
        }
        cv.notify_all();
        if (notify_fn) notify_fn();
    }

    void read_frames() {
        std::string body;
        for (;;) {
            char head[4];
            if (!read_all(from, head, 4)) break;
            const u32 size = Reader(head, 4).u32();
            if (size < 5 || size > (1u << 30)) break;
            body.resize(size);
            if (!read_all(from, body.data(), size)) break;
            Reader f(body.data(), size);
            const u8 op = f.u8();
            const u32 id = f.u32();
            const std::string_view rest = std::string_view(body).substr(5);
            if (op == kReply) {
                std::lock_guard lock(mu);
                if (auto it = pending.find(id); it != pending.end()) {
                    it->second->body = rest;
                    it->second->done = true;
                }
                cv.notify_all();
            } else if (op == kInFlow || op == kWorking) {  // (kInFlow: a program has more of what it was sent than it has taken: what it is sent waits)
                Reader r(rest.data(), rest.size());
                const u32 proc = r.u32();
                const bool on = r.u8() != 0;
                std::lock_guard lock(mu);
                if (auto it = procs.find(proc); it != procs.end()) (op == kInFlow ? it->second->in_paused : it->second->working) = on;
                cv.notify_all();
            } else if (op == kOutput || op == kExit) {
                Reader r(rest.data(), rest.size());
                const u32 proc = r.u32();
                std::unique_lock lock(mu);
                auto it = procs.find(proc);
                if (it == procs.end()) continue;
                Proc& p = *it->second;
                if (op == kOutput) {
                    const int stream = r.u8();
                    std::string bytes = r.str();
                    if (stream == 1 || stream == 2) p.had[stream] += bytes.size();
                    if (stream == 2 && p.quiet) continue;  // (an older agent's: nobody here reads them)
                    p.held += bytes.size();
                    held_most = std::max(held_most, p.held);
                    p.queue.push_back({stream, std::move(bytes)});
                    if (p.held > kHoldMost && !p.paused && agent_level >= 2) {
                        p.paused = true;
                        flow(proc, true);
                    }
                } else {
                    p.exited = true;
                    p.working = false;
                    p.status = static_cast<int>(r.u32());
                    p.queue.push_back({0, {}});
                }
                cv.notify_all();
                // Far more than its reader has taken: nothing more is read
                // until it has taken some (an agent that was told to pause
                // it does not get here but for what was already on its way).
                if (op == kOutput && p.held > (agent_level >= 2 ? kHoldHard : kHoldMost)) {
                    Proc* const held = &p;  // (not looked at once the connection has failed: it may be gone then)
                    room.wait(lock, [&] { return state == kFailed || held->held <= kHoldLess; });
                }
            }
        }
        fail(std::format("the connection to {} was lost", self->host_));
    }

    // A request, and its answer (false if there is none: no connection, or it
    // went). tail: bytes that end the request, as they are (a file's text);
    // bulk: it goes behind what is small (enqueue). (Until the agent has said
    // hello and given back what it kept, only those go.)
    bool call(u8 op, const std::string& payload, std::string* answer, int ms = 60'000, std::string tail = {}, bool bulk = false) {
        Slot slot;
        u32 id;
        {
            std::lock_guard lock(mu);
            if (state != kReady || (!up && op != kHello && op != kResume)) return false;
            id = next_id++;
            pending[id] = &slot;
        }
        const size_t more = tail.size();
        const bool sent = enqueue(frame_head(op, id, payload, more), std::move(tail), bulk, bulk);
        std::unique_lock lock(mu);
        if (sent) cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return slot.done; });
        pending.erase(id);
        if (!slot.done || slot.failed) return false;
        *answer = std::move(slot.body);
        return true;
    }

    // A message that has no answer. may_wait, bulk: as enqueue's.
    void send(u8 op, const std::string& payload, bool may_wait = false, bool bulk = false) {
        {
            std::lock_guard lock(mu);
            if (state != kReady || !up) return;
        }
        enqueue(frame_head(op, 0, payload, 0), {}, may_wait, bulk);
    }

    // The agent is to stop reading a program's output, or to read on (mu is held).
    void flow(u32 proc, bool paused) {
        if (state != kReady) return;
        Writer w;
        w.u32(proc).u8(paused);
        enqueue(frame_head(kFlow, 0, w.b, 0));
    }

    // Closed: blit is quitting, or the connection is let go of. With an
    // agent that lasts, it is told first (what is working is kept, or not).
    void close(bool keep) {
        bool goodbye;
        {
            std::lock_guard lock(mu);
            if (closing) return;
            closing = true;
            goodbye = up && lasting;
            if (!up && ssh > 0) ::kill(ssh, SIGTERM);  // (still connecting: a password may be waited for)
        }
        if (goodbye) {
            Writer w;
            w.u8(keep);
            {
                std::lock_guard lock(out_mu);
                out_finish = true;  // (it goes before the connection is let go of)
            }
            enqueue(frame_head(kGoodbye, 0, w.b, 0));
        }
        cv.notify_all();
        fail("closed");
    }
};

Remote::Remote(std::string host) : host_(std::move(host)), impl_(std::make_unique<Impl>()) { impl_->self = this; }

Remote::~Remote() {
    Impl& m = *impl_;
    m.close(false);
    // The agent sees its input end (the writer closes it, once what it is
    // writing has gone); ssh then ends, and the connection's thread with it.
    {
        std::lock_guard lock(m.out_mu);
        m.out_done = true;
    }
    m.out_cv.notify_all();
    if (m.live_thread.joinable()) m.live_thread.join();
    for (auto& [id, p] : m.procs) {
        if (p->pump_out.joinable()) p->pump_out.join();
        p->pump_in.request_stop();
        if (p->pump_in.joinable()) p->pump_in.join();
        if (p->in_r >= 0) ::close(p->in_r);
    }
}

std::shared_ptr<Remote> Remote::get(const std::string& host) {
    // A write to a connection that has just gone must fail, not end blit.
    static const bool quiet = (std::signal(SIGPIPE, SIG_IGN), true);
    (void)quiet;
    std::lock_guard lock(registry_mu);
    if (auto it = registry.find(host); it != registry.end()) {
        // One that failed is made again, when asked for a while after (not
        // over and over while the host cannot be reached; and not one that
        // was refused, or whose agent was stopped, until asked to by hand).
        constexpr u64 kAgainNs = 3'000'000'000ull;
        Impl& m = *it->second->impl_;
        {
            std::lock_guard lock_m(m.mu);
            if (m.state == kFailed && !m.asked && !m.refused_once && !m.stopped && now_ns() - m.failed_ns >= kAgainNs) m.again_now();
        }
        m.cv.notify_all();
        return it->second;
    }
    std::shared_ptr<Remote> r(new Remote(host));
    registry[host] = r;
    Remote* raw = r.get();
    raw->impl_->live_thread = std::jthread([raw] { raw->impl_->live(); });
    return r;
}

void Remote::set_notify(std::function<void()> fn) { notify_fn = std::move(fn); }

void Remote::set_askpass(std::string program) {
    std::lock_guard lock(registry_mu);
    askpass_program = std::move(program);
}

void Remote::set_agent_times(u32 idle_s, u32 keep_s) {
    std::lock_guard lock(registry_mu);
    agent_idle_s = std::max<u32>(idle_s, 1);
    agent_keep_s = std::max<u32>(keep_s, 1);
}

void Remote::set_wanted(const std::vector<std::string>& hosts) {
    std::lock_guard lock(registry_mu);
    for (auto& [host, r] : registry) r->impl_->wanted = std::ranges::find(hosts, host) != hosts.end();
}

std::vector<RemoteAsk> Remote::asks() {
    std::lock_guard lock(asks_mu);
    std::vector<RemoteAsk> out;
    for (const auto& [id, slot] : pending_asks)
        if (!slot->done) out.push_back(slot->ask);
    return out;
}

void Remote::answer(u64 id, const std::string* text) {
    {
        std::lock_guard lock(asks_mu);
        auto it = pending_asks.find(id);
        if (it == pending_asks.end()) return;
        it->second->done = true;
        it->second->cancelled = text == nullptr;
        if (text) it->second->answer = *text;
    }
    asks_cv.notify_all();
}

void Remote::reconnect(const std::string& host) {
    std::shared_ptr<Remote> r;
    {
        std::lock_guard lock(registry_mu);
        auto it = registry.find(host);
        if (it == registry.end()) return;
        r = it->second;
    }
    Impl& m = *r->impl_;
    {
        std::lock_guard lock(m.mu);
        if (m.state != kFailed) return;
        m.refused_once = m.stopped = false;  // (by hand: whatever stopped it)
        m.again_now();
    }
    m.cv.notify_all();
}

void Remote::nudge() {
    for (const std::shared_ptr<Remote>& r : all()) {
        Impl& m = *r->impl_;
        {
            std::lock_guard lock(m.mu);
            if (m.state != kFailed || !m.retry_ns) continue;  // (only one that is being made again by itself)
            m.again_now();
        }
        m.cv.notify_all();
    }
}

std::vector<std::shared_ptr<Remote>> Remote::all() {
    std::lock_guard lock(registry_mu);
    std::vector<std::shared_ptr<Remote>> out;
    for (auto& [host, r] : registry) out.push_back(r);
    return out;
}

void Remote::close_all(bool keep_working) {
    std::map<std::string, std::shared_ptr<Remote>> gone;
    {
        std::lock_guard lock(registry_mu);
        gone.swap(registry);
    }
    for (auto& [host, r] : gone) r->impl_->close(keep_working);  // (what waits for one is woken, and fails)
}

Remote::State Remote::state() const {
    std::lock_guard lock(impl_->mu);
    return impl_->state == kReady && !impl_->up ? kConnecting : impl_->state;
}
std::string Remote::error() const {
    std::lock_guard lock(impl_->mu);
    return impl_->error;
}
std::string Remote::progress() const {
    std::lock_guard lock(impl_->mu);
    return impl_->progress;
}
bool Remote::retrying() const {
    std::lock_guard lock(impl_->mu);
    return impl_->state == kFailed && impl_->retry_ns != 0;
}
bool Remote::lasting() const {
    std::lock_guard lock(impl_->mu);
    return impl_->lasting;
}
bool Remote::stopped() const {
    std::lock_guard lock(impl_->mu);
    return impl_->stopped;
}
std::string Remote::home() const {
    std::lock_guard lock(impl_->mu);
    return impl_->home;
}
std::string Remote::system() const {
    std::lock_guard lock(impl_->mu);
    return impl_->system;
}
std::string Remote::agent_path() const {
    std::lock_guard lock(impl_->mu);
    return impl_->home.empty() || impl_->agent_file.empty() ? std::string() : impl_->home + "/.cache/blit/" + impl_->agent_file;
}

int Remote::working() const {
    std::lock_guard lock(impl_->mu);
    int n = 0;
    for (const auto& [id, p] : impl_->procs) n += p->keep && p->working && !p->exited;
    return n;
}

bool Remote::stop_agent() {
    Impl& m = *impl_;
    {
        std::lock_guard lock(m.mu);
        if (!m.up || !m.lasting) return false;
        m.stopped = true;
    }
    m.send(kStop, {});
    return true;
}

// Ready, or (for a while after it went, while it is being made again by
// itself) once it is back: a save just after a drop waits for that.
bool Remote::wait_ready(int ms) {
    Impl& m = *impl_;
    const u64 until = now_ns() + static_cast<u64>(std::max(ms, 0)) * 1'000'000;
    std::unique_lock lock(m.mu);
    for (;;) {
        if (m.up && m.state == kReady) return true;
        const bool coming = m.state == kConnecting || (m.state == kReady && !m.up) || (m.retry_ns && now_ns() - m.failed_ns < kQueueNs) || m.asked;
        if (m.closing || !coming || now_ns() >= until) return false;
        m.cv.wait_for(lock, std::chrono::nanoseconds(std::min<u64>(until - now_ns(), 100'000'000)));
    }
}

// ---------------------------------------------------------------- calls

bool Remote::stat(const std::string& path, RemoteStat* out) {
    Writer w;
    w.str(path);
    std::string a;
    if (!impl_->call(kStat, w.b, &a)) return false;
    Reader r(a.data(), a.size());
    out->kind = r.u8();
    out->size = r.u64();
    out->modified_ns = r.i64();
    return r.ok;
}

int Remote::read(const std::string& path, u64 most, std::string* out, i64* modified_ns) {
    Writer w;
    w.str(path).u64(most);
    std::string a;
    if (!impl_->call(kRead, w.b, &a, 300'000)) return 5;
    Reader r(a.data(), a.size());
    const int status = r.u8();
    *out = r.str();
    const i64 m = r.i64();
    if (modified_ns) *modified_ns = m;
    return r.ok ? status : 4;
}

bool Remote::write(const std::string& path, std::string data, std::string* err, i64* modified_ns) {
    Impl& m = *impl_;
    std::string a;
    bool answered;
    u32 level;
    {
        std::lock_guard lock(m.mu);
        level = m.agent_level;
    }
    if (level >= 3 && data.size() > kPart) {
        // In parts, each behind what else is waiting to go (a key to a
        // terminal there waits for a part at most, not the whole file); the
        // last is answered.
        u32 number;
        {
            std::lock_guard lock(m.out_mu);
            number = m.next_write++;
        }
        answered = false;
        for (size_t at = 0; at < data.size();) {
            const size_t n = std::min(kPart, data.size() - at);
            const u8 part = (at == 0 ? kFirst : 0) | (at + n == data.size() ? kLast : 0);
            Writer w;
            w.u32(number).u8(part).str(path).u32(static_cast<u32>(n));
            std::string piece = data.substr(at, n);
            at += n;
            if (part & kLast) answered = m.call(kWritePart, w.b, &a, 300'000, std::move(piece), true);
            else if (!m.enqueue(frame_head(kWritePart, 0, w.b, n), std::move(piece), true, true)) break;
        }
    } else {
        Writer w;
        w.str(path).u32(static_cast<u32>(data.size()));  // (the text itself follows, as it is)
        answered = m.call(kWrite, w.b, &a, 300'000, std::move(data), true);
    }
    if (!answered) {
        if (err) *err = std::format("no connection to {}", host_);
        return false;
    }
    Reader r(a.data(), a.size());
    const bool ok = r.u8() != 0;
    const std::string e = r.str();
    const i64 when = r.i64();
    if (!ok && err) *err = e;
    if (modified_ns) *modified_ns = when;
    return ok;
}

bool Remote::list(const std::string& path, std::vector<RemoteEntry>* out) {
    Writer w;
    w.str(path);
    std::string a;
    if (!impl_->call(kList, w.b, &a)) return false;
    Reader r(a.data(), a.size());
    if (!r.u8()) return false;
    out->resize(r.u32());
    for (RemoteEntry& e : *out) {
        e.name = r.str();
        e.dir = r.u8() == 2;
    }
    return r.ok;
}

namespace {
bool ok_or(Remote::Impl& m, u8 op, const std::string& payload, std::string* err, const std::string& host) {
    std::string a;
    if (!m.call(op, payload, &a)) {
        if (err) *err = std::format("no connection to {}", host);
        return false;
    }
    Reader r(a.data(), a.size());
    const bool ok = r.u8() != 0;
    if (!ok && err) *err = r.str();
    return ok;
}
}  // namespace

bool Remote::make_dir(const std::string& path, bool parents, std::string* err) {
    Writer w;
    w.str(path).u8(parents);
    return ok_or(*impl_, kMkdir, w.b, err, host_);
}
bool Remote::create(const std::string& path, std::string* err) {
    Writer w;
    w.str(path);
    return ok_or(*impl_, kCreate, w.b, err, host_);
}
bool Remote::rename(const std::string& from, const std::string& to, std::string* err) {
    Writer w;
    w.str(from).str(to);
    return ok_or(*impl_, kRename, w.b, err, host_);
}
bool Remote::trash(const std::string& path, std::string* err) {
    Writer w;
    w.str(path);
    return ok_or(*impl_, kTrash, w.b, err, host_);
}

std::string Remote::realpath(const std::string& path) {
    Writer w;
    w.str(path);
    std::string a;
    if (!impl_->call(kRealpath, w.b, &a)) return path;
    Reader r(a.data(), a.size());
    r.u8();
    return r.str();
}

std::string Remote::which(const std::string& program) {
    Writer w;
    w.str(program);
    std::string a;
    if (!impl_->call(kWhich, w.b, &a)) return {};
    Reader r(a.data(), a.size());
    return r.str();
}

bool Remote::walk(const std::string& root, const FinderOptions& o, FileIndex* out, u32* walk_id) {
    Writer w;
    w.str(root).u8(o.gitignore).u8(o.hidden).u32(o.max_files).u32(static_cast<u32>(o.ignore.size()));
    for (const std::string& rule : o.ignore) w.str(rule);
    std::string a;
    if (!impl_->call(kWalk, w.b, &a, 600'000)) return false;
    Reader r(a.data(), a.size());
    *walk_id = r.u32();
    out->truncated = r.u8() != 0;
    const u32 n = r.u32();
    out->root = remote_path(host_, root);
    out->names.clear();
    out->offs.clear();
    out->offs.reserve(n + 1);
    for (u32 i = 0; i < n && r.ok; ++i) {
        out->offs.push_back(static_cast<u32>(out->names.size()));
        out->names += r.str();
    }
    out->offs.push_back(static_cast<u32>(out->names.size()));
    return r.ok;
}

bool Remote::grep(u32 walk_id, const std::vector<u32>* only, const std::string& typed, u64 most_bytes, u32 most_lines, std::vector<FindHit>* hits,
                  std::vector<u32>* hit_files, bool* capped) {
    Writer w;
    w.u32(walk_id).u32(only ? static_cast<u32>(only->size()) : 0);
    if (only)
        for (const u32 f : *only) w.u32(f);
    w.str(typed).u64(most_bytes).u32(most_lines);
    std::string a;
    if (!impl_->call(kGrep, w.b, &a, 600'000)) return false;
    Reader r(a.data(), a.size());
    *capped = r.u8() != 0;
    hits->resize(r.u32());
    for (FindHit& h : *hits) {
        h.file = r.u32();
        h.line = r.u32();
        h.col = r.u32();
        h.text = r.str();
    }
    hit_files->resize(r.u32());
    for (u32& f : *hit_files) f = r.u32();
    return r.ok;
}

// ---------------------------------------------------------------- programs

bool Remote::spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env, RemoteProc* out, std::string* err,
                   unsigned rows, unsigned cols) {
    Impl& m = *impl_;
    int in[2], o[2], e[2];
    if (::pipe(in) || ::pipe(o) || ::pipe(e)) {
        if (err) *err = "cannot make a pipe";
        return false;
    }
    const bool quiet = !out->errors && !rows;
    if (quiet) {  // (no pipe for what nobody reads)
        ::close(e[0]), ::close(e[1]);
        e[0] = e[1] = -1;
    }
    for (const int fd : {in[0], in[1], o[0], o[1], e[0], e[1]})
        if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    auto p = std::make_unique<Impl::Proc>();
    p->in_r = in[0], p->out_w = o[1], p->err_w = e[1];
    p->quiet = quiet;
    p->keep = out->keep;
    u32 id;
    {
        std::lock_guard lock(m.mu);
        id = m.next_proc++;
        m.procs[id] = std::move(p);
    }
    Writer w;
    w.u32(id).str(cwd);
    if (rows) w.u32(rows).u32(cols);
    w.u32(static_cast<u32>(argv.size()));
    for (const std::string& a : argv) w.str(a);
    w.u32(static_cast<u32>(env.size()));
    for (const std::string& v : env) w.str(v);
    const u8 flags = (quiet ? kQuiet : 0) | (out->keep ? kKeep : 0) | (out->work ? kWork : 0);
    if (!rows) w.u8(flags).str(out->done);  // (an agent from before the flags reads no further, and one from before 4 no further than them)
    else w.u8(flags);
    std::string a;
    bool ok = m.call(rows ? kSpawnPty : kSpawn, w.b, &a);
    std::string why = std::format("no connection to {}", host_);
    if (ok) {
        Reader r(a.data(), a.size());
        ok = r.u8() != 0;
        why = r.str();
    }
    if (!ok) {
        {
            std::lock_guard lock(m.mu);
            m.procs.erase(id);
        }
        for (const int fd : {in[0], in[1], o[0], o[1], e[0], e[1]})
            if (fd >= 0) ::close(fd);
        if (err) *err = why;
        return false;
    }
    Impl::Proc* proc;
    {
        std::lock_guard lock(m.mu);
        proc = m.procs[id].get();
    }
    // What is written to its input goes there (a terminal's ahead of what is
    // big, anything else's behind it); what it writes comes back into its pipes.
    proc->pump_in = std::jthread([&m, proc, id, fd = in[0], bulk = rows == 0](std::stop_token stop) {
        char buf[65536];
        while (!stop.stop_requested()) {  // (looked at now and then: a read that waits is not woken by closing its pipe)
            {
                // The program there has not taken what it was sent: nothing more is read until it has
                // (so that whatever writes here waits, as a program writing into a full pipe does).
                // Nor while one kept running there is without its connection: what is typed goes when it is back.
                std::unique_lock lock(m.mu);
                if (proc->keep && !m.up && !proc->exited && !m.closing) {
                    m.cv.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
                if (proc->in_paused) {
                    m.cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !proc->in_paused || m.state == kFailed; });
                    continue;
                }
            }
            pollfd ready{fd, POLLIN, 0};
            if (::poll(&ready, 1, 100) <= 0) continue;
            const ssize_t k = ::read(fd, buf, sizeof buf);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            Writer chunk;
            chunk.u32(id).str(buf, static_cast<size_t>(k));
            m.send(kStdin, chunk.b, true, bulk);  // (a program fed faster than the connection takes it waits)
        }
        Writer done;
        done.u32(id).str("", 0);  // (its input is closed)
        m.send(kStdin, done.b, false, bulk);
    });
    proc->pump_out = std::jthread([&m, proc, id] {
        for (;;) {
            std::pair<int, std::string> item;
            {
                std::unique_lock lock(m.mu);
                m.cv.wait(lock, [&] { return !proc->queue.empty(); });
                item = std::move(proc->queue.front());
                proc->queue.pop_front();
            }
            if (item.first == 0) break;
            // (Into a pipe whose reader has gone, it goes nowhere, at once.)
            const int fd = item.first == 1 ? proc->out_w : proc->err_w;
            if (fd >= 0) write_all(fd, item.second.data(), item.second.size());
            {
                std::lock_guard lock(m.mu);
                proc->held -= std::min(proc->held, item.second.size());
                if (proc->paused && proc->held <= kHoldLess) {  // taken: the agent reads on
                    proc->paused = false;
                    m.flow(id, false);
                }
            }
            m.room.notify_all();
        }
        ::close(proc->out_w);
        if (proc->err_w >= 0) ::close(proc->err_w);
        std::lock_guard lock(m.mu);
        proc->ended = true;
        m.cv.notify_all();
    });
    out->id = id;
    out->in = in[1];
    out->out = o[0];
    out->err = e[0];  // (-1 when its errors are not wanted)
    return true;
}

int Remote::wait(u32 id, int ms) {
    Impl& m = *impl_;
    std::unique_ptr<Impl::Proc> p;
    {
        std::unique_lock lock(m.mu);
        auto it = m.procs.find(id);
        if (it == m.procs.end()) return -1;
        Impl::Proc* proc = it->second.get();
        if (ms < 0) m.cv.wait(lock, [&] { return proc->ended; });
        else if (!m.cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return proc->ended; })) return -2;
        p = std::move(it->second);
        m.procs.erase(it);
    }
    if (p->pump_out.joinable()) p->pump_out.join();
    p->pump_in.request_stop();  // (if the program's own code has not closed its end of the input yet)
    if (p->pump_in.joinable()) p->pump_in.join();
    ::close(p->in_r);
    return p->status;
}

void Remote::resize(u32 proc, unsigned rows, unsigned cols, unsigned width_px, unsigned height_px) {
    Writer w;
    w.u32(proc).u32(rows).u32(cols);
    if (width_px && height_px) w.u32(width_px).u32(height_px);
    impl_->send(kResize, w.b);
}

void Remote::busy(u32 proc) {
    Writer w;
    w.u32(proc);
    impl_->send(kBusy, w.b);
}

void Remote::kill(u32 proc, int sig) {
    Writer w;
    w.u32(proc).u32(static_cast<u32>(sig));
    impl_->send(kKill, w.b);
}

u64 Remote::held_most() const {
    std::lock_guard lock(impl_->mu);
    return impl_->held_most;
}

}  // namespace blit
