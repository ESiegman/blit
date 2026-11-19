#include "dap.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>

#include "remote.hpp"

extern char** environ;

namespace blit {

namespace {

inline int spawn_chdir(posix_spawn_file_actions_t* fa, const char* dir) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
    return posix_spawn_file_actions_addchdir(fa, dir);
#else
    return posix_spawn_file_actions_addchdir_np(fa, dir);
#endif
}

}  // namespace

// The adapter's end of things, shared by the Dap (on the main thread), the
// thread that reads it, and the one that lets go of it: the Dap can go at
// once while the other two finish with it.
struct Dap::Link {
    std::function<void()> notify;
    // The adapter: here, a process and its pipes; there, a program the
    // agent runs (its connection and its number: mu, set by the reader once
    // it has started).
    pid_t pid = -1;
    std::shared_ptr<Remote> conn;
    RemoteProc proc;
    int out = -1, err = -1;  // (the reader's)
    // On another machine: where, and what is run there (by the reader).
    std::string remote_dir;
    std::vector<std::string> argv;
    // Writing, which never waits: what the adapter has not taken yet (and,
    // until it has started there, all that is sent) waits in `pending`, and
    // the reader writes it as the adapter takes it (a byte to `wake_w` has
    // it look). One that stops taking it holds up nothing here.
    std::mutex write_mu;
    int in = -1;
    bool up = false, in_gone = false;
    std::string pending;
    int wake_r = -1, wake_w = -1;
    // What it has said, for poll().
    std::mutex mu;
    std::deque<Message> queue;
    std::string errors;
    std::atomic<bool> ended{false};
    std::jthread reader;  // (last: it is stopped and joined by end(), never by the Dap)

    bool start_remote(std::stop_token stop);
    void read_loop(std::stop_token stop);
    void send(const std::string& frame);
    void write_pending();
    void kill_now(int sig);
    void end(int grace_ms);
};

Dap::~Dap() { stop(); }

bool Dap::start(const std::vector<std::string>& argv, const std::string& dir, std::function<void()> notify, std::string* err) {
    auto l = std::make_shared<Link>();
    l->notify = std::move(notify);
    int wake[2];
    if (::pipe(wake)) return *err = "cannot make a pipe", false;
    for (const int fd : wake) ::fcntl(fd, F_SETFD, FD_CLOEXEC), ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    l->wake_r = wake[0], l->wake_w = wake[1];
    std::string host, there;
    if (split_remote(dir, &host, &there)) {
        l->remote_dir = dir;  // (connected to, and started there, on the reader's thread)
        l->argv = argv;
    } else {
        int in[2], out[2], er[2];
        if (::pipe(in) || ::pipe(out) || ::pipe(er)) {
            ::close(wake[0]), ::close(wake[1]);
            return *err = "cannot make a pipe", false;
        }
        for (const int fd : {in[1], out[0], er[0]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_adddup2(&fa, er[1], 2);
        if (!dir.empty()) spawn_chdir(&fa, dir.c_str());
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__)
        posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif
        std::vector<std::string> words = argv;
        std::vector<char*> args;
        for (std::string& w : words) args.push_back(w.data());
        args.push_back(nullptr);
        const int rc = ::posix_spawnp(&l->pid, args[0], &fa, &attr, args.data(), environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&fa);
        ::close(in[0]), ::close(out[1]), ::close(er[1]);
        if (rc != 0) {
            ::close(in[1]), ::close(out[0]), ::close(er[0]);
            ::close(wake[0]), ::close(wake[1]);
            l->pid = -1;
            return *err = std::string("cannot run ") + argv[0] + ": " + std::strerror(rc), false;
        }
        ::fcntl(in[1], F_SETFL, ::fcntl(in[1], F_GETFL) | O_NONBLOCK);  // (what it does not take at once waits: send)
        l->in = in[1], l->out = out[0], l->err = er[0];
        l->up = true;
    }
    ::signal(SIGPIPE, SIG_IGN);
    Link* raw = l.get();  // (alive until end() has joined the reader)
    l->reader = std::jthread([raw](std::stop_token stop) { raw->read_loop(stop); });
    link_ = std::move(l);
    return true;
}

// On another machine: the connection (waited for, as a worker may, but not
// once it has failed, nor past a request to stop) and the adapter started
// there; then what was asked of it meanwhile is sent.
bool Dap::Link::start_remote(std::stop_token stop) {
    std::string host, there;
    split_remote(remote_dir, &host, &there);
    std::shared_ptr<Remote> c = Remote::get(host);  // (made now if it is not there, or again if it failed a while ago)
    bool ready = false;
    for (int waited = 0; !ready && waited < kRemoteWaitMs && !stop.stop_requested() && c->state() != Remote::kFailed; waited += 200) ready = c->wait_ready(200);
    auto failed = [&](const std::string& why) {
        std::lock_guard lock(mu);
        errors += why + "\n";
        return false;
    };
    if (stop.stop_requested()) return false;
    if (!ready) return failed(remote_error(remote_dir));
    RemoteProc p;
    std::string why;
    if (!c->spawn(argv, there, {}, &p, &why)) return failed("cannot start the debugger there: " + why);
    {
        std::lock_guard lock(mu);
        conn = std::move(c);
        proc = p;
    }
    out = p.out, err = p.err;
    ::fcntl(p.in, F_SETFL, ::fcntl(p.in, F_GETFL) | O_NONBLOCK);
    std::lock_guard lock(write_mu);
    in = p.in;
    up = true;  // (what was sent meanwhile is written by the reader's loop)
    return true;
}

// As much of what waits as the adapter takes now (write_mu held). One that
// has gone takes nothing more: what waits is dropped.
void Dap::Link::write_pending() {
    size_t at = 0;
    while (at < pending.size() && in >= 0 && !in_gone) {
        const ssize_t k = ::write(in, pending.data() + at, pending.size() - at);
        if (k > 0) at += static_cast<size_t>(k);
        else if (k < 0 && errno == EINTR) continue;
        else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else in_gone = true;
    }
    if (in_gone) pending.clear();
    else pending.erase(0, at);
}

// Messages, as they come: a header ("Content-Length: n"), a blank line, n
// bytes of JSON. (And what it writes as errors, the last of it kept.)
void Dap::Link::read_loop(std::stop_token stop) {
    if (!remote_dir.empty() && !start_remote(stop)) {
        ended = true;
        if (notify) notify();
        return;
    }
    std::string in_text;
    char buf[65536];
    // (Its output, its errors, the wake, and its input while something waits
    // to be written: a descriptor of -1 is not looked at.)
    pollfd fds[4] = {{out, POLLIN, 0}, {err, POLLIN, 0}, {wake_r, POLLIN, 0}, {-1, POLLOUT, 0}};
    while (!stop.stop_requested()) {
        {
            std::lock_guard lock(write_mu);
            fds[3].fd = pending.empty() || in_gone ? -1 : in;
        }
        const int r = ::poll(fds, 4, 100);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (fds[2].revents)
            while (::read(wake_r, buf, sizeof buf) > 0) {
            }
        if (fds[3].fd >= 0 && fds[3].revents) {
            std::lock_guard lock(write_mu);
            write_pending();
        }
        if (fds[1].fd >= 0 && fds[1].revents) {
            const ssize_t k = ::read(err, buf, sizeof buf);
            if (k <= 0) {
                fds[1].fd = -1;  // (its errors have ended: its output may not have)
            } else {
                std::lock_guard lock(mu);
                errors.append(buf, static_cast<size_t>(k));
                if (errors.size() > 4096) errors.erase(0, errors.size() - 4096);
            }
        }
        if (!fds[0].revents) continue;
        const ssize_t k = ::read(out, buf, sizeof buf);
        if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) break;
        if (k < 0) continue;
        in_text.append(buf, static_cast<size_t>(k));
        bool any = false;
        // Text that is not a message is the program's own (gdb lets the
        // program write where its messages go): it is told as output.
        auto program_wrote = [&](std::string text) {
            std::string json = "{\"type\":\"event\",\"event\":\"output\",\"body\":{\"category\":\"stdout\",\"output\":";
            json::quote(json, text);
            json += "}}";
            auto doc = std::make_shared<json::Doc>();
            if (!doc->parse(std::move(json))) return;
            Message m;
            m.type = "event";
            m.name = "output";
            m.doc = std::move(doc);
            std::lock_guard lock(mu);
            queue.push_back(std::move(m));
            any = true;
        };
        for (;;) {
            const size_t at = in_text.find("Content-Length:");
            if (at == std::string::npos) {  // (no header yet: whole lines are the program's; what follows the last may be the start of one)
                if (in_text.find('\n') != std::string::npos) {
                    const size_t cut = in_text.rfind('\n') + 1;
                    program_wrote(in_text.substr(0, cut));
                    in_text.erase(0, cut);
                }
                break;
            }
            if (at > 0) {
                program_wrote(in_text.substr(0, at));
                in_text.erase(0, at);
                continue;
            }
            const size_t head_end = in_text.find("\r\n\r\n");
            if (head_end == std::string::npos) break;
            size_t length = 0;
            size_t num = at + 15;
            while (num < head_end && in_text[num] == ' ') ++num;
            std::from_chars(in_text.data() + num, in_text.data() + head_end, length);
            if (in_text.size() < head_end + 4 + length) break;
            std::string text = in_text.substr(head_end + 4, length);
            in_text.erase(0, head_end + 4 + length);
            auto doc = std::make_shared<json::Doc>();
            if (!doc->parse(std::move(text))) continue;
            const json::Value v = doc->root();
            Message m;
            m.type = std::string(v["type"].str());
            m.name = std::string(m.type == "event" ? v["event"].str() : v["command"].str());
            m.seq = v["seq"].integer();
            m.request_seq = v["request_seq"].integer();
            m.success = v["success"].boolean(true);
            m.message = std::string(v["message"].str());
            m.doc = std::move(doc);
            std::lock_guard lock(mu);
            queue.push_back(std::move(m));
            any = true;
        }
        if (any && notify) notify();
    }
    ended = true;
    if (notify) notify();
}

// (On the main thread: what the adapter does not take at once, the reader writes when it does.)
void Dap::Link::send(const std::string& frame) {
    std::lock_guard lock(write_mu);
    if (!up) {  // (not started there yet: sent when it has, unless it never will be)
        if (!ended) pending += frame;
        return;
    }
    if (in_gone) return;
    const bool waiting = !pending.empty();
    pending += frame;
    if (waiting) return;  // (the reader is writing what waits already: after it)
    write_pending();
    if (!pending.empty()) {
        const char byte = 1;
        [[maybe_unused]] const ssize_t n = ::write(wake_w, &byte, 1);
    }
}

// A signal to the adapter (a call that does not wait).
void Dap::Link::kill_now(int sig) {
    pid_t p;
    std::shared_ptr<Remote> c;
    u32 id;
    {
        std::lock_guard lock(mu);
        p = pid, c = conn, id = proc.id;
    }
    if (p > 0) ::kill(p, sig);
    if (c && id) c->kill(id, sig);
}

// Letting go of the adapter (on a thread of its own: each step may wait).
void Dap::Link::end(int grace_ms) {
    for (int waited = 0; waited < grace_ms && !ended; waited += 10) ::usleep(10'000);
    const bool alive = !ended;
    reader.request_stop();  // (one still connecting stops; one starting the adapter there finishes that first)
    if (reader.joinable()) reader.join();
    if (alive) kill_now(SIGTERM);
    {
        std::lock_guard lock(write_mu);
        if (in >= 0) ::close(in);
        in = -1;
        up = false;
    }
    if (out >= 0) ::close(out);
    if (err >= 0) ::close(err);
    ::close(wake_r), ::close(wake_w);
    if (pid > 0) {
        int st = 0;
        for (int i = 0; i < 50 && ::waitpid(pid, &st, WNOHANG) == 0; ++i) ::usleep(10'000);
        if (::waitpid(pid, &st, WNOHANG) == 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, 0);
        }
    }
    // The program there is reaped: the connection keeps a record and descriptors for it until it is.
    if (conn && proc.id && conn->wait(proc.id, 1000) == -2) {
        conn->kill(proc.id, SIGKILL);
        conn->wait(proc.id, 3000);
    }
}

i64 Dap::request(std::string_view command, std::string_view arguments, Done done) {
    const i64 seq = seq_++;
    if (!link_) return seq;
    std::string json = "{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":";
    json::quote(json, command);
    json += ",\"arguments\":";
    json += arguments;
    json += "}";
    if (done) waiting_.emplace_back(seq, std::move(done));
    link_->send("Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json);
    return seq;
}

void Dap::respond(const Message& to, bool success, std::string_view body) {
    if (!link_) return;
    std::string json = "{\"seq\":" + std::to_string(seq_++) + ",\"type\":\"response\",\"request_seq\":" + std::to_string(to.seq) + ",\"success\":" +
                       (success ? "true" : "false") + ",\"command\":";
    json::quote(json, to.name);
    json += ",\"body\":";
    json += body;
    json += "}";
    link_->send("Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json);
}

std::vector<Dap::Message> Dap::poll() {
    std::deque<Message> got;
    if (link_) {
        std::lock_guard lock(link_->mu);
        got.swap(link_->queue);
    }
    std::vector<Message> events;
    for (Message& m : got) {
        if (m.type == "response") {
            for (size_t i = 0; i < waiting_.size(); ++i)
                if (waiting_[i].first == m.request_seq) {
                    Done done = std::move(waiting_[i].second);
                    waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
                    done(m);
                    break;
                }
        } else {
            events.push_back(std::move(m));
        }
    }
    return events;
}

bool Dap::ended() const { return !link_ || link_->ended; }

std::string Dap::errors() const {
    if (!link_) return {};
    std::lock_guard lock(link_->mu);
    return link_->errors;
}

void Dap::stop(int grace_ms) {
    if (!link_) return;
    std::shared_ptr<Link> l = std::move(link_);
    // With no time to end of itself, it is told to at once, from here (a
    // thread just started may not run before blit itself has gone).
    if (grace_ms <= 0 && !l->ended) l->kill_now(SIGTERM);
    std::thread([l = std::move(l), grace_ms] { l->end(grace_ms); }).detach();
}

}  // namespace blit
