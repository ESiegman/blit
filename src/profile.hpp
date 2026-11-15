#pragma once
// Profiling a program: where its time goes, from the system's sampling
// profiler (perf on Linux, Instruments' Time Profiler through xctrace on
// macOS), run here or on another machine (remote.hpp), and what was found:
// a call tree for the flame graph, each function's time, and the time on
// each line of source the samples could be traced to.
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
};

struct Profile {
    // What was run, where, how.
    std::string program, args, backend, host;  // host empty: this machine
    std::string when;                          // the local time it was recorded ("14:03")
    u64 weight = 0;      // of every sample (the root's total)
    u64 samples = 0;
    double seconds = 0;  // from the first sample to the last
    u32 threads = 0;
    std::string unit;    // what a sample's weight counts ("ns")
    std::vector<ProfileFunc> funcs;
    std::vector<ProfileNode> nodes;  // [0]: the root, every sample
    // Weight by source line: file -> line (from 1) -> weight.
    std::map<std::string, std::map<u32, ProfileLine>> lines;
    std::string output;  // what the program printed (its last 64 KB)
    std::string note;    // what the reader should know (no source lines found, ...)

    // Building it: a sample's frames, the innermost first.
    struct Frame {
        std::string_view name, module, file;
        u32 line = 0;
    };
    void add(std::span<const Frame> frames, u64 weight);
    void finish();  // the kids in order, functions' lines

    // The functions, the heaviest by own time first.
    std::vector<u32> by_self() const;

private:
    std::unordered_map<std::string, u32> func_ids_;            // name + '\0' + module
    std::unordered_map<u64, u32> node_ids_;                    // parent << 32 | func
    std::vector<u64> seen_;                                    // (a function counted once per sample)
    u64 sample_no_ = 0;
};

// The two profilers' output read into a profile (prefix: put before every
// source path, "ssh://host" for one recorded on another machine).
bool parse_perf_script(std::string_view text, const std::string& prefix, Profile* out, std::string* err);
bool parse_xctrace(std::string_view xml, const std::string& prefix, Profile* out, std::string* err);

// What to record.
struct ProfileRequest {
    std::string program;            // its full path; another machine's: ssh://host/path
    std::vector<std::string> args;
    std::string cwd;                // where it runs (another machine's: ssh://host/path); empty: the program's folder
    u32 frequency = 999;            // samples a second
    bool dwarf = false;             // perf: walk stacks with DWARF (for code built without frame pointers)
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
