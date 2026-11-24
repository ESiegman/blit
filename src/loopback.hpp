#pragma once
// The loopback socket module (evaluation 6.9 item 5; the embedded design's
// F1): a TCP endpoint a tool exposes on this machine, talked to on a thread
// of its own that wakes whoever asked when bytes come or the link changes,
// the way the debugger's and the terminals' readers do. Two ways round:
//
//   - connect: to a port a tool listens on (OpenOCD's and pyOCD's RTT
//     servers and Tcl port, J-Link's RTT telnet port 19021 and its
//     semihosting telnet port, pyOCD's semihosting console), tried again
//     whenever it is refused or closed, so that a link outlives the tool
//     going and coming back (a debug session ended and started again);
//   - listen: on a port of this machine's, for a tool that connects out
//     (QEMU's semihosting chardev), one link at a time, taken again each
//     time the tool connects.
//
// How a connection is made is the module's one seam (LoopbackDial): a TCP
// connect here, by default; the agent's TCP pump (`blit-agent --connect
// HOST:PORT`, the embedded design's F2) makes a port on another machine
// one here by giving the link a dial that starts the pump and hands back
// its descriptor. Nothing here knows of windows, the app or the agent.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "common.hpp"

namespace blit {

// Where a link goes: a host (127.0.0.1 unless said) and a port.
struct LoopbackEnd {
    std::string host = "127.0.0.1";
    u16 port = 0;
    std::string text() const;  // "127.0.0.1:19021"
};

// A connection made: a descriptor to read and write (a socket; a pump's
// pipe pair is one descriptor each way: `out` -1 when it is the same as
// `in`), or -1 and why in words ("connection refused"). Called on the
// link's thread; it may take its time, up to about a second.
struct LoopbackConn {
    int in = -1, out = -1;
};
using LoopbackDial = std::function<LoopbackConn(const LoopbackEnd&, std::string* why)>;

// A TCP connection to an end here, made within `ms` (non-blocking, closed
// on exec, Nagle off: a key typed goes at once).
LoopbackConn loopback_dial_tcp(const LoopbackEnd& end, std::string* why, int ms = 1000);
// A socket listening on a port of 127.0.0.1 (0: any free one, *bound says
// which); -1 and why when it cannot be had ("port 19020 is in use").
int loopback_listen(u16 port, u16* bound, std::string* why);
// A port no one listens on now, of 127.0.0.1's (for a tool told to listen
// there: OpenOCD's Tcl port); 0 if none could be had.
u16 loopback_free_port();

class Loopback {
public:
    enum class Mode : u8 { kConnect, kListen };
    struct Options {
        Mode mode = Mode::kConnect;
        LoopbackEnd end;
        LoopbackDial dial;     // (connect: empty is loopback_dial_tcp)
        std::string greeting;  // sent the moment a link opens (J-Link's channel string, within its 100 ms)
        int retry_ms = 250;    // how soon a refused or closed link is tried again (a second, once it has been down 10 s)
    };
    // What happens, told on the link's thread (each may take a lock of its
    // own; none is called with the link's held). `opened`: the link is up
    // (a connection made, or taken); `closed`: it went, or could not be
    // made, and why ("connection refused", "closed by the other end").
    struct Events {
        std::function<void(const char* p, size_t n)> data;
        std::function<void()> opened;
        std::function<void(const std::string& why)> closed;
    };

    Loopback(Options o, Events e);
    ~Loopback();  // (stopped, its thread joined, the descriptors closed)
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    // Bytes to the other end, from any thread: written now as far as it
    // takes them, the rest by the link's thread. False (nothing kept) while
    // the link is down.
    bool send(std::string_view bytes);
    // Tried again now, not at the next retry (a tool said it is listening).
    void retry_now();
    bool open() const { return open_; }
    // The port listened on (Mode::kListen, once it is bound), else the end's.
    u16 port() const { return port_; }
    const Options& options() const { return o_; }
    // The last reason the link was down, "" while it is up.
    std::string why() const;
    // How many times a link has opened (the tests').
    u32 opens() const { return opens_; }

private:
    void run(std::stop_token stop);
    void poke();
    bool flush();  // (the link's thread: what waits written; false when the link failed)
    void drop(const std::string& why);

    Options o_;
    Events e_;
    int wake_r_ = -1, wake_w_ = -1;
    int listen_fd_ = -1;
    std::atomic<u16> port_{0};
    std::atomic<bool> open_{false};
    std::atomic<u32> opens_{0};
    std::atomic<bool> again_{false};
    mutable std::mutex mu_;  // (the descriptors while open, what waits to be written, why)
    LoopbackConn conn_;
    std::string pending_;
    std::string why_;
    std::jthread thread_;  // (last: stopped first)
};

}  // namespace blit
