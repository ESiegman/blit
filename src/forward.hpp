#pragma once
// A TCP port on another machine reached as one on this machine, as ssh -L
// reaches one, but over blit's own connection to that machine: a socket
// listens on this machine's loopback (127.0.0.1 and ::1), and each
// connection made to it starts blit's agent there as a pump (blit-agent
// --connect HOST:PORT, pump.hpp), which connects to HOST:PORT from that
// machine; the two are joined, each way on its own (pump_join).
//
// The seam for what reaches a port beside a board on another machine
// (OpenOCD's gdb, telnet and RTT ports, J-Link's RTT telnet, probe-rs's
// DAP server, a Renode UART socket): forward_local gives the address to
// connect to here, for a port on whichever machine the project is on.
#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"

namespace blit {

class Forward {
public:
    // A port on `host` (an ssh host, as Remote has it), `target` as that
    // machine names it ("localhost:4444"), reached at `local` here (0: a
    // port of the system's choosing). Null, with why, when nothing can
    // listen there ("port 4444 is in use here").
    static std::shared_ptr<Forward> open(const std::string& host, const std::string& target, u16 local, std::string* err);
    ~Forward();  // no longer listening, and its connections ended
    Forward(const Forward&) = delete;
    Forward& operator=(const Forward&) = delete;

    const std::string& host() const { return host_; }
    const std::string& target() const { return target_; }
    u16 port() const { return port_; }
    std::string address() const;  // "localhost:PORT"
    int connections() const { return live_; }  // joined now
    // What went wrong with a connection since last asked ("box: Cannot
    // connect to localhost:4444: Connection refused"); `told` (set before
    // the first connection, called on a thread of the forward's) says when
    // there is something to take.
    std::vector<std::string> take_errors();
    std::function<void()> told;

private:
    Forward() = default;
    void listen_loop(std::stop_token stop);
    void serve(int sock, std::stop_token stop);
    void error(const std::string& why);

    std::string host_, target_;
    u16 port_ = 0;
    int fds_[2] = {-1, -1};  // listening: 127.0.0.1, ::1
    std::atomic<int> live_{0};
    std::mutex mu_;
    std::vector<std::string> errors_;  // (mu_)
    struct Conn {
        std::jthread thread;
        std::atomic<bool> done{false};
    };
    std::list<Conn> conns_;  // (the listener's own)
    bool refused_ = false;   // (the listener's: too many at once, said)
    std::jthread listener_;  // (last: stopped and joined first)
};

// An address here ("localhost:4444") for a TCP port `target` on the machine
// `root` is on (a project's folder; ssh://host/... for another machine).
// On this machine it is the target itself, with nothing in between; on
// another, a Forward of its own, kept for as long as *keep is (the tab that
// reads it holds it). False, with why, when it cannot be had.
bool forward_local(const std::string& root, const std::string& target, std::shared_ptr<Forward>* keep, std::string* address, std::string* err);

}  // namespace blit
