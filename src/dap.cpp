#include "dap.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>

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

Dap::~Dap() {
    stop();
    reader_.request_stop();
    err_reader_.request_stop();
    if (reader_.joinable()) reader_.join();
    if (err_reader_.joinable()) err_reader_.join();
    if (in_ >= 0) ::close(in_);
    if (out_ >= 0) ::close(out_);
    if (err_ >= 0) ::close(err_);
    if (pid_ > 0) {
        int st = 0;
        for (int i = 0; i < 50 && ::waitpid(pid_, &st, WNOHANG) == 0; ++i) ::usleep(10'000);
        if (::waitpid(pid_, &st, WNOHANG) == 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, &st, 0);
        }
    }
    if (conn_ && proc_.id) conn_->wait(proc_.id, 1000);
}

bool Dap::start(const std::vector<std::string>& argv, const std::string& dir, std::function<void()> notify, std::string* err) {
    notify_ = std::move(notify);
    std::string host, there;
    if (split_remote(dir, &host, &there)) {
        conn_ = remote_for(dir, &there, kRemoteWaitMs);
        if (!conn_) return *err = remote_error(dir), false;
        if (!conn_->spawn(argv, there, {}, &proc_, err)) return false;
        in_ = proc_.in, out_ = proc_.out, err_ = proc_.err;
    } else {
        int in[2], out[2], er[2];
        if (::pipe(in) || ::pipe(out) || ::pipe(er)) return *err = "cannot make a pipe", false;
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
#endif
        std::vector<std::string> words = argv;
        std::vector<char*> args;
        for (std::string& w : words) args.push_back(w.data());
        args.push_back(nullptr);
        const int rc = ::posix_spawnp(&pid_, args[0], &fa, &attr, args.data(), environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&fa);
        ::close(in[0]), ::close(out[1]), ::close(er[1]);
        if (rc != 0) {
            ::close(in[1]), ::close(out[0]), ::close(er[0]);
            return *err = std::string("cannot run ") + argv[0] + ": " + std::strerror(rc), false;
        }
        in_ = in[1], out_ = out[0], err_ = er[0];
    }
    ::signal(SIGPIPE, SIG_IGN);
    reader_ = std::jthread([this](std::stop_token stop) { read_loop(stop); });
    err_reader_ = std::jthread([this](std::stop_token stop) {
        char buf[4096];
        while (!stop.stop_requested()) {
            pollfd p{err_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            const ssize_t k = ::read(err_, buf, sizeof buf);
            if (k <= 0) break;
            std::lock_guard lock(mu_);
            errors_.append(buf, static_cast<size_t>(k));
            if (errors_.size() > 4096) errors_.erase(0, errors_.size() - 4096);
        }
    });
    return true;
}

// Messages, as they come: a header ("Content-Length: n"), a blank line, n bytes of JSON.
void Dap::read_loop(std::stop_token stop) {
    std::string in;
    char buf[65536];
    while (!stop.stop_requested()) {
        pollfd p{out_, POLLIN, 0};
        const int r = ::poll(&p, 1, 100);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        const ssize_t k = ::read(out_, buf, sizeof buf);
        if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) break;
        if (k < 0) continue;
        in.append(buf, static_cast<size_t>(k));
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
            std::lock_guard lock(mu_);
            queue_.push_back(std::move(m));
            any = true;
        };
        for (;;) {
            const size_t at = in.find("Content-Length:");
            if (at == std::string::npos) {  // (no header yet: what can not be the start of one is the program's)
                const size_t keep = std::min<size_t>(in.size(), 15);
                if (in.size() > keep && in.find('\n') != std::string::npos) {
                    const size_t cut = in.rfind('\n') + 1;
                    program_wrote(in.substr(0, cut));
                    in.erase(0, cut);
                }
                break;
            }
            if (at > 0) {
                program_wrote(in.substr(0, at));
                in.erase(0, at);
                continue;
            }
            const size_t head_end = in.find("\r\n\r\n");
            if (head_end == std::string::npos) break;
            size_t length = 0;
            size_t num = at + 15;
            while (num < head_end && in[num] == ' ') ++num;
            std::from_chars(in.data() + num, in.data() + head_end, length);
            if (in.size() < head_end + 4 + length) break;
            std::string text = in.substr(head_end + 4, length);
            in.erase(0, head_end + 4 + length);
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
            std::lock_guard lock(mu_);
            queue_.push_back(std::move(m));
            any = true;
        }
        if (any && notify_) notify_();
    }
    ended_ = true;
    if (notify_) notify_();
}

void Dap::send(const std::string& json) {
    const std::string frame = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    std::lock_guard lock(write_mu_);
    size_t at = 0;
    while (at < frame.size() && in_ >= 0) {
        const ssize_t k = ::write(in_, frame.data() + at, frame.size() - at);
        if (k > 0) at += static_cast<size_t>(k);
        else if (k < 0 && errno == EINTR) continue;
        else break;
    }
}

i64 Dap::request(std::string_view command, std::string_view arguments, Done done) {
    const i64 seq = seq_++;
    std::string json = "{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":";
    json::quote(json, command);
    json += ",\"arguments\":";
    json += arguments;
    json += "}";
    if (done) waiting_.emplace_back(seq, std::move(done));
    send(json);
    return seq;
}

void Dap::respond(const Message& to, bool success, std::string_view body) {
    std::string json = "{\"seq\":" + std::to_string(seq_++) + ",\"type\":\"response\",\"request_seq\":" + std::to_string(to.seq) + ",\"success\":" +
                       (success ? "true" : "false") + ",\"command\":";
    json::quote(json, to.name);
    json += ",\"body\":";
    json += body;
    json += "}";
    send(json);
}

std::vector<Dap::Message> Dap::poll() {
    std::deque<Message> got;
    {
        std::lock_guard lock(mu_);
        got.swap(queue_);
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

std::string Dap::errors() const {
    std::lock_guard lock(const_cast<std::mutex&>(mu_));
    return errors_;
}

void Dap::stop() {
    if (pid_ > 0 && !ended_) ::kill(pid_, SIGTERM);
    if (conn_ && proc_.id && !ended_) conn_->kill(proc_.id, SIGTERM);
}

}  // namespace blit
