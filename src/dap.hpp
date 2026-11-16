#pragma once
// A debugger, through the Debug Adapter Protocol: the adapter (lldb-dap,
// gdb's own, debugpy) runs as a program of its own, here or on another
// machine through blit's agent, and blit and it exchange JSON messages
// framed as a language server's are ("Content-Length: n", a blank line, the
// message). appdebug.cpp is what the editor does with it.
//
// The adapter is read on a thread of the client's own; what it says is
// queued, and poll() (on the main thread) runs the answers' callbacks and
// hands back the events.
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "json.hpp"
#include "remote.hpp"

namespace blit {

class Dap {
public:
    struct Message {
        std::string type;      // "response", "event", or "request" (the adapter asking blit)
        std::string name;      // the command or the event
        i64 seq = 0, request_seq = 0;
        bool success = true;
        std::string message;   // why a request failed
        std::shared_ptr<json::Doc> doc;
        json::Value body() const { return doc ? doc->root()["body"] : json::Value(); }
    };
    using Done = std::function<void(const Message&)>;

    Dap() = default;
    ~Dap();
    Dap(const Dap&) = delete;
    Dap& operator=(const Dap&) = delete;

    // The adapter: argv, run in `dir` (another machine's: ssh://host/path).
    bool start(const std::vector<std::string>& argv, const std::string& dir, std::function<void()> notify, std::string* err);
    // A request; `done` runs (in poll) with its answer. Its arguments are a JSON object's text.
    i64 request(std::string_view command, std::string_view arguments = "{}", Done done = {});
    // An answer to a request of the adapter's.
    void respond(const Message& to, bool success, std::string_view body = "{}");
    // The answers' callbacks run; the events are handed back.
    std::vector<Message> poll();
    bool ended() const { return ended_; }
    std::string errors() const;  // the last of what the adapter wrote as errors
    void stop();                 // the adapter is told to end, then made to

private:
    void read_loop(std::stop_token stop);
    void send(const std::string& json);

    std::shared_ptr<Remote> conn_;
    RemoteProc proc_;
    pid_t pid_ = -1;
    int in_ = -1, out_ = -1, err_ = -1;
    std::jthread reader_, err_reader_;
    std::mutex mu_;
    std::deque<Message> queue_;
    std::vector<std::pair<i64, Done>> waiting_;  // (main thread only)
    std::string errors_;
    std::atomic<bool> ended_{false};
    std::atomic<i64> seq_{1};
    std::function<void()> notify_;
    std::mutex write_mu_;
};

}  // namespace blit
