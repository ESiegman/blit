#include "instance.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "remote.hpp"

namespace blit {

namespace {

void cloexec(int fd) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }

// A write to an asker that has gone must not end blit with SIGPIPE.
void no_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

bool address(const std::string& path, sockaddr_un* a) {
    std::memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof a->sun_path) return false;
    std::memcpy(a->sun_path, path.c_str(), path.size() + 1);
    return true;
}

int connect_to(const std::string& path) {
    sockaddr_un a;
    if (!address(path, &a)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    cloexec(fd);
    no_sigpipe(fd);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool write_all(int fd, std::string_view s) {
    while (!s.empty()) {
        const ssize_t n = ::write(fd, s.data(), s.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        s.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

// An ask, in lines: "blit 1", "wait" when it waits, "open LINE COL PATH"
// for each file, and "end". (A path with a line break in it is not sent.)
std::string encode(const InstanceAsk& ask) {
    std::string out = "blit 1\n";
    if (ask.wait) out += "wait\n";
    for (const OpenAsk& f : ask.files) {
        if (f.path.empty() || f.path.find('\n') != std::string::npos) continue;
        out += "open " + std::to_string(f.line) + " " + std::to_string(f.col) + " " + f.path + "\n";
    }
    out += "end\n";
    return out;
}

bool decode(std::string_view text, InstanceAsk* ask) {
    bool first = true, ended = false;
    while (!text.empty() && !ended) {
        const size_t nl = text.find('\n');
        if (nl == std::string_view::npos) return false;
        const std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl + 1);
        if (first) {
            if (line != "blit 1") return false;
            first = false;
        } else if (line == "wait") {
            ask->wait = true;
        } else if (line == "end") {
            ended = true;
        } else if (line.starts_with("open ")) {
            std::string_view rest = line.substr(5);
            auto number = [&](u64* out) {
                const size_t sp = rest.find(' ');
                if (sp == std::string_view::npos || sp == 0) return false;
                u64 v = 0;
                for (const char c : rest.substr(0, sp)) {
                    if (c < '0' || c > '9') return false;
                    v = v * 10 + static_cast<u64>(c - '0');
                }
                *out = v;
                rest.remove_prefix(sp + 1);
                return true;
            };
            OpenAsk f;
            if (!number(&f.line) || !number(&f.col) || rest.empty()) return false;
            f.path = std::string(rest);
            ask->files.push_back(std::move(f));
        }  // (anything else is from a later blit: passed over)
    }
    return ended;
}

}  // namespace

std::string instance_socket() {
    std::string dir;
#if defined(__APPLE__)
    // The user's own temporary folder, as the system has it (the same for a
    // blit started from a terminal as for one started from the Dock, which
    // TMPDIR need not be).
    char buf[PATH_MAX];
    if (::confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf) > 0) dir = buf;
#endif
    if (dir.empty())
        if (const char* t = std::getenv("TMPDIR"); t && *t) dir = t;
    if (dir.empty()) dir = "/tmp";
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    u32 h = 2166136261u;  // (FNV-1a of the program's path)
    for (const char c : program_path()) {
        h ^= static_cast<u8>(c);
        h *= 16777619u;
    }
    char name[64];
    std::snprintf(name, sizeof name, "/blit-%u-%08x.sock", static_cast<unsigned>(::getuid()), h);
    std::string path = dir + name;
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) path = std::string("/tmp") + name;
    return path;
}

int instance_hand_over(const InstanceAsk& ask, void (*taken)()) {
    const int fd = connect_to(instance_socket());
    if (fd < 0) return -1;
    if (!write_all(fd, encode(ask))) {
        ::close(fd);
        return -1;
    }
    std::string reply;
    char buf[256];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        const bool had = reply.find('\n') != std::string::npos;
        reply.append(buf, static_cast<size_t>(n));
        if (!had && reply.starts_with("ok\n") && taken) taken();
        if (reply.find('\n') != std::string::npos && !(ask.wait && reply.starts_with("ok"))) break;
        if (ask.wait && reply.find("done\n") != std::string::npos) break;
    }
    ::close(fd);
    if (reply.starts_with("no")) return -1;  // (it could not read the ask: start one of our own)
    return reply.starts_with("ok") || reply.find("done") != std::string::npos ? 0 : 1;
}

bool InstanceServer::start(std::function<void(InstanceAsk)> got) {
    path_ = instance_socket();
    sockaddr_un a;
    if (!address(path_, &a)) return false;
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    cloexec(fd_);
    const mode_t was = ::umask(077);  // (the user's own: nobody else may hand files over)
    bool bound = ::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    if (!bound && errno == EADDRINUSE) {
        if (const int other = connect_to(path_); other >= 0) {  // another blit of this program is up
            ::close(other);
        } else {  // left by one that went without tidying up
            ::unlink(path_.c_str());
            bound = ::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
        }
    }
    ::umask(was);
    if (!bound || ::listen(fd_, 8) != 0 || ::pipe(wake_) != 0) {
        if (bound) ::unlink(path_.c_str());
        ::close(fd_);
        fd_ = -1;
        path_.clear();
        return false;
    }
    cloexec(wake_[0]);
    cloexec(wake_[1]);
    thread_ = std::jthread([this, got = std::move(got)] {
        for (;;) {
            pollfd p[2] = {{fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
            if (::poll(p, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (p[1].revents) return;
            if (!(p[0].revents & POLLIN)) continue;
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            cloexec(c);
            no_sigpipe(c);
            // The ask, read whole (an asker gets two seconds and a megabyte).
            std::string in;
            char buf[4096];
            const u64 until = now_ns() + 2'000'000'000ull;
            while (in.size() < (1u << 20) && in.find("\nend\n") == std::string::npos) {
                const u64 t = now_ns();
                if (t >= until) break;
                pollfd q{c, POLLIN, 0};
                if (::poll(&q, 1, static_cast<int>((until - t) / 1'000'000) + 1) <= 0) break;
                const ssize_t n = ::read(c, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                in.append(buf, static_cast<size_t>(n));
            }
            InstanceAsk ask;
            if (!decode(in, &ask)) {
                answer(c, "no\n");
                continue;
            }
            if (ask.wait) {
                ask.fd = c;  // (answered when its tabs have closed)
                write_all(c, "ok\n");
            } else {
                answer(c, "ok\n");
            }
            got(std::move(ask));
        }
    });
    return true;
}

void InstanceServer::stop() {
    if (fd_ < 0) return;
    if (wake_[1] >= 0) write_all(wake_[1], "x");
    if (thread_.joinable()) thread_.join();
    ::close(fd_);
    fd_ = -1;
    for (int& w : wake_) {
        if (w >= 0) ::close(w);
        w = -1;
    }
    ::unlink(path_.c_str());
    path_.clear();
}

void InstanceServer::answer(int fd, const char* what) {
    if (fd < 0) return;
    write_all(fd, what);
    ::close(fd);
}

}  // namespace blit
