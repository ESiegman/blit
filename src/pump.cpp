#include "pump.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <vector>

namespace blit {

namespace {

constexpr size_t kBufMost = 256u << 10;  // what waits to go one way, at most, before the other side is read no more
constexpr u64 kMs = 1'000'000ull;

void put_u32(std::string& s, u32 v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u16(std::string& s, u32 v) {
    v = std::min<u32>(v, 0xFFFF);
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>(v >> 8));
}
u32 get_u32(std::string_view s, size_t at) {
    u32 v = 0;
    for (size_t i = 0; i < 4 && at + i < s.size(); ++i) v |= static_cast<u32>(static_cast<u8>(s[at + i])) << (8 * i);
    return v;
}
u32 get_u16(std::string_view s, size_t at) {
    return at + 1 < s.size() ? static_cast<u32>(static_cast<u8>(s[at])) | static_cast<u32>(static_cast<u8>(s[at + 1])) << 8 : 0;
}

std::string control(u8 op, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(kPumpEscape));
    out.push_back(static_cast<char>(op));
    out.push_back(static_cast<char>(std::min<size_t>(payload.size(), 255)));
    out.append(payload, 0, std::min<size_t>(payload.size(), 255));
    return out;
}

std::string numbered(u32 n, u32 after_ms) {
    std::string p;
    put_u32(p, n);
    put_u16(p, after_ms);
    return p;
}

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A field of a line of news: no tab or line's end in it.
std::string field(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

// One line of news, on the errors (written whole: it is small, and the
// agent reads it at once).
void say(std::string_view kind, std::initializer_list<std::string_view> fields = {}) {
    std::string line(kind);
    for (const std::string_view f : fields) line += "\t" + field(f);
    line += "\n";
    for (size_t done = 0; done < line.size();) {
        const ssize_t k = ::write(2, line.data() + done, line.size() - done);
        if (k > 0) done += static_cast<size_t>(k);
        else if (k < 0 && (errno == EINTR || would_block(errno))) {
            pollfd p{2, POLLOUT, 0};
            ::poll(&p, 1, 100);
        } else {
            return;
        }
    }
}

// The pump's input and output, set not to wait where they are pipes or
// sockets (under the agent); a terminal (the pump run by hand) is left as it is.
void no_wait(int fd) {
    struct stat st;
    if (::fstat(fd, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// What waits to go one way: written as the far side takes it.
struct Pending {
    std::string b;
    size_t at = 0;
    size_t size() const { return b.size() - at; }
    bool empty() const { return at == b.size(); }
    void add(const char* p, size_t n) {
        if (at == b.size()) b.clear(), at = 0;
        b.append(p, n);
    }
    // Written to fd as far as it takes; false when it cannot be any more.
    bool flush(int fd) {
        while (at < b.size()) {
            const ssize_t k = ::write(fd, b.data() + at, b.size() - at);
            if (k > 0) {
                at += static_cast<size_t>(k);
                continue;
            }
            if (k < 0 && errno == EINTR) continue;
            if (k < 0 && would_block(errno)) break;
            return false;
        }
        if (at == b.size()) b.clear(), at = 0;
        else if (at > (64u << 10) && at * 2 > b.size()) b.erase(0, at), at = 0;
        return true;
    }
    void clear() { b.clear(), at = 0; }
};

// ---------------------------------------------------------------- --serial

class SerialPump {
public:
    SerialPortId id;
    SerialConfig cfg;

    int run() {
        no_wait(0);
        no_wait(1);
        since_ = now_ns();
        char buf[65536];
        for (;;) {
            const u64 now = now_ns();
            if (fd_ < 0 && !released_ && now >= next_look_) look(now);
            if (fd_ >= 0) due(now);
            // What to wait for: what blit sends (unless the port has more of
            // it than it takes yet), the port, and room to write either way.
            pollfd p[3] = {{to_port_.size() < kBufMost ? 0 : -1, POLLIN, 0},
                           {to_out_.empty() ? -1 : 1, POLLOUT, 0},
                           {fd_, static_cast<short>((to_out_.size() < kBufMost ? POLLIN : 0) | (to_port_.empty() ? 0 : POLLOUT)), 0}};
            int wait = 1000;
            if (fd_ < 0 && !released_) wait = static_cast<int>(std::min<u64>(next_look_ > now ? (next_look_ - now) / kMs + 1 : 0, 1000));
            if (fd_ >= 0 && !ops_.empty()) wait = static_cast<int>(std::min<u64>(ops_.front().due > now ? (ops_.front().due - now) / kMs + 1 : 0, 1000));
            if (::poll(p, 3, wait) < 0 && errno != EINTR) return 1;
            if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                const ssize_t k = ::read(0, buf, sizeof buf);
                if (k > 0) {
                    in_.append(buf, static_cast<size_t>(k));
                    take_input();
                } else if (k == 0 || (errno != EINTR && !would_block(errno))) {
                    break;  // (blit has gone, or closed the tab)
                }
            }
            if (fd_ >= 0 && (p[2].revents & POLLIN)) {
                const ssize_t k = ::read(fd_, buf, sizeof buf);
                if (k > 0) to_out_.add(buf, static_cast<size_t>(k));
                else if (k == 0 || (errno != EINTR && !would_block(errno))) gone(k == 0 || errno == EIO || errno == ENXIO || errno == ENODEV ? "hung up" : std::strerror(errno));
            } else if (fd_ >= 0 && (p[2].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                gone("hung up");
            }
            if (fd_ >= 0 && !to_port_.empty() && !to_port_.flush(fd_)) to_port_.clear();  // (a port that cannot be written: its read says why)
            if (!to_out_.empty() && !to_out_.flush(1)) break;  // (blit has gone)
        }
        if (fd_ >= 0) serial_close(fd_);
        return 0;
    }

private:
    struct Op {
        u64 due = 0;
        u32 n = 0;
        u8 kind = 0;
        int dtr = -1, rts = -1;
        std::string text;
    };

    int fd_ = -1;
    std::string device_;  // where it is open, or was
    bool released_ = false;
    Pending to_port_, to_out_;
    std::string in_;  // what blit sent that is not yet taken apart (an escape cut in two)
    std::deque<Op> ops_;
    int dtr_ = -1, rts_ = -1;  // as last set (set again when it opens again)
    std::string waited_;
    bool said_wait_ = false;
    u64 since_ = 0, next_look_ = 0;

    std::string name() const { return base_name(device_.empty() ? id.path : device_); }

    // The port looked for, and opened if it is there.
    void look(u64 now) {
        std::string why;
        const std::string path = serial_locate(id, &why);
        if (!path.empty()) {
            int f = -1;
            std::string err;
            if (serial_open(path, cfg, &f, &err)) {
                fd_ = f;
                device_ = path;
                to_port_.clear();
                said_wait_ = false;
                say("open", {path});
                if ((dtr_ >= 0 || rts_ >= 0) && !serial_set_lines(fd_, dtr_, rts_, &err)) say("error", {"Cannot set DTR and RTS on " + name() + ": " + err});
                return;
            }
            why = err;
        }
        if (!said_wait_ || why != waited_) {
            say("wait", {why});
            waited_ = why;
            said_wait_ = true;
        }
        // Looked for again in a quarter of a second (a second, once it has been gone a while), as a tab here does.
        next_look_ = now + (now - since_ < 10'000'000'000ull ? 250 : 1000) * kMs;
    }

    // The port has gone: waited for.
    void gone(const std::string& why) {
        serial_close(fd_);
        fd_ = -1;
        to_port_.clear();
        say("gone", {why});
        // (What blit shows says it is waiting for it already: only another why is said.)
        waited_.clear();
        said_wait_ = true;
        since_ = next_look_ = now_ns();
    }

    // What blit sent: the device's bytes, and between them what is asked of the port.
    void take_input() {
        size_t i = 0;
        const size_t n = in_.size();
        while (i < n) {
            if (static_cast<u8>(in_[i]) != kPumpEscape) {
                const void* esc = std::memchr(in_.data() + i, static_cast<int>(kPumpEscape), n - i);
                const size_t j = esc ? static_cast<size_t>(static_cast<const char*>(esc) - in_.data()) : n;
                if (fd_ >= 0) to_port_.add(in_.data() + i, j - i);  // (while it is not open, what is typed goes nowhere, as here)
                i = j;
                continue;
            }
            if (i + 1 >= n) break;
            if (static_cast<u8>(in_[i + 1]) == kPumpEscape) {
                if (fd_ >= 0) to_port_.add(in_.data() + i, 1);
                i += 2;
                continue;
            }
            if (i + 2 >= n) break;
            const size_t len = static_cast<u8>(in_[i + 2]);
            if (i + 3 + len > n) break;
            asked(static_cast<u8>(in_[i + 1]), std::string_view(in_).substr(i + 3, len));
            i += 3 + len;
        }
        in_.erase(0, i);
    }

    void asked(u8 op, std::string_view p) {
        if (op == kPumpRelease) {
            if (fd_ >= 0) serial_close(fd_);
            fd_ = -1;
            to_port_.clear();
            released_ = true;
            say("released");
            return;
        }
        if (op == kPumpReclaim) {
            if (!released_) return;
            released_ = false;
            said_wait_ = false;
            since_ = next_look_ = now_ns();
            return;
        }
        if (op != kPumpConfig && op != kPumpLines && op != kPumpBreak) return;
        Op o;
        o.n = get_u32(p, 0);
        o.due = now_ns() + get_u16(p, 4) * kMs;
        o.kind = op;
        if (op == kPumpConfig) o.text = std::string(p.substr(std::min<size_t>(6, p.size())));
        if (op == kPumpLines) {
            o.dtr = p.size() > 6 ? static_cast<int8_t>(p[6]) : -1;
            o.rts = p.size() > 7 ? static_cast<int8_t>(p[7]) : -1;
        }
        // (A speed asked for while the port is not open is what it opens at.)
        if (op == kPumpConfig && fd_ < 0) {
            SerialConfig c = cfg;
            if (!serial_parse(o.text, &c)) return result(o.n, false, "Not a speed or a framing: " + o.text);
            cfg = c;
            return result(o.n, true, {});
        }
        ops_.insert(std::ranges::upper_bound(ops_, o.due, {}, &Op::due), std::move(o));
    }

    // What was asked of the port whose time has come, done (while it is open).
    void due(u64 now) {
        while (fd_ >= 0 && !ops_.empty() && ops_.front().due <= now) {
            const Op o = std::move(ops_.front());
            ops_.pop_front();
            std::string err;
            bool ok = true;
            if (o.kind == kPumpConfig) {
                SerialConfig c = cfg;
                ok = serial_parse(o.text, &c);
                if (!ok) err = "Not a speed or a framing: " + o.text;
                else if ((ok = serial_configure(fd_, c, &err))) cfg = c;
            } else if (o.kind == kPumpBreak) {
                ok = serial_break(fd_, &err);
            } else {
                ok = serial_set_lines(fd_, o.dtr, o.rts, &err);
                if (ok && o.dtr >= 0) dtr_ = o.dtr;
                if (ok && o.rts >= 0) rts_ = o.rts;
            }
            result(o.n, ok, err);
        }
    }

    static void result(u32 n, bool ok, const std::string& err) {
        if (ok) say("done", {std::to_string(n)});
        else say("fail", {std::to_string(n), err.empty() ? std::string("failed") : err});
    }
};

// ---------------------------------------------------------------- --connect

// A TCP connection to host:port, waiting no more than 10 s for it; -1 with why.
// (An address, or localhost, is taken as it is, with no lookup: the agent is
// linked statically, and a lookup through the system's resolver may not work
// where its C library is not the one it was linked with. A name is looked up.)
int connect_to(const std::string& host, const std::string& port, std::string* why) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    std::vector<addrinfo*> found;  // (each a list getaddrinfo made, tried in turn)
    int rc = 0;
    std::vector<std::string> names;
    if (host == "localhost") names = {"127.0.0.1", "::1"};
    else names.push_back(host);
    for (const std::string& h : names) {
        addrinfo* res = nullptr;
        rc = ::getaddrinfo(h.c_str(), port.c_str(), &hints, &res);
        if (rc != 0 && host != "localhost") {
            hints.ai_flags = AI_NUMERICSERV;
            rc = ::getaddrinfo(h.c_str(), port.c_str(), &hints, &res);
        }
        if (rc == 0) found.push_back(res);
    }
    if (found.empty()) {
        *why = "Cannot find " + host + ": " + ::gai_strerror(rc);
        return -1;
    }
    int fd = -1, err = 0;
    for (const addrinfo* list : found)
        for (const addrinfo* a = list; a && fd < 0; a = a->ai_next) {
            const int s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) {
                err = errno;
                continue;
            }
            ::fcntl(s, F_SETFD, FD_CLOEXEC);
            ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL) | O_NONBLOCK);
            if (::connect(s, a->ai_addr, a->ai_addrlen) == 0) {
                fd = s;
                break;
            }
            if (errno != EINPROGRESS) {
                err = errno;
                ::close(s);
                continue;
            }
            pollfd p{s, POLLOUT, 0};
            int e = ETIMEDOUT;
            socklen_t len = sizeof e;
            if (::poll(&p, 1, 10'000) > 0) {
                e = 0;
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &len);
            }
            if (e == 0) {
                fd = s;
            } else {
                err = e;
                ::close(s);
            }
        }
    for (addrinfo* list : found) ::freeaddrinfo(list);
    if (fd < 0) *why = "Cannot connect to " + host + ":" + port + ": " + std::strerror(err ? err : ECONNREFUSED);
    return fd;
}

}  // namespace

// ---------------------------------------------------------------- what goes to a serial pump

std::string pump_escape(std::string_view bytes) {
    if (!std::memchr(bytes.data(), static_cast<int>(kPumpEscape), bytes.size())) return std::string(bytes);
    std::string out;
    out.reserve(bytes.size() + 16);
    for (const char c : bytes) {
        out.push_back(c);
        if (static_cast<u8>(c) == kPumpEscape) out.push_back(c);
    }
    return out;
}

std::string pump_config(u32 n, u32 after_ms, const SerialConfig& c) { return control(kPumpConfig, numbered(n, after_ms) + serial_text(c)); }

std::string pump_lines(u32 n, u32 after_ms, int dtr, int rts) {
    std::string p = numbered(n, after_ms);
    p.push_back(static_cast<char>(static_cast<int8_t>(dtr < 0 ? -1 : dtr ? 1 : 0)));
    p.push_back(static_cast<char>(static_cast<int8_t>(rts < 0 ? -1 : rts ? 1 : 0)));
    return control(kPumpLines, p);
}

std::string pump_break(u32 n, u32 after_ms) { return control(kPumpBreak, numbered(n, after_ms)); }
std::string pump_release() { return control(kPumpRelease, {}); }
std::string pump_reclaim() { return control(kPumpReclaim, {}); }

// ---------------------------------------------------------------- what a serial pump says

bool pump_news_parse(std::string_view line, PumpNews* out) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    std::vector<std::string_view> f;
    for (size_t at = 0;;) {
        const size_t tab = line.find('\t', at);
        f.push_back(line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at));
        if (tab == std::string_view::npos) break;
        at = tab + 1;
    }
    auto num = [](std::string_view s) { return static_cast<u32>(std::strtoul(std::string(s).c_str(), nullptr, 10)); };
    PumpNews n;
    const std::string_view k = f[0];
    if (k == "id" && f.size() >= 5) {
        n.kind = PumpNews::kId;
        n.id.serial = std::string(f[1]);
        const std::string ids(f[2]);
        n.id.vid = static_cast<u16>(std::strtoul(ids.c_str(), nullptr, 16));
        if (const size_t colon = ids.find(':'); colon != std::string::npos) n.id.pid = static_cast<u16>(std::strtoul(ids.c_str() + colon + 1, nullptr, 16));
        n.id.iface = static_cast<int>(std::strtol(std::string(f[3]).c_str(), nullptr, 10));
        n.id.name = std::string(f[4]);
        n.text = n.id.name;
    } else if (k == "open" && f.size() >= 2) {
        n.kind = PumpNews::kOpen, n.text = std::string(f[1]);
    } else if (k == "wait") {
        n.kind = PumpNews::kWait, n.text = f.size() >= 2 ? std::string(f[1]) : std::string();
    } else if (k == "gone" && f.size() >= 2) {
        n.kind = PumpNews::kGone, n.text = std::string(f[1]);
    } else if (k == "released") {
        n.kind = PumpNews::kReleased;
    } else if (k == "done" && f.size() >= 2) {
        n.kind = PumpNews::kDone, n.n = num(f[1]);
    } else if (k == "fail" && f.size() >= 3) {
        n.kind = PumpNews::kFail, n.n = num(f[1]), n.text = std::string(f[2]);
    } else if (k == "error" && f.size() >= 2) {
        n.kind = PumpNews::kError, n.text = std::string(f[1]);
    } else {
        return false;
    }
    *out = std::move(n);
    return true;
}

// ---------------------------------------------------------------- two streams joined

void pump_join(PumpJoin& j, const std::function<bool()>& stop) {
    for (const int fd : {j.a_in, j.a_out, j.b_in, j.b_out}) no_wait(fd);
    // A side's writing ended: a socket's sending side shut (its reading goes on), a pipe closed.
    auto end_writing = [](int& fd) {
        struct stat st;
        if (::fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode)) {
            ::shutdown(fd, SHUT_WR);
        } else {
            ::close(fd);
            fd = -1;
        }
    };
    Pending ab, ba;  // what waits to go: from a to b, and from b to a
    bool a_end = false, b_end = false;  // (nothing more to read from that side)
    bool ab_done = false, ba_done = false;  // (that way has ended)
    char buf[65536];
    while (!(ab_done && ba_done)) {
        if (stop && stop()) return;
        pollfd p[4] = {{!a_end && ab.size() < kBufMost ? j.a_in : -1, POLLIN, 0},
                       {!b_end && ba.size() < kBufMost ? j.b_in : -1, POLLIN, 0},
                       {!ab_done && !ab.empty() ? j.b_out : -1, POLLOUT, 0},
                       {!ba_done && !ba.empty() ? j.a_out : -1, POLLOUT, 0}};
        if (::poll(p, 4, 200) < 0 && errno != EINTR) return;
        auto take = [&](int fd, short ev, Pending& into, bool& end) {
            if (end || !(ev & (POLLIN | POLLHUP | POLLERR))) return;
            const ssize_t k = ::read(fd, buf, sizeof buf);
            if (k > 0) into.add(buf, static_cast<size_t>(k));
            else if (k == 0 || (errno != EINTR && !would_block(errno))) end = true;
        };
        take(j.a_in, p[0].revents, ab, a_end);
        take(j.b_in, p[1].revents, ba, b_end);
        auto give = [&](Pending& from, int& to, bool& from_end, bool& done) {
            if (done) return;
            if (!from.empty() && !from.flush(to)) {  // (that side takes no more: what was for it goes nowhere)
                from.clear();
                from_end = done = true;
                return;
            }
            if (from_end && from.empty()) {
                end_writing(to);
                done = true;
            }
        };
        give(ab, j.b_out, a_end, ab_done);
        give(ba, j.a_out, b_end, ba_done);
    }
}

// ---------------------------------------------------------------- the pumps

int pump_serial(int argc, char** argv) {
    if (argc < 2) {
        say("error", {"blit-agent --serial PATH CONFIG [SERIAL VID:PID IFACE]"});
        return 2;
    }
    SerialPump p;
    p.id.path = argv[0];
    if (!serial_parse(argv[1], &p.cfg)) {
        say("error", {std::string("Not a speed or a framing: ") + argv[1]});
        return 2;
    }
    if (argc >= 5) {  // (known already: as it was when it was first opened, wherever it is now)
        p.id.serial = argv[2];
        const std::string ids = argv[3];
        p.id.vid = static_cast<u16>(std::strtoul(ids.c_str(), nullptr, 16));
        if (const size_t colon = ids.find(':'); colon != std::string::npos) p.id.pid = static_cast<u16>(std::strtoul(ids.c_str() + colon + 1, nullptr, 16));
        p.id.iface = static_cast<int>(std::strtol(argv[4], nullptr, 10));
    } else {
        const SerialPortId found = serial_identify(p.id.path);
        p.id.serial = found.serial;
        p.id.vid = found.vid, p.id.pid = found.pid;
        p.id.iface = found.iface;
        p.id.name = found.name;
        if (!found.serial.empty()) {
            char ids[16];
            std::snprintf(ids, sizeof ids, "%04x:%04x", found.vid, found.pid);
            say("id", {found.serial, ids, std::to_string(found.iface), found.name});
        }
    }
    return p.run();
}

int pump_connect(const char* target) {
    const std::string t = target;
    std::string host, port;
    if (t.starts_with('[')) {  // ([::1]:3333)
        const size_t close = t.find("]:");
        if (close != std::string::npos) host = t.substr(1, close - 1), port = t.substr(close + 2);
    } else if (const size_t colon = t.rfind(':'); colon != std::string::npos) {
        host = t.substr(0, colon), port = t.substr(colon + 1);
    }
    if (host.empty() || port.empty()) {
        std::fprintf(stderr, "Not HOST:PORT: %s\n", target);
        return 2;
    }
    std::string why;
    const int s = connect_to(host, port, &why);
    if (s < 0) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
    }
    const int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // (a debugger's packets, a console's keys: each at once)
    PumpJoin j{0, 1, s, s};
    pump_join(j);
    ::close(s);
    return 0;
}

}  // namespace blit
