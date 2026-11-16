#pragma once
// Programs run in the background for scripts (applua.cpp): each on a thread
// of its own that feeds it its input, collects what it prints and waits
// for it to end. What happens is queued, and whoever started the jobs is
// told (set_notify) and reads the queue with take(). POSIX only: elsewhere
// a job fails to start.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"

namespace blit {

struct JobSpec {
    std::vector<std::string> argv;  // the program (a path, or a name looked for in PATH) and its arguments
    std::string cwd;                // the folder to run it in; empty: this process's
    std::string input;              // what it reads from its standard input
    std::vector<std::string> env;   // "NAME=value", on top of this process's environment
    bool stream_out = false;        // report what it prints as it comes (kOut), and not all at once with kExit
    bool stream_err = false;
};

struct JobEvent {
    enum : u8 { kOut, kErr, kExit };
    u64 id = 0;
    u8 kind = kExit;
    int code = 0;          // kExit: its exit status (128 + the signal's number if a signal ended it; -1 if it never ran)
    std::string out, err;  // kOut and kErr: a piece of output (in out); kExit: everything not already reported
};

class Jobs {
public:
    Jobs();
    ~Jobs();  // ends what is still running, and waits
    Jobs(const Jobs&) = delete;
    Jobs& operator=(const Jobs&) = delete;

    // Called on a job's thread whenever take() has something new.
    void set_notify(std::function<void()> fn);
    // Starts a program. The number is the job's from then on.
    u64 start(JobSpec spec);
    // Asks a job to end (SIGTERM). False if it is not running.
    bool stop(u64 id);
    void stop_all();
    std::vector<JobEvent> take();
    size_t running() const { return running_.load(); }

private:
    struct Job {
        u64 id = 0;
        std::atomic<int> pid{0};
        std::mutex pid_mu;  // held to signal the program, and to reap it (so a pid is never signalled once it is free to be another's)
        std::atomic<bool> done{false};
        int wake[2] = {-1, -1};  // a pipe that stop() writes to: the thread is waiting on the program's pipes
        std::jthread thread;
        ~Job();
    };
    void run(Job* job, JobSpec spec);
    void push(JobEvent e);

    std::mutex mu_;
    std::function<void()> notify_;
    std::vector<JobEvent> events_;
    std::vector<std::unique_ptr<Job>> jobs_;
    std::atomic<size_t> running_{0};
    u64 next_ = 1;
};

}  // namespace blit
