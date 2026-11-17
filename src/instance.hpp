#pragma once

// One blit takes the files: `blit file` run while a blit of the same
// program is up hands the file to it through a socket and goes, so the file
// opens as a tab in the window already there. `blit -w file` waits until
// its tabs are closed (for git's editor). Each program file has its own
// socket, so a build being worked on never hands its files to the blit in
// Applications (POSIX only).

#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"

namespace blit {

struct OpenAsk {
    std::string path;  // made whole (from the asker's folder), or ssh://host/path
    u64 line = 0;      // from 1; 0: none given
    u64 col = 0;       // from 1; 0: none given
};

struct InstanceAsk {
    std::vector<OpenAsk> files;  // none: just come to the front
    bool wait = false;           // tell the asker when all of them have been closed
    int fd = -1;                 // the asker's connection, while it waits (the server's side)
};

// Where a running blit of this program listens.
std::string instance_socket();

// Hands the files to a running blit of this program, and waits for it to
// take them (and with wait, for their tabs to close; `taken` is called when
// it has them). 0 when it took them, 1 when it went away first, -1 when none
// is running (start one).
int instance_hand_over(const InstanceAsk& ask, void (*taken)() = nullptr);

// The running blit's side: a thread that takes the asks and passes each to
// `got` (which is called on that thread; the main thread opens the files).
class InstanceServer {
public:
    InstanceServer() = default;
    InstanceServer(const InstanceServer&) = delete;
    InstanceServer& operator=(const InstanceServer&) = delete;
    ~InstanceServer() { stop(); }

    // False when another blit of this program is listening already, or the
    // socket cannot be made.
    bool start(std::function<void(InstanceAsk)> got);
    void stop();
    // The answer to an ask that waited (or whose files did not open), and
    // the end of its connection.
    static void answer(int fd, const char* what);

private:
    int fd_ = -1;
    int wake_[2] = {-1, -1};
    std::string path_;
    std::jthread thread_;
};

}  // namespace blit
