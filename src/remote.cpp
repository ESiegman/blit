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
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
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
    "mkdir -p \"$d\" && head -c \"$n\" > \"$f.tmp\" && chmod +x \"$f.tmp\" && mv \"$f.tmp\" \"$f\" || { echo BLIT-FAILED; exit 1; }; fi; "
    "echo BLIT-START; exec \"$f\"";

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
        uid_t uid = 0;
        gid_t gid = 0;
        if (::getpeereid(c, &uid, &gid) != 0 || uid != ::getuid()) return;  // (only this user's programs are answered)
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

}  // namespace

#if !defined(BLIT_NO_REMOTE)
bool is_remote(std::string_view path) { return path.starts_with("ssh://"); }

bool split_remote(std::string_view path, std::string* host, std::string* there) {
    if (!is_remote(path)) return false;
    path.remove_prefix(6);
    const size_t slash = path.find('/');
    *host = std::string(path.substr(0, slash));
    *there = slash == std::string_view::npos ? std::string("/") : std::string(path.substr(slash));
    return !host->empty() && host->front() != '-';  // (never one ssh would take for an option)
}
#endif

std::string remote_path(std::string_view host, std::string_view there) {
    return std::format("ssh://{}{}{}", host, there.starts_with('/') ? "" : "/", there);
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

struct Remote::Impl {
    Remote* self = nullptr;
    mutable std::mutex mu;
    std::condition_variable cv;
    State state = kConnecting;  // (kReady from the agent's start: calls may go; up once it has said hello)
    bool up = false;
    u64 failed_ns = 0;  // when it failed: a connection that has is made again when next asked for, but not at once
    std::string error, progress, home, system;
    std::string ssh_errors;  // the last of what ssh said on its error stream
    pid_t ssh = -1;
    int to = -1, from = -1, errs = -1;  // ssh's input (written here), output and errors (read here)
    std::mutex write_mu;
    u32 next_id = 1;
    struct Slot {
        bool done = false, failed = false;
        std::string body;
    };
    std::map<u32, Slot*> pending;
    // The programs running there.
    struct Proc {
        int out_w = -1, err_w = -1, in_r = -1;  // this end of the pipes blit's code was given the other end of
        std::deque<std::pair<int, std::string>> queue;  // output to put into the pipes (stream, bytes); stream 0: the end
        bool exited = false, ended = false;
        int status = -1;
        std::jthread pump_in, pump_out;
    };
    std::map<u32, std::unique_ptr<Proc>> procs;
    u32 next_proc = 1;
    std::jthread connector, reader, err_reader;

    void fail(const std::string& why) {
        std::vector<std::unique_ptr<Proc>> gone;
        {
            std::lock_guard lock(mu);
            if (state == kFailed) return;
            state = kFailed;
            failed_ns = now_ns();
            error = why;
            for (auto& [id, slot] : pending) slot->done = slot->failed = true;
            for (auto& [id, p] : procs)
                if (!p->exited) {
                    p->exited = true;
                    p->status = -1;
                    p->queue.push_back({0, {}});
                }
        }
        cv.notify_all();
        if (notify_fn) notify_fn();
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
#endif
        const int rc = ::posix_spawnp(&ssh, argv[0], &fa, &attr, argv.data(), envp.data());
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&fa);
        ::close(in[0]), ::close(out[1]), ::close(er[1]);
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
            while (!said.empty() && (said.back() == '\n' || said.back() == '\r')) said.pop_back();
            if (const size_t nl = said.rfind('\n'); nl != std::string::npos) said = said.substr(nl + 1);
            return said.empty() ? std::string(what) : std::format("{}: {}", what, said);
        };
        set_progress(std::format("connecting to {}", host));
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
        // Hello: the agent's version, and where home is there.
        std::string body;
        Writer w;
        w.u32(kVersion);
        {
            std::lock_guard lock(mu);
            if (state == kFailed) return;  // (closed meanwhile)
            state = kReady;  // (for the call: set back below if it fails)
        }
        if (!call(kHello, w.b, &body, 30'000)) return fail(why(std::format("blit's agent on {} did not answer", host)));
        Reader r(body.data(), body.size());
        r.str();
        {
            std::lock_guard lock(mu);
            home = r.str();
            system = r.str();
            progress.clear();
            up = true;
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
            } else if (op == kOutput || op == kExit) {
                Reader r(rest.data(), rest.size());
                const u32 proc = r.u32();
                std::lock_guard lock(mu);
                auto it = procs.find(proc);
                if (it == procs.end()) continue;
                Proc& p = *it->second;
                if (op == kOutput) {
                    const int stream = r.u8();
                    p.queue.push_back({stream, r.str()});
                } else {
                    p.exited = true;
                    p.status = static_cast<int>(r.u32());
                    p.queue.push_back({0, {}});
                }
                cv.notify_all();
            }
        }
        fail(std::format("the connection to {} was lost", self->host_));
    }

    // A request, and its answer (false if there is none: no connection, or it went).
    bool call(u8 op, const std::string& payload, std::string* answer, int ms = 60'000) {
        Slot slot;
        u32 id;
        {
            std::lock_guard lock(mu);
            if (state != kReady) return false;
            id = next_id++;
            pending[id] = &slot;
        }
        bool sent;
        {
            const std::string f = frame(op, id, payload);
            std::lock_guard lock(write_mu);
            sent = write_all(to, f.data(), f.size());
        }
        std::unique_lock lock(mu);
        if (sent) cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return slot.done; });
        pending.erase(id);
        if (!slot.done || slot.failed) return false;
        *answer = std::move(slot.body);
        return true;
    }

    void send(u8 op, const std::string& payload) {
        {
            std::lock_guard lock(mu);
            if (state != kReady) return;
        }
        const std::string f = frame(op, 0, payload);
        std::lock_guard lock(write_mu);
        write_all(to, f.data(), f.size());
    }
};

Remote::Remote(std::string host) : host_(std::move(host)), impl_(std::make_unique<Impl>()) { impl_->self = this; }

Remote::~Remote() {
    Impl& m = *impl_;
    if (m.to >= 0) ::close(m.to);  // the agent sees its input end, and ends (and its programs with it)
    if (m.ssh > 0) {
        for (int i = 0; i < 50 && ::waitpid(m.ssh, nullptr, WNOHANG) == 0; ++i) ::usleep(10'000);
        if (::waitpid(m.ssh, nullptr, WNOHANG) == 0) {
            ::kill(m.ssh, SIGTERM);
            ::waitpid(m.ssh, nullptr, 0);
        }
    }
    if (m.connector.joinable()) m.connector.join();
    if (m.reader.joinable()) m.reader.join();
    if (m.err_reader.joinable()) m.err_reader.join();
    m.fail("closed");
    for (auto& [id, p] : m.procs) {
        if (p->pump_out.joinable()) p->pump_out.join();
        p->pump_in.request_stop();
        if (p->pump_in.joinable()) p->pump_in.join();
        if (p->in_r >= 0) ::close(p->in_r);
    }
    if (m.from >= 0) ::close(m.from);
    if (m.errs >= 0) ::close(m.errs);
}

std::shared_ptr<Remote> Remote::get(const std::string& host) {
    // A write to a connection that has just gone must fail, not end blit.
    static const bool quiet = (std::signal(SIGPIPE, SIG_IGN), true);
    (void)quiet;
    std::lock_guard lock(registry_mu);
    if (auto it = registry.find(host); it != registry.end()) {
        // One that failed is made again, when asked for a while after (not
        // over and over while the host cannot be reached).
        constexpr u64 kAgainNs = 3'000'000'000ull;
        Impl& m = *it->second->impl_;
        std::unique_lock lock_m(m.mu);
        if (m.state != kFailed || now_ns() - m.failed_ns < kAgainNs) return it->second;
    }
    std::shared_ptr<Remote> r(new Remote(host));
    registry[host] = r;
    Remote* raw = r.get();
    raw->impl_->connector = std::jthread([raw] { raw->impl_->connect(); });
    return r;
}

void Remote::set_notify(std::function<void()> fn) { notify_fn = std::move(fn); }

void Remote::set_askpass(std::string program) {
    std::lock_guard lock(registry_mu);
    askpass_program = std::move(program);
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
    {
        std::lock_guard lock(registry_mu);
        auto it = registry.find(host);
        if (it == registry.end()) return;
        std::lock_guard lock_m(it->second->impl_->mu);
        if (it->second->impl_->state != kFailed) return;
        it->second->impl_->failed_ns = 0;  // (long ago: get() makes it again)
    }
    get(host);
}

std::vector<std::shared_ptr<Remote>> Remote::all() {
    std::lock_guard lock(registry_mu);
    std::vector<std::shared_ptr<Remote>> out;
    for (auto& [host, r] : registry) out.push_back(r);
    return out;
}

void Remote::close_all() {
    std::map<std::string, std::shared_ptr<Remote>> gone;
    {
        std::lock_guard lock(registry_mu);
        gone.swap(registry);
    }
    for (auto& [host, r] : gone) r->impl_->fail("closed");  // (what waits for one is woken, and fails)
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
std::string Remote::home() const {
    std::lock_guard lock(impl_->mu);
    return impl_->home;
}
std::string Remote::system() const {
    std::lock_guard lock(impl_->mu);
    return impl_->system;
}

bool Remote::wait_ready(int ms) {
    std::unique_lock lock(impl_->mu);
    impl_->cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return impl_->state == kFailed || impl_->up; });
    return impl_->up && impl_->state == kReady;
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

bool Remote::write(const std::string& path, std::string_view data, std::string* err, i64* modified_ns) {
    Writer w;
    w.str(path).str(data.data(), data.size());
    std::string a;
    if (!impl_->call(kWrite, w.b, &a, 300'000)) {
        if (err) *err = std::format("no connection to {}", host_);
        return false;
    }
    Reader r(a.data(), a.size());
    const bool ok = r.u8() != 0;
    const std::string e = r.str();
    const i64 m = r.i64();
    if (!ok && err) *err = e;
    if (modified_ns) *modified_ns = m;
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
    for (const int fd : {in[0], in[1], o[0], o[1], e[0], e[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    auto p = std::make_unique<Impl::Proc>();
    p->in_r = in[0], p->out_w = o[1], p->err_w = e[1];
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
        for (const int fd : {in[0], in[1], o[0], o[1], e[0], e[1]}) ::close(fd);
        if (err) *err = why;
        return false;
    }
    Impl::Proc* proc;
    {
        std::lock_guard lock(m.mu);
        proc = m.procs[id].get();
    }
    // What is written to its input goes there; what it writes comes back into its pipes.
    proc->pump_in = std::jthread([&m, id, fd = in[0]](std::stop_token stop) {
        char buf[65536];
        while (!stop.stop_requested()) {  // (looked at now and then: a read that waits is not woken by closing its pipe)
            pollfd ready{fd, POLLIN, 0};
            if (::poll(&ready, 1, 100) <= 0) continue;
            const ssize_t k = ::read(fd, buf, sizeof buf);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            Writer chunk;
            chunk.u32(id).str(buf, static_cast<size_t>(k));
            m.send(kStdin, chunk.b);
        }
        Writer done;
        done.u32(id).str("", 0);  // (its input is closed)
        m.send(kStdin, done.b);
    });
    proc->pump_out = std::jthread([&m, proc] {
        for (;;) {
            std::pair<int, std::string> item;
            {
                std::unique_lock lock(m.mu);
                m.cv.wait(lock, [&] { return !proc->queue.empty(); });
                item = std::move(proc->queue.front());
                proc->queue.pop_front();
            }
            if (item.first == 0) break;
            write_all(item.first == 1 ? proc->out_w : proc->err_w, item.second.data(), item.second.size());
        }
        ::close(proc->out_w);
        ::close(proc->err_w);
        std::lock_guard lock(m.mu);
        proc->ended = true;
        m.cv.notify_all();
    });
    out->id = id;
    out->in = in[1];
    out->out = o[0];
    out->err = e[0];
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

void Remote::resize(u32 proc, unsigned rows, unsigned cols) {
    Writer w;
    w.u32(proc).u32(rows).u32(cols);
    impl_->send(kResize, w.b);
}

void Remote::kill(u32 proc, int sig) {
    Writer w;
    w.u32(proc).u32(static_cast<u32>(sig));
    impl_->send(kKill, w.b);
}

}  // namespace blit
