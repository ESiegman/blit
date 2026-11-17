#pragma once
// Working on another machine's files, over ssh. A path there is written
// "ssh://host/path/on/it" (the host as ~/.ssh/config or ssh knows it), and
// everywhere blit goes to the files themselves (opening, saving, listing a
// folder, the finder, git, a language server) it asks remote.hpp when the
// path is such a one. blit's agent (agent/blit-agent.cpp) does the work on
// that machine: it is started over ssh, copied there the first time.
//
// A connection is made the first time a host is asked for, on a thread of
// its own; until it is ready, what is asked of it from the main thread
// fails at once (and the main thread is told when it is ready, see
// Remote::set_notify), while a worker thread may wait for it. A call waits
// for its answer, so the main thread makes none: what it wants of another
// machine is asked on a worker thread (appremote.cpp, the tree's reader, git's
// worker), and only what does not wait for an answer (kill, resize) goes
// from the main thread.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct FinderOptions;
struct FileIndex;
struct FindHit;

// "ssh://host/path": the host and the path on it ("/" at least).
// (BLIT_NO_REMOTE: a build for measuring what the checks cost, in which no path is one.)
#if defined(BLIT_NO_REMOTE)
inline bool is_remote(std::string_view) { return false; }
inline bool split_remote(std::string_view, std::string*, std::string*) { return false; }
#else
bool is_remote(std::string_view path);
bool split_remote(std::string_view path, std::string* host, std::string* there);
#endif
std::string remote_path(std::string_view host, std::string_view there);

class Remote;
// How long a worker thread waits for a machine's connection.
inline constexpr int kRemoteWaitMs = 30'000;
// The connection a remote path is on, and the path there. Null if the path
// is not remote, or if the connection is not ready after waiting no more
// than wait_ms for it (the main thread waits for nothing: what it asks of a
// host still connecting fails, and it is told when the host is ready).
std::shared_ptr<Remote> remote_for(std::string_view path, std::string* there, int wait_ms = 0);
// Why there is no connection for that path ("connecting to host", or what failed).
std::string remote_error(std::string_view path);

// A question ssh asks while connecting (a password, a key's passphrase,
// whether to trust a host it has not seen), waiting for the person at blit.
struct RemoteAsk {
    u64 id = 0;
    std::string host, prompt;
    bool secret = true;  // typed hidden (all but a yes-or-no question)
};

// blit run by ssh as its SSH_ASKPASS: hands the question to the blit that
// started that ssh (over the socket named in BLIT_ASKPASS_SOCKET) and prints
// the answer. main() calls it before anything else.
int remote_askpass(const char* socket, const char* prompt);
// This program's own path (what ssh is told to run to ask).
std::string program_path();

struct RemoteStat {
    int kind = 0;  // 0 nothing there, 1 a file, 2 a folder, 3 something else
    u64 size = 0;
    i64 modified_ns = 0;
};

struct RemoteEntry {
    std::string name;
    bool dir = false;
};

// A program running there, as local pipes: what is written to `in` goes to
// its input, its output and errors come out of `out` and `err`. Its output
// is held here for whoever reads `out` (and `err`) only up to a few MB:
// past that, the agent stops reading it there until it has been taken, and
// the program waits, as one writing into a full pipe does.
struct RemoteProc {
    u32 id = 0;
    int in = -1, out = -1, err = -1;
    // (Asked before spawn: false, its errors are not wanted. They go to
    // /dev/null there rather than over the connection, and `err` is -1.)
    bool errors = true;
};

class Remote {
public:
    enum State { kConnecting, kReady, kFailed };

    // The connection to a host, made when first asked for.
    static std::shared_ptr<Remote> get(const std::string& host);
    // Told (on the connection's own thread) when a connection is ready or has failed.
    static void set_notify(std::function<void()> fn);
    // Every connection there is, for the status bar and the tests.
    static std::vector<std::shared_ptr<Remote>> all();
    static void reconnect(const std::string& host);  // one that failed, made again now
    // The program ssh is to run to ask its questions (blit itself, see
    // remote_askpass). Without one ssh never asks: it fails instead (BatchMode).
    static void set_askpass(std::string program);
    // Questions waiting for an answer, oldest first; and an answer to one
    // (null: the person would not answer, and connecting fails).
    static std::vector<RemoteAsk> asks();
    static void answer(u64 id, const std::string* text);
    static void close_all();

    ~Remote();
    State state() const;
    std::string error() const;
    std::string progress() const;  // what connecting is doing now ("copying blit's agent")
    const std::string& host() const { return host_; }
    std::string home() const;
    std::string system() const;   // "Linux x86_64"
    bool wait_ready(int ms);       // for threads that may wait

    // Calls: false (with *err where there is one) when they fail or the connection does.
    bool stat(const std::string& path, RemoteStat* out);
    int read(const std::string& path, u64 most, std::string* out, i64* modified_ns = nullptr);  // 0 read, 1 none, 2 not a file, 3 too big, 4 failed, 5 no connection
    // (The text is moved into the request as it is: a big file is not copied again.)
    bool write(const std::string& path, std::string data, std::string* err, i64* modified_ns = nullptr);
    bool list(const std::string& path, std::vector<RemoteEntry>* out);
    bool make_dir(const std::string& path, bool parents, std::string* err);
    bool create(const std::string& path, std::string* err);
    bool rename(const std::string& from, const std::string& to, std::string* err);
    bool trash(const std::string& path, std::string* err);
    std::string realpath(const std::string& path);
    std::string which(const std::string& program);
    // The finder's work, done there: a folder's files, and text searched for in them.
    bool walk(const std::string& root, const FinderOptions& o, FileIndex* out, u32* walk_id);
    bool grep(u32 walk_id, const std::vector<u32>* only, const std::string& typed, u64 most_bytes, u32 most_lines, std::vector<FindHit>* hits,
              std::vector<u32>* hit_files, bool* capped);

    // A program, run in `cwd` there.
    // (rows and cols: on a pseudo-terminal of that size there; its output and
    // what it writes as errors both come out of `out`.)
    bool spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env, RemoteProc* out, std::string* err,
               unsigned rows = 0, unsigned cols = 0);
    void resize(u32 proc, unsigned rows, unsigned cols, unsigned width_px = 0, unsigned height_px = 0);  // its pseudo-terminal's size
    int wait(u32 proc, int ms = -1);  // its exit status, once it has ended (-1 if the connection went; -2 still running after ms)
    void kill(u32 proc, int sig);
    // The most output held at once for one program, waiting for its reader,
    // since the connection was made (for the tests: it stays a few MB).
    u64 held_most() const;

    struct Impl;

private:
    explicit Remote(std::string host);
    std::string host_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace blit
