#include "forward.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "pump.hpp"
#include "remote.hpp"

namespace blit {

namespace {

constexpr size_t kConnsMost = 32;  // connections joined at once, at most

// A socket listening on the loopback at `port` (0: any), v4 or v6; -1 with errno.
int listen_on(bool v6, u16 port) {
    const int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int ok;
    if (v6) {
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_loopback;
        a.sin6_port = htons(port);
        ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (ok != 0 || ::listen(fd, 16) != 0) {
        const int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

u16 port_of(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

}  // namespace

std::shared_ptr<Forward> Forward::open(const std::string& host, const std::string& target, u16 local, std::string* err) {
    std::shared_ptr<Forward> f(new Forward());
    f->host_ = host;
    f->target_ = target;
    f->fds_[0] = listen_on(false, local);
    if (f->fds_[0] < 0) {
        *err = errno == EADDRINUSE ? "port " + std::to_string(local) + " is in use here" : std::string("cannot listen here: ") + std::strerror(errno);
        return nullptr;
    }
    f->port_ = port_of(f->fds_[0]);
    f->fds_[1] = listen_on(true, f->port_);  // (and on ::1 the same port, where there is one: "localhost" may be either)
    Forward* raw = f.get();
    f->listener_ = std::jthread([raw](std::stop_token stop) { raw->listen_loop(stop); });
    return f;
}

Forward::~Forward() {
    listener_.request_stop();
    if (listener_.joinable()) listener_.join();
    conns_.clear();  // (each asked to stop, and joined: its pump there ended)
    for (const int fd : fds_)
        if (fd >= 0) ::close(fd);
}

std::string Forward::address() const { return "localhost:" + std::to_string(port_); }

std::vector<std::string> Forward::take_errors() {
    std::lock_guard lock(mu_);
    return std::exchange(errors_, {});
}

void Forward::error(const std::string& why) {
    {
        std::lock_guard lock(mu_);
        errors_.push_back(why);
    }
    if (told) told();
}

void Forward::listen_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        pollfd p[2] = {{fds_[0], POLLIN, 0}, {fds_[1], POLLIN, 0}};
        const int r = ::poll(p, 2, 200);
        conns_.remove_if([](const Conn& c) { return c.done.load(); });  // (those that have ended, let go of)
        if (r <= 0) continue;
        for (const pollfd& l : p) {
            if (!(l.revents & POLLIN)) continue;
            const int s = ::accept(l.fd, nullptr, nullptr);
            if (s < 0) continue;
            if (conns_.size() >= kConnsMost) {  // (a port there that is this one, reached through itself, would make them without end)
                ::close(s);
                if (!refused_) error(address() + ": more than " + std::to_string(kConnsMost) + " connections at once; " + target_ + " on " + host_ + " may be this port itself");
                refused_ = true;
                continue;
            }
            refused_ = false;
            ::fcntl(s, F_SETFD, FD_CLOEXEC);
            ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL) & ~O_NONBLOCK);
            const int one = 1;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            Conn& c = conns_.emplace_back();
            c.thread = std::jthread([this, s, &c](std::stop_token st) {
                serve(s, st);
                c.done = true;
            });
        }
    }
}

// One connection: the pump started there, the two joined until either ends.
void Forward::serve(int sock, std::stop_token stop) {
    const std::string root = remote_prefix(host_) + "/";
    std::shared_ptr<Remote> c;
    std::string there;
    for (int waited = 0; !c && waited < kRemoteWaitMs && !stop.stop_requested(); waited += 250) c = remote_for(root, &there, 250);
    if (!c) {
        if (!stop.stop_requested()) error(host_ + ": " + remote_error(root));
        ::close(sock);
        return;
    }
    if (c->agent_level() < 6) {  // (blit brings its own agent: an older one is met only when the agents kept were built from older sources)
        error("blit's agent on " + host_ + " is too old to reach a port there");
        ::close(sock);
        return;
    }
    RemoteProc p;
    std::string err;
    if (!c->spawn({c->agent_path(), "--connect", target_}, {}, {}, &p, &err)) {
        error("Cannot start blit's agent on " + host_ + ": " + err);
        ::close(sock);
        return;
    }
    ++live_;
    PumpJoin j{sock, sock, p.out, p.in};
    pump_join(j, [&] { return stop.stop_requested(); });
    if (stop.stop_requested()) c->kill(p.id, SIGHUP);
    // Why it ended, if it said (it could not connect there).
    std::string said;
    char buf[4096];
    for (int i = 0; i < 25; ++i) {
        pollfd e{p.err, POLLIN, 0};
        if (::poll(&e, 1, stop.stop_requested() ? 0 : 20) <= 0) break;
        const ssize_t k = ::read(p.err, buf, sizeof buf);
        if (k <= 0) break;
        said.append(buf, static_cast<size_t>(k));
    }
    if (said.starts_with("connected\n")) said.erase(0, 10);  // (it was: what follows, if anything, is why it ended)
    while (!said.empty() && (said.back() == '\n' || said.back() == '\r')) said.pop_back();
    if (!said.empty()) error(host_ + ": " + said.substr(said.rfind('\n') + 1));
    ::close(sock);
    if (j.b_out >= 0) ::close(j.b_out);
    ::close(p.out);
    if (p.err >= 0) ::close(p.err);
    if (c->wait(p.id, 3000) == -2) {
        c->kill(p.id, SIGKILL);
        c->wait(p.id, 3000);
    }
    --live_;
}

LoopbackDial forward_dial(const std::string& host) {
    return [host](const LoopbackEnd& end, std::string* why) -> LoopbackConn {
        const std::string root = remote_prefix(host) + "/";
        std::string there;
        const std::shared_ptr<Remote> c = remote_for(root, &there, 1000);
        if (!c) {
            *why = remote_error(root);
            return {};
        }
        if (c->agent_level() < 6) {  // (blit brings its own agent: an older one is met only when the agents kept were built from older sources)
            *why = "blit's agent on " + host + " is too old to reach a port there";
            return {};
        }
        RemoteProc p;
        std::string err;
        if (!c->spawn({c->agent_path(), "--connect", end.text()}, {}, {}, &p, &err)) {
            *why = "cannot start blit's agent on " + host + ": " + err;
            return {};
        }
        // Its first line: "connected", or why not (it then ends).
        std::string said;
        char buf[512];
        for (const u64 until = now_ns() + 12'000'000'000ull; said.find('\n') == std::string::npos && now_ns() < until;) {
            pollfd e{p.err, POLLIN, 0};
            if (::poll(&e, 1, 100) <= 0) continue;
            const ssize_t k = ::read(p.err, buf, sizeof buf);
            if (k <= 0) break;
            said.append(buf, static_cast<size_t>(k));
        }
        const std::string first = said.substr(0, said.find('\n'));
        // (Reaped on a thread of its own once it ends: when the link closes
        // its pipes, or now, when it could not connect.)
        auto reap = [c, p](bool now) {
            std::thread([c, p, now] {
                if (now) {
                    ::close(p.in);
                    ::close(p.out);
                }
                char sink[512];
                while (::read(p.err, sink, sizeof sink) > 0) {}
                ::close(p.err);
                if (c->wait(p.id, now ? 3000 : -1) == -2) {
                    c->kill(p.id, SIGKILL);
                    c->wait(p.id, 3000);
                }
            }).detach();
        };
        if (first != "connected") {
            // (In a link's words here, loopback.cpp's: "connection refused on box".)
            std::string w = first;
            if (const std::string pre = "Cannot connect to " + end.text() + ": "; w.starts_with(pre)) w.erase(0, pre.size());
            std::string low;
            for (const char ch : w) low += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            if (first.empty()) w = "no answer";
            else if (low == "connection refused") w = "connection refused";
            else if (low == "connection timed out" || low == "operation timed out") w = "no answer";
            else if (low == "no route to host" || low == "network is unreachable") w = "unreachable";
            *why = w + " on " + host;
            c->kill(p.id, SIGTERM);
            reap(true);
            return {};
        }
        reap(false);
        return {p.out, p.in};
    };
}

bool forward_local(const std::string& root, const std::string& target, std::shared_ptr<Forward>* keep, std::string* address, std::string* err) {
    std::string host, there;
    if (!split_remote(root, &host, &there)) {
        keep->reset();
        *address = target;
        return true;
    }
    std::shared_ptr<Forward> f = Forward::open(host, target, 0, err);
    if (!f) return false;
    *address = f->address();
    *keep = std::move(f);
    return true;
}

}  // namespace blit
