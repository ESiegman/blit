#pragma once
// Engines: programs blit drives for what it shows natively, run with no
// terminal tab (a probe picker's listing, the Boards view's,
// `openFPGALoader --scan-usb`, a flash tool asked what it sees), here or on
// another machine through blit's agent alike. Each run is on a thread of its
// own: what it prints comes a line at a time (a JSON line as a value, where
// asked), then its end, on that thread, never the main one. A run has a
// time limit and can be called off; `which` is asked once per machine and
// program, then kept.
//
// The transport is the engine's: pipes (output and errors apart, input
// given), or a pseudo-terminal for a tool that draws its progress only to a
// terminal (probe-rs's bars, which indicatif hides on a pipe; esptool's and
// openFPGALoader's in-place redraws), whose errors come with its output and
// whose line being drawn over with \r comes too, as kRedrawn. Here a pipe's
// program is one of Jobs' (job.hpp), a terminal's is pty.cpp's; there each
// is the agent's (kSpawn, kSpawnPty), through remote.hpp. POSIX only.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common.hpp"
#include "json.hpp"

namespace blit {

struct EngineSpec {
    std::string host;               // where: "" this machine, else a host as remote.hpp names it ("lab", "docker:NAME")
    std::vector<std::string> argv;  // the program (a path, or a name looked for on PATH: there, the agent's) and its arguments
    std::string cwd;                // the folder it runs in (empty: this process's; there, the agent's)
    std::vector<std::string> env;   // "NAME=value", added to the environment
    std::string input;              // what it reads (pipes only: a terminal's program reads nothing)
    bool pty = false;               // on a pseudo-terminal of rows by cols (else pipes)
    u32 rows = 24, cols = 160;
    u32 timeout_ms = 0;             // the most it may take (0: no limit); past it, it is ended as cancel() ends it
    bool json = false;              // a line that is a JSON object goes to on_json, the rest to on_line
};

// How a run ended.
struct EngineExit {
    int code = -1;           // its exit status (128 + the signal's number if one ended it); -1: it never ran, or the connection went
    bool started = false;    // it ran
    bool timed_out = false;  // ended for taking longer than its limit
    bool cancelled = false;  // ended by cancel()
    std::string error;       // why it did not start, or the connection went
};

class Engine {
public:
    enum Stream : u8 { kOut = 1, kErr = 2, kRedrawn = 3 };  // (kRedrawn: a line drawn over with \r before it ended, on a terminal)
    // Each line (its newline, a terminal's \r and escape sequences gone), on the run's thread.
    using LineFn = std::function<void(std::string_view line, Stream stream)>;
    using JsonFn = std::function<void(const json::Value& value, Stream stream)>;
    using ExitFn = std::function<void(const EngineExit& exit)>;

    // Runs a program; on_exit is called once, after its last line. The run
    // goes on while the Engine is held (letting go of it cancels the run and
    // waits for its thread).
    static std::shared_ptr<Engine> run(EngineSpec spec, LineFn on_line, ExitFn on_exit, JsonFn on_json = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void cancel();         // ends it (SIGTERM, then SIGKILL if it lingers); on_exit says cancelled
    bool running() const { return !ended_.load(); }
    bool wait(int ms);     // until it has ended and on_exit has been called (false: not within ms)

    // The program's full path on a machine ("" this one), as its PATH has it
    // (there, the agent's): "" if it is not there. Asked once per machine and
    // program, then kept (forget() to ask again; one not there is asked
    // about again a few seconds on: a tool installed since its warning).
    // It may wait for the machine (a connection, a round trip): never on the
    // main thread; which_known() is for that.
    static std::string which(const std::string& host, const std::string& program);
    static bool which_known(const std::string& host, const std::string& program, std::string* path);
    static void forget(const std::string& host);
    static u64 which_asked();  // (the times a machine was asked, not the kept answers: for the tests)

private:
    Engine() = default;
    void work(EngineSpec spec);
    void here_pipe(const EngineSpec& spec, EngineExit* exit);
    void here_pty(const EngineSpec& spec, EngineExit* exit);
    void there(const EngineSpec& spec, EngineExit* exit);
    bool stopping(const EngineSpec& spec, EngineExit* exit) const;  // cancelled, or past its time (said in exit)

    struct Lines;
    LineFn on_line_;
    JsonFn on_json_;
    ExitFn on_exit_;
    bool json_ = false;
    u64 deadline_ns_ = 0;
    std::atomic<bool> cancelled_{false}, ended_{false};
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::jthread thread_;
};

}  // namespace blit
