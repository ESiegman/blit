#pragma once
// A debugger, through the Debug Adapter Protocol: the adapter (lldb-dap,
// gdb's own, debugpy) runs as a program of its own, here or on another
// machine through blit's agent, and blit and it exchange JSON messages
// framed as a language server's are ("Content-Length: n", a blank line, the
// message). appdebug.cpp is what the editor does with it.
//
// The adapter is read on a thread of the client's own; what it says is
// queued, and poll() (on the main thread) runs the answers' callbacks and
// hands back the events. Nothing on the main thread waits for it: on
// another machine it is started on that thread (the connection waited for,
// the adapter started there), and it is let go of on a thread of its own.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common.hpp"
#include "json.hpp"

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
    ~Dap();  // (stop(): nothing here waits)
    Dap(const Dap&) = delete;
    Dap& operator=(const Dap&) = delete;

    // The adapter: argv, run in `dir`. Here it is started now. On another
    // machine (ssh://host/path) the connection is waited for and the adapter
    // started there on the reader's thread: requests made meanwhile are sent
    // once it has started, and one that cannot be started has ended(), with
    // errors() saying why.
    bool start(const std::vector<std::string>& argv, const std::string& dir, std::function<void()> notify, std::string* err);
    // A request; `done` runs (in poll) with its answer. Its arguments are a JSON object's text.
    i64 request(std::string_view command, std::string_view arguments = "{}", Done done = {});
    // An answer to a request of the adapter's.
    void respond(const Message& to, bool success, std::string_view body = "{}");
    // The answers' callbacks run; the events are handed back.
    std::vector<Message> poll();
    bool ended() const;
    std::string errors() const;  // the last of what the adapter wrote as errors
    // The adapter is let go of, on a thread of its own that holds what it
    // needs (the main thread waits for none of it): given up to grace_ms first
    // to end of itself (after a disconnect, so that it can let go of the
    // program it ran), then made to end, and reaped. Nothing more is sent.
    void stop(int grace_ms = 0);

    struct Link;  // (dap.cpp: the adapter's pipes and process, and the thread reading it)

private:
    std::shared_ptr<Link> link_;
    std::vector<std::pair<i64, Done>> waiting_;  // (main thread only)
    i64 seq_ = 1;
};

}  // namespace blit
