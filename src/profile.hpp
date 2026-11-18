#pragma once
// Profiling a program: where its time goes, from the system's sampling
// profiler (perf on Linux, Instruments' Time Profiler through xctrace on
// macOS), run here or on another machine (remote.hpp); or where its memory
// comes from (the pages it touches first: perf's page faults, Instruments'
// Virtual Memory Trace), or where its threads wait (Instruments' System
// Call Trace; perf's context switches); and what was found:
// a call tree for the flame graph (of all its threads, and of each), each
// function's time, and the time on each line of source the samples could
// be traced to. A profile is kept in a file of its own (to_json), and read
// again from it.
//
// Nothing here draws or waits on the main thread: a ProfileJob records and
// reads on a thread of its own and says when it has something.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common.hpp"

namespace blit {

struct ProfileFunc {
    std::string name;    // as the profiler gave it (demangled)
    std::string module;  // the program or library it is in (a path)
    std::string file;    // its source file, if known (another machine's: ssh://host/path)
    u32 line = 0;        // the first of its lines that samples were on (from 1; 0: none known)
    u64 self = 0;        // weight of the samples with it on top of the stack
    u64 total = 0;       // and of those with it anywhere in the stack
    bool own = false;    // in the program profiled, not a library
};

// One place in the call tree: a function, reached by the path above it.
struct ProfileNode {
    u32 func = 0;
    u32 parent = 0;
    u64 total = 0, self = 0;
    std::vector<u32> kids;  // the heaviest first
};

struct ProfileLine {
    u64 self = 0, total = 0;
    bool operator==(const ProfileLine&) const = default;
};

// A thread, and what it ran: a call tree of its own in the profile's nodes.
struct ProfileThread {
    std::string name;  // as the profiler named it ("Main Thread", or the program's name)
    u64 id = 0;        // its number there
    u64 weight = 0, samples = 0;
    u32 root = 0;      // the root of its tree: a node like nodes[0], whose parent is 0 but is not one of its kids
};

struct Profile {
    // What was run, where, how.
    std::string program, args, backend, host;  // host empty: this machine
    std::string when;                          // the local time it was recorded ("14:03")
    u64 weight = 0;      // of every sample (the root's total)
    u64 samples = 0;
    double seconds = 0;  // from the first sample to the last
    u32 threads = 0;
    std::string unit;    // what a sample's weight counts ("ns", "bytes")
    std::string kind = "time";  // what was recorded: "time" on the CPU, "memory" first touched, "waiting"
    std::vector<ProfileFunc> funcs;
    std::vector<ProfileNode> nodes;  // [0]: the root, every sample
    std::vector<ProfileThread> by_thread;  // the heaviest first
    // Weight by source line: file -> line (from 1) -> weight.
    std::map<std::string, std::map<u32, ProfileLine>> lines;
    std::string output;  // what the program printed (its last 64 KB)
    std::string note;    // what the reader should know (no source lines found, ...)

    // Building it: a sample's frames, the innermost first, and the thread it was on.
    struct Frame {
        std::string_view name, module, file;
        u32 line = 0;
    };
    void add(std::span<const Frame> frames, u64 weight, u64 thread = 0, std::string_view thread_name = {});
    void finish();  // the kids in order, functions' lines

    // The functions, the heaviest by own time first.
    std::vector<u32> by_self() const;
    // Each function's own weight and total under one node (a thread's root):
    // what by_self and the functions' self and total are for the whole.
    void weights_under(u32 root, std::vector<u64>* self, std::vector<u64>* total) const;
    bool thread_root(u32 node) const;

    // As a file (JSON), and back.
    std::string to_json() const;
    static bool from_json(std::string_view text, Profile* out, std::string* err);

private:
    void add_path(u32 root, std::span<const Frame> frames, u64 weight, bool counts);
    std::unordered_map<std::string, u32> func_ids_;            // name + '\0' + module
    std::unordered_map<u64, u32> node_ids_;                    // parent << 32 | func
    std::unordered_map<u64, u32> thread_ids_;                  // a thread's number -> by_thread
    std::vector<u64> seen_;                                    // (a function counted once per sample)
    u64 sample_no_ = 0;
};

// The call tree turned over (bottom-up): its roots' kids are the functions
// time was spent in (their own time, each as much as it had), each of those
// with what called it above it, and so on out to the outermost caller; for
// all threads and for each. The functions, threads and lines are the same
// (their weights are the profile's: weights_under() on this tree would give
// the callers the time), and so are their indices.
Profile invert_profile(const Profile& p);

// Two profiles of the same program compared, before and after: for each
// node of the later one's tree (all threads), the share of the earlier's
// weight the same path had (by its functions' names; -1 where it had no
// such path); and each function's own and total share before and after,
// those that changed most first, with those the later one no longer has.
struct ProfileDiff {
    std::vector<double> node_before;  // by the later profile's node
    struct Row {
        u32 func = ~0u;     // the later profile's function (~0: one it no longer has)
        std::string name;   // (the earlier's name, for one that is gone)
        double self_before = 0, self_after = 0, total_before = 0, total_after = 0;  // shares of each profile's weight
    };
    std::vector<Row> rows;
};
ProfileDiff diff_profiles(const Profile& before, const Profile& after);

// The two profilers' output read into a profile (prefix: put before every
// source path, "ssh://host" for one recorded on another machine).
// (scale: what one of perf's periods weighs, a page's bytes for page
// faults. xctrace's table says by its schema what it is: the Time
// Profiler's samples, the Virtual Memory Trace's faults of new memory, or
// the System Call Trace's calls, each weighing how long it took.)
bool parse_perf_script(std::string_view text, const std::string& prefix, Profile* out, std::string* err, u64 scale = 1);
bool parse_xctrace(std::string_view xml, const std::string& prefix, Profile* out, std::string* err);
// perf's context switches (each a sample where a thread stopped running)
// and the switches themselves (--show-switch-events), as samples of the
// stacks the threads waited on, each weighing the nanoseconds until its
// thread ran again (a thread preempted, not waiting, is left out).
std::string perf_waits_as_samples(std::string_view text);

// What to record.
struct ProfileRequest {
    std::string program;            // its full path; another machine's: ssh://host/path
    std::vector<std::string> args;
    std::vector<std::string> env;   // "NAME=value", given it on top of what it would have
    std::string cwd;                // where it runs (another machine's: ssh://host/path); empty: the program's folder
    u32 frequency = 999;            // samples a second
    bool dwarf = false;             // perf: walk stacks with DWARF (for code built without frame pointers)
    // What is recorded: time on the CPU; memory, as the pages it touches
    // first (new memory, where it was first written); or time spent
    // waiting (in system calls on macOS; off the CPU on Linux, which needs
    // kernel.perf_event_paranoid at 1 or lower).
    enum Kind : u8 { kTime, kMemory, kWaiting };
    Kind kind = kTime;
};

// Records a program and reads what was recorded, on a thread of its own.
class ProfileJob {
public:
    enum State { kStarting, kRecording, kReading, kDone, kFailed };

    ProfileJob(ProfileRequest r, std::function<void()> notify);
    ~ProfileJob();  // stops it, and waits
    void stop();    // the recording ends here (and the program with it), and what was recorded is read

    State state() const;
    std::string status() const;  // what it is doing, in a few words
    double elapsed() const;      // seconds since the recording started
    std::shared_ptr<Profile> result() const;
    std::string error() const;
    const ProfileRequest& request() const { return req_; }

    struct Impl;

private:
    ProfileRequest req_;
    std::unique_ptr<Impl> impl_;
};

// Which profiler a machine has, in a word ("perf", "xctrace"), by what
// `uname -s` says there.
const char* profiler_for(std::string_view system);

}  // namespace blit
