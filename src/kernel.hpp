#pragma once
// A notebook's kernel: blit's own small Python helper (its text is in
// kernel.cpp, written to blit's cache folder when it is needed) run with
// the project's Python, here or on another machine through blit's agent.
// It runs one cell at a time, in IPython's shell if it can import it (its
// display hooks, rich results, magics, plots), else in plain Python. blit
// and it talk in JSON lines over its standard input and output: blit sends
// a cell's code; it says when the cell starts (and its count), each of its
// outputs as Jupyter's JSON has it, as they come (printed text a few times
// a second), and when it is done.
//
// It is read on a thread of its own, which queues what it says and tells
// whoever started it (notify); poll() on the main thread takes it. Nothing
// on the main thread waits for it: on another machine the connection is
// waited for and the helper started on that thread, and ending it (here,
// its process group killed; there, its program) waits for nothing either.
// The helper ends of itself when its input closes, so a blit that is
// killed leaves none behind.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

class Kernel {
public:
    struct Event {
        enum Kind : u8 { kReady, kStart, kOutput, kClear, kDone, kEnded } kind = kEnded;
        u64 cell = 0;   // what blit called the cell it is of
        i64 count = -1;  // kStart, kDone: its execution count
        bool flag = false;  // kReady: in IPython; kClear: only when the next output comes; kDone: it ran without an error
        std::string text;   // kOutput: the output (its JSON); kReady: Python's version; kEnded: why, if it was not asked to
    };

    Kernel() = default;
    ~Kernel();  // (stop())
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // The helper, run in `dir` with `python` (words for a shell: what the
    // Python console runs) and given the notebook's folder to work in. On
    // another machine (ssh://host/path, docker://...) the Python is chosen
    // there as the console's would be, and it is started on the reader's
    // thread: cells asked for meanwhile are sent once it has started.
    bool start(const std::string& python, const std::string& dir, const std::string& notebook_dir, std::function<void()> notify, std::string* err);
    // A cell's code run, once those before it have (blit's name for it: `cell`).
    void run(u64 cell, std::string_view code);
    // The cell running stopped: Ctrl+C in it (SIGINT to the helper).
    void interrupt();
    std::vector<Event> poll();
    bool ended() const;
    i64 pid() const;  // the helper's process, as it says (-1: not yet; on another machine, its there)
    bool remote() const;
    // Let go of: its programs ended, and reaped on a thread of their own.
    void stop();

    // The helper's text (for the tests).
    static std::string_view helper();

    struct Link;  // (kernel.cpp: its process and pipes, and the thread reading it)

private:
    std::shared_ptr<Link> link_;
};

}  // namespace blit
