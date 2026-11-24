#include "loopback.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace blit {

namespace {

void nonblocking(int fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

void no_sigpipe([[maybe_unused]] int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A write that never raises SIGPIPE (a socket's send; a pipe's write, which
// the process ignores SIGPIPE for already: main.cpp).
ssize_t put(int fd, const char* p, size_t n) {
    const ssize_t k = ::send(fd, p, n, kSendFlags);
    if (k < 0 && errno == ENOTSOCK) return ::write(fd, p, n);
    return k;
}

std::string error_text(int e) {
    if (e == ECONNREFUSED) return "connection refused";
    if (e == ETIMEDOUT) return "no answer";
    if (e == EHOSTUNREACH || e == ENETUNREACH) return "unreachable";
    return std::strerror(e);
}

void close_conn(LoopbackConn& c) {
    if (c.out >= 0 && c.out != c.in) ::close(c.out);
    if (c.in >= 0) ::close(c.in);
    c = {};
}

}  // namespace

std::string LoopbackEnd::text() const { return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port); }

LoopbackConn loopback_dial_tcp(const LoopbackEnd& end, std::string* why, int ms) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* found = nullptr;
    const std::string service = std::to_string(end.port);
    if (const int r = ::getaddrinfo(end.host.c_str(), service.c_str(), &hints, &found); r != 0) {
        *why = std::string("cannot find ") + end.host + ": " + ::gai_strerror(r);
        return {};
    }
    LoopbackConn out;
    for (addrinfo* a = found; a && out.in < 0; a = a->ai_next) {
        const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            *why = error_text(errno);
            continue;
        }
        nonblocking(fd);
        no_sigpipe(fd);
        int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            r = ::poll(&p, 1, ms);
            if (r == 0) {
                errno = ETIMEDOUT;
                r = -1;
            } else if (r > 0) {
                int err = 0;
                socklen_t len = sizeof err;
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                errno = err;
                r = err ? -1 : 0;
            }
        }
        if (r != 0) {
            *why = error_text(errno);
            ::close(fd);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        out.in = fd;
    }
    ::freeaddrinfo(found);
    if (out.in >= 0) why->clear();
    return out;
}

int loopback_listen(u16 port, u16* bound, std::string* why) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *why = std::strerror(errno);
        return -1;
    }
    nonblocking(fd);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 4) != 0) {
        *why = errno == EADDRINUSE ? "port " + std::to_string(port) + " is in use" : std::string(std::strerror(errno));
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    if (bound) *bound = ntohs(a.sin_port);
    return fd;
}

u16 loopback_free_port() {
    u16 port = 0;
    std::string why;
    const int fd = loopback_listen(0, &port, &why);
    if (fd < 0) return 0;
    ::close(fd);
    return port;
}

// ---------------------------------------------------------------- the link

Loopback::Loopback(Options o, Events e) : o_(std::move(o)), e_(std::move(e)) {
    if (!o_.dial) o_.dial = [](const LoopbackEnd& end, std::string* why) { return loopback_dial_tcp(end, why); };
    if (o_.mode == Mode::kConnect) port_ = o_.end.port;
    int p[2];
    if (::pipe(p) == 0) {
        wake_r_ = p[0], wake_w_ = p[1];
        nonblocking(wake_r_);
        nonblocking(wake_w_);
    }
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

Loopback::~Loopback() {
    thread_.request_stop();
    poke();
    if (thread_.joinable()) thread_.join();
    close_conn(conn_);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (wake_r_ >= 0) ::close(wake_r_);
    if (wake_w_ >= 0) ::close(wake_w_);
}

void Loopback::poke() {
    if (wake_w_ < 0) return;
    const char c = 1;
    [[maybe_unused]] const ssize_t k = ::write(wake_w_, &c, 1);
}

std::string Loopback::why() const {
    std::lock_guard lock(mu_);
    return why_;
}

void Loopback::retry_now() {
    again_ = true;
    poke();
}

bool Loopback::send(std::string_view bytes) {
    std::lock_guard lock(mu_);
    if (!open_) return false;
    const int fd = conn_.out >= 0 ? conn_.out : conn_.in;
    if (pending_.empty()) {
        while (!bytes.empty()) {
            const ssize_t k = put(fd, bytes.data(), bytes.size());
            if (k > 0) {
                bytes.remove_prefix(static_cast<size_t>(k));
            } else if (k < 0 && errno == EINTR) {
                continue;
            } else {
                break;  // (EAGAIN: the rest waits; a failure: the reader finds it)
            }
        }
    }
    if (!bytes.empty()) {
        pending_.append(bytes);
        poke();
    }
    return true;
}

bool Loopback::flush() {
    std::lock_guard lock(mu_);
    const int fd = conn_.out >= 0 ? conn_.out : conn_.in;
    while (!pending_.empty()) {
        const ssize_t k = put(fd, pending_.data(), pending_.size());
        if (k > 0) pending_.erase(0, static_cast<size_t>(k));
        else if (k < 0 && errno == EINTR) continue;
        else return k < 0 && errno == EAGAIN;
    }
    return true;
}

void Loopback::drop(const std::string& why) {
    {
        std::lock_guard lock(mu_);
        open_ = false;
        close_conn(conn_);
        pending_.clear();
        why_ = why;
    }
    if (e_.closed) e_.closed(why);
}

void Loopback::run(std::stop_token stop) {
    char buf[65536];
    std::string said;  // (the reason last told while down: told again only when it changes)
    u64 down_since = now_ns();
    auto wait = [&](int ms) {  // (until a retry is due, it is asked for, or the link stops)
        pollfd w{wake_r_, POLLIN, 0};
        ::poll(&w, 1, ms);
        while (::read(wake_r_, buf, sizeof buf) > 0) {
        }
    };
    auto down = [&](const std::string& why) {
        {
            std::lock_guard lock(mu_);
            why_ = why;
        }
        if (why != said) {
            said = why;
            if (e_.closed) e_.closed(why);
        }
    };
    while (!stop.stop_requested()) {
        if (!open_) {
            const int retry = now_ns() - down_since < 10'000'000'000ull ? o_.retry_ms : 1000;
            LoopbackConn c;
            std::string why;
            if (o_.mode == Mode::kListen) {
                if (listen_fd_ < 0) {
                    u16 bound = 0;
                    listen_fd_ = loopback_listen(o_.end.port, &bound, &why);
                    if (listen_fd_ < 0) {
                        down(why);
                        wait(retry);
                        continue;
                    }
                    port_ = bound;
                    down("waiting for a connection on " + std::to_string(bound));
                }
                pollfd p[2] = {{listen_fd_, POLLIN, 0}, {wake_r_, POLLIN, 0}};
                ::poll(p, 2, 1000);
                if (p[1].revents) wait(0);
                if (!(p[0].revents & POLLIN)) continue;
                const int fd = ::accept(listen_fd_, nullptr, nullptr);
                if (fd < 0) continue;
                nonblocking(fd);
                no_sigpipe(fd);
                int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                c.in = fd;
            } else {
                again_ = false;
                c = o_.dial(o_.end, &why);
                if (c.in < 0) {
                    down(why.empty() ? std::string("no connection") : why);
                    if (!again_) wait(retry);
                    continue;
                }
                nonblocking(c.in);
                if (c.out >= 0) nonblocking(c.out);
            }
            {
                std::lock_guard lock(mu_);
                conn_ = c;
                pending_.clear();
                why_.clear();
                open_ = true;
            }
            ++opens_;
            said.clear();
            if (!o_.greeting.empty()) send(o_.greeting);
            if (e_.opened) e_.opened();
            continue;
        }
        bool backed;
        int in, out;
        {
            std::lock_guard lock(mu_);
            backed = !pending_.empty();
            in = conn_.in;
            out = conn_.out >= 0 ? conn_.out : conn_.in;
        }
        pollfd p[4] = {{in, POLLIN, 0}, {wake_r_, POLLIN, 0}, {out != in ? out : -1, 0, 0}, {listen_fd_, POLLIN, 0}};
        if (backed) (out != in ? p[2] : p[0]).events |= POLLOUT;
        const int r = ::poll(p, 4, 1000);
        if (r < 0 && errno != EINTR) {
            drop(std::strerror(errno));
            down_since = now_ns();
            continue;
        }
        if (r <= 0) continue;
        if (p[1].revents) wait(0);
        if (p[3].revents & POLLIN) {  // (one link at a time: another that connects meanwhile is closed)
            if (const int extra = ::accept(listen_fd_, nullptr, nullptr); extra >= 0) ::close(extra);
        }
        if ((p[0].revents | p[2].revents) & POLLOUT) {
            if (!flush()) {
                drop("closed by the other end");
                down_since = now_ns();
                continue;
            }
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t k = ::read(in, buf, sizeof buf);
            if (k > 0) {
                if (e_.data) e_.data(buf, static_cast<size_t>(k));
            } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                drop(k == 0 || errno == ECONNRESET ? std::string("closed by the other end") : std::string(std::strerror(errno)));
                down_since = now_ns();
                said = why();
            }
        }
    }
}

}  // namespace blit
