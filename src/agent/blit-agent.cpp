// blit's agent: the part of blit that runs on another machine, so that blit
// can work on that machine's files as though they were its own. blit
// starts it over ssh and talks to it on its standard input and output
// (protocol.hpp says how). It reads and writes files, lists folders, walks
// a folder and searches its files with the finder's own code (finder.cpp),
// and runs programs (git, language servers) with their input and output
// carried to and fro.
//
// Built here, with the compiler blit is built with: for this machine by
// CMake (the tests' stand-in for a server, and Macs), and for Linux by
// build.sh (GCC 16 in a container, linked statically, so that it runs on
// any Linux). blit copies it to a machine the first time it goes there.
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <map>
#include <memory>
#include <span>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "finder.hpp"
#include "protocol.hpp"
#include "pty.hpp"

extern char** environ;

namespace {

using namespace blit_wire;

const std::string kAgentVersion = std::format("blit-agent {}", kAgentLevel);

// ---------------------------------------------------------------- sending

std::mutex out_mu;  // a frame goes out whole, whichever thread sends it

void send_frame(uint8_t op, uint32_t id, const std::string& payload) {
    const std::string f = frame(op, id, payload);
    std::lock_guard lock(out_mu);
    for (size_t done = 0; done < f.size();) {
        const ssize_t n = ::write(1, f.data() + done, f.size() - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            pollfd p{1, POLLOUT, 0};
            ::poll(&p, 1, 1000);
        } else {
            std::_Exit(0);  // blit is gone
        }
    }
}

void reply(uint32_t id, const Writer& w) { send_frame(kReply, id, w.b); }

// Requests answered on threads of their own (reading a big file, walking a
// folder, searching it), so that a program's output is never held up.
std::atomic<int> working{0};
template <class F>
void on_its_own(F&& f) {
    ++working;
    std::jthread([f = std::forward<F>(f)] {
        f();
        --working;
    }).detach();
}

// ---------------------------------------------------------------- files

int64_t mtime_ns(const struct stat& st) {
#if defined(__APPLE__)
    return static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
}

// A file whole into `out` (no more than `most` bytes). The status as kRead
// has it; with text_only, a file with a zero byte in its first 8 KB is not
// read further (status 2, as for something that is not a file).
uint8_t read_file(const std::string& path, uint64_t most, std::string* out, int64_t* modified, bool text_only = false) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : 4;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return 4;
    }
    if (!S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) > most) {
        const bool file = S_ISREG(st.st_mode);
        ::close(fd);
        return file ? 3 : 2;
    }
    *modified = mtime_ns(st);
    size_t got = 0;
    bool binary = false;
    out->resize_and_overwrite(static_cast<size_t>(st.st_size), [&](char* p, size_t n) {
        constexpr size_t kFirst = 8192;
        while (got < n) {
            const ssize_t k = ::read(fd, p + got, (text_only && got < kFirst ? std::min(n, kFirst) : n) - got);
            if (k > 0) got += static_cast<size_t>(k);
            else if (k < 0 && errno == EINTR) continue;
            else break;
            if (text_only && got <= kFirst && (got == n || got == kFirst) && std::memchr(p, 0, got)) {
                binary = true;
                break;
            }
        }
        return binary ? 0 : got;
    });
    ::close(fd);
    return binary ? 2 : 0;
}

// A file being written: through a temporary file beside it, so that it is
// never seen half written; through a link to the file it names; keeping its
// mode. Its text comes whole (kWrite) or a part at a time (kWritePart).
struct Writing {
    std::string path, tmp, err;  // (err: why it failed, once it has: what comes after is dropped)
    int fd = -1;

    bool begin(std::string p) {
        char real[PATH_MAX];
        path = ::realpath(p.c_str(), real) ? std::string(real) : std::move(p);
        tmp = path + ".blit-tmp";
        struct stat st{};
        const bool had = ::stat(path.c_str(), &st) == 0;
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, had ? 0600 : 0644);
        if (fd < 0) {
            err = std::format("cannot write {}: {}", tmp, std::strerror(errno));
            return false;
        }
        if (had) ::fchmod(fd, st.st_mode & 07777);  // (the mode it is to have, before any of its text is in it)
        return true;
    }
    void add(std::string_view data) {
        for (size_t done = 0; fd >= 0 && done < data.size();) {
            const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
            if (n > 0) done += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else fail();
        }
    }
    void fail() {
        err = std::format("cannot save {}: {}", path, std::strerror(errno));
        ::close(fd);
        fd = -1;
        ::unlink(tmp.c_str());
    }
    bool finish(std::string* why, int64_t* modified) {
        if (fd >= 0 && ::close(fd) != 0) {
            fd = -1;
            fail();
        }
        fd = -1;
        if (err.empty() && ::rename(tmp.c_str(), path.c_str()) != 0) {
            err = std::format("cannot save {}: {}", path, std::strerror(errno));
            ::unlink(tmp.c_str());
        }
        if (!err.empty()) return *why = err, false;
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) *modified = mtime_ns(st);
        return true;
    }
};

bool write_file(std::string path, std::string_view data, std::string* err, int64_t* modified) {
    Writing w;
    if (w.begin(std::move(path))) w.add(data);
    return w.finish(err, modified);
}

// The files being written a part at a time, by blit's number for each.
std::map<uint32_t, Writing> writings;

bool make_dirs(const std::string& path, bool parents, std::string* err) {
    if (parents)
        for (size_t at = 1; (at = path.find('/', at)) != std::string::npos; ++at) ::mkdir(path.substr(0, at).c_str(), 0755);
    if (::mkdir(path.c_str(), 0755) == 0 || (parents && errno == EEXIST)) return true;
    *err = std::format("cannot make {}: {}", path, std::strerror(errno));
    return false;
}

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

// Into the freedesktop trash (~/.local/share/Trash), as file managers do,
// with a note of where it came from.
bool trash(const std::string& path, std::string* err) {
    const char* data = std::getenv("XDG_DATA_HOME");
    const std::string base = (data && *data ? std::string(data) : home() + "/.local/share") + "/Trash";
    const std::string files = base + "/files", info = base + "/info";
    make_dirs(files, true, err);
    make_dirs(info, true, err);
    const std::string name = path.substr(path.find_last_of('/') + 1);
    std::string target = name;
    for (int k = 2; ::access((files + "/" + target).c_str(), F_OK) == 0; ++k) target = std::format("{} {}", name, k);
    if (::rename(path.c_str(), (files + "/" + target).c_str()) != 0) {
        *err = std::format("cannot move {} to the trash: {}", path, std::strerror(errno));
        return false;
    }
    if (FILE* f = std::fopen((info + "/" + target + ".trashinfo").c_str(), "wb")) {
        char when[32];
        const time_t now = ::time(nullptr);
        struct tm t;
        ::localtime_r(&now, &t);
        std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &t);
        std::fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", path.c_str(), when);
        std::fclose(f);
    }
    return true;
}

std::string which(const std::string& program) {
    if (program.contains('/')) return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
    const char* path = std::getenv("PATH");
    const std::string_view dirs = path ? path : "/usr/bin:/bin";
    for (size_t at = 0; at <= dirs.size();) {
        const size_t colon = dirs.find(':', at);
        const std::string_view d = dirs.substr(at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
        const std::string full = std::format("{}/{}", d.empty() ? "." : d, program);
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(full.c_str(), X_OK) == 0) return full;
        if (colon == std::string_view::npos) break;
        at = colon + 1;
    }
    return {};
}

// The latest few folders walked, by number, for searches in them.
std::mutex walks_mu;
std::map<uint32_t, std::shared_ptr<const blit::FileIndex>> walks;
uint32_t next_walk = 1;

// ---------------------------------------------------------------- programs

// What a program has been sent and has not yet taken is held here up to
// kInMost: past that, blit is told to hold what it sends until the
// program has taken all but kInLess (kInFlow), so that what is held stays
// a few MB (what was on its way still comes).
constexpr size_t kInMost = 1u << 20, kInLess = 256u << 10;
uint32_t blit_level = 0;  // (what blit says of itself in kHello: 3 understands kInFlow)

struct Child {
    pid_t pid = -1;
    int in = -1, out = -1, err = -1;  // (on a pseudo-terminal, in and out are one descriptor)
    bool pty = false;
    std::string pending;    // input not written yet (from pending_at)
    size_t pending_at = 0;
    bool in_paused = false; // blit has been told to hold its input (kInFlow)
    bool close_in = false;  // and then its input closes
    bool paused = false;    // blit has more of its output than it has taken yet: none is read until it asks again (kFlow)
    bool exited = false;
    int status = 0;
};
std::map<uint32_t, Child> children;

void in_flow(uint32_t id, Child& c, bool paused) {
    if (c.in_paused == paused || (paused && blit_level < 3)) return;
    c.in_paused = paused;
    Writer w;
    w.u32(id).u8(paused);
    send_frame(kInFlow, 0, w.b);
}

// (quiet: what it writes as errors goes nowhere, rather than to blit, which would not read it.)
bool spawn(uint32_t id, const std::string& cwd, std::vector<std::string> argv, std::span<const std::string> env, bool quiet, std::string* err) {
    int in[2], out[2], er[2] = {-1, -1};
    if (::pipe(in) != 0) return *err = "cannot make a pipe", false;
    if (::pipe(out) != 0) {
        ::close(in[0]), ::close(in[1]);
        return *err = "cannot make a pipe", false;
    }
    if (!quiet && ::pipe(er) != 0) {
        ::close(in[0]), ::close(in[1]), ::close(out[0]), ::close(out[1]);
        return *err = "cannot make a pipe", false;
    }
    for (const int fd : {in[1], out[0], er[0]}) {
        if (fd < 0) continue;
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    // Into its folder by way of a shell (posix_spawn has no portable way),
    // which then becomes the program itself.
    std::vector<std::string> words;
    if (!cwd.empty()) {
        words = {"/bin/sh", "-c", "cd \"$1\" && shift && exec \"$@\"", "sh", cwd};
        words.insert(words.end(), argv.begin(), argv.end());
    } else {
        words = std::move(argv);
    }
    std::vector<char*> args;
    for (std::string& w : words) args.push_back(w.data());
    args.push_back(nullptr);
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view var(*e);
        const std::string_view name = var.substr(0, var.find('=') + 1);
        if (std::ranges::none_of(env, [&](const std::string& mine) { return mine.starts_with(name); })) envs.emplace_back(var);
    }
    envs.insert(envs.end(), env.begin(), env.end());
    std::vector<char*> envp;
    for (std::string& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    if (quiet) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    else posix_spawn_file_actions_adddup2(&fa, er[1], 2);
    for (const int fd : {in[0], out[1], er[1]})
        if (fd >= 0) posix_spawn_file_actions_addclose(&fa, fd);
    // (A process group of its own: what it starts in turn, a language
    // server's worker, is ended with it when blit goes.)
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, args[0], &fa, &attr, args.data(), envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in[0]), ::close(out[1]);
    if (er[1] >= 0) ::close(er[1]);
    if (rc != 0) {
        ::close(in[1]), ::close(out[0]);
        if (er[0] >= 0) ::close(er[0]);
        *err = std::format("cannot run {}: {}", words.empty() ? "" : words[0], std::strerror(rc));
        return false;
    }
    Child& c = children[id];
    c.pid = pid, c.in = in[1], c.out = out[0], c.err = er[0];
    return true;
}

// A program's end is told once all it wrote has gone, and it has exited.
// (It is waited for only then: until that its pid, and so its process
// group's, is not given to another program, which end_children would
// otherwise reach.)
void finish_children() {
    for (auto it = children.begin(); it != children.end();) {
        Child& c = it->second;
        if (!c.exited && c.pid > 0) {
            siginfo_t si{};
            if (::waitid(P_PID, static_cast<id_t>(c.pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == c.pid)
                c.exited = true, c.status = si.si_code == CLD_EXITED ? si.si_status : 128 + si.si_status;
        }
        if (c.exited && c.out < 0 && c.err < 0) {
            int st = 0;
            if (c.pid > 0) ::waitpid(c.pid, &st, WNOHANG);  // (now let go of)
            if (c.in >= 0) ::close(c.in);
            Writer w;
            w.u32(it->first).u32(static_cast<uint32_t>(c.status));
            send_frame(kExit, 0, w.b);
            it = children.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------- requests

void handle(uint8_t op, uint32_t id, std::string_view body) {
    Reader r(body.data(), body.size());
    Writer w;
    switch (op) {
        case kHello: {
            r.u32();
            blit_level = r.n >= 4 ? r.u32() : 0;  // (an older blit says only the version)
            struct utsname u;
            const std::string sys = ::uname(&u) == 0 ? std::format("{} {}", u.sysname, u.machine) : "unknown";
            w.str(std::string(kAgentVersion)).str(home()).str(sys).u32(static_cast<uint32_t>(::getpid()));
            return reply(id, w);
        }
        case kStat: {
            const std::string path = r.str();
            struct stat st;
            if (::stat(path.c_str(), &st) != 0) w.u8(0).u64(0).i64(0).u32(0);
            else w.u8(S_ISREG(st.st_mode) ? 1 : S_ISDIR(st.st_mode) ? 2 : 3).u64(static_cast<uint64_t>(st.st_size)).i64(mtime_ns(st)).u32(st.st_mode);
            return reply(id, w);
        }
        case kRead: {
            std::string path = r.str();
            const uint64_t most = r.u64();
            return on_its_own([id, path = std::move(path), most] {
                std::string data;
                int64_t m = 0;
                const uint8_t status = read_file(path, most, &data, &m);
                Writer out;
                out.u8(status).str(data).i64(m);
                reply(id, out);
            });
        }
        case kWrite: {
            const std::string path = r.str(), data = r.str();
            std::string err;
            int64_t m = 0;
            const bool ok = write_file(path, data, &err, &m);
            w.u8(ok).str(err).i64(m);
            return reply(id, w);
        }
        case kWritePart: {
            const uint32_t number = r.u32();
            const uint8_t part = r.u8();
            std::string path = r.str();
            const uint32_t k = r.u32();
            const std::string_view data(r.p, std::min<size_t>(k, r.n));  // (as it is in the frame: not copied)
            Writing& f = writings[number];
            if (part & kFirst) f.begin(std::move(path));
            f.add(data);
            if (!(part & kLast)) return;
            std::string err;
            int64_t m = 0;
            const bool ok = f.finish(&err, &m);
            writings.erase(number);
            w.u8(ok).str(err).i64(m);
            return reply(id, w);
        }
        case kList: {
            const std::string path = r.str();
            DIR* d = ::opendir(path.c_str());
            if (!d) {
                w.u8(0).u32(0);
                return reply(id, w);
            }
            std::vector<std::pair<std::string, uint8_t>> items;
            while (const dirent* e = ::readdir(d)) {
                const std::string_view name = e->d_name;
                if (name == "." || name == "..") continue;
                uint8_t kind = e->d_type == DT_DIR ? 2 : e->d_type == DT_REG ? 1 : 0;
                if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
                    struct stat st;
                    if (::stat(std::format("{}/{}", path, name).c_str(), &st) == 0) kind = S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
                }
                if (kind) items.emplace_back(name, kind);
            }
            ::closedir(d);
            w.u8(1).u32(static_cast<uint32_t>(items.size()));
            for (const auto& [name, kind] : items) w.str(name).u8(kind);
            return reply(id, w);
        }
        case kMkdir: {
            const std::string path = r.str();
            const bool parents = r.u8() != 0;
            std::string err;
            const bool ok = make_dirs(path, parents, &err);
            w.u8(ok).str(err);
            return reply(id, w);
        }
        case kCreate: {
            const std::string path = r.str();
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd >= 0) ::close(fd);
            w.u8(fd >= 0).str(fd >= 0 ? std::string() : std::format("cannot make {}: {}", path, std::strerror(errno)));
            return reply(id, w);
        }
        case kRename: {
            const std::string from = r.str(), to = r.str();
            const bool ok = ::rename(from.c_str(), to.c_str()) == 0;
            w.u8(ok).str(ok ? std::string() : std::format("cannot rename {}: {}", from, std::strerror(errno)));
            return reply(id, w);
        }
        case kTrash: {
            std::string err;
            const bool ok = trash(r.str(), &err);
            w.u8(ok).str(err);
            return reply(id, w);
        }
        case kRealpath: {
            const std::string path = r.str();
            char real[PATH_MAX];
            const bool ok = ::realpath(path.c_str(), real) != nullptr;
            w.u8(ok).str(ok ? std::string(real) : path);
            return reply(id, w);
        }
        case kWhich: {
            w.str(which(r.str()));
            return reply(id, w);
        }
        case kWalk: {
            std::string root = r.str();
            blit::FinderOptions o;
            o.gitignore = r.u8() != 0;
            o.hidden = r.u8() != 0;
            o.max_files = r.u32();
            o.ignore.resize(r.u32());
            for (std::string& rule : o.ignore) rule = r.str();
            return on_its_own([id, root = std::move(root), o = std::move(o)] {
                std::shared_ptr<const blit::FileIndex> ix = blit::Finder::walk(root, o);
                uint32_t number;
                {
                    std::lock_guard lock(walks_mu);
                    number = next_walk++;
                    walks[number] = ix;
                    while (walks.size() > 4) walks.erase(walks.begin());
                }
                Writer out;
                out.u32(number).u8(ix->truncated).u32(static_cast<uint32_t>(ix->size()));
                for (size_t i = 0; i < ix->size(); ++i) {
                    const std::string_view p = ix->path(i);
                    out.str(p.data(), p.size());
                }
                reply(id, out);
            });
        }
        case kGrep: {
            const uint32_t number = r.u32();
            std::vector<uint32_t> only(r.u32());
            for (uint32_t& f : only) f = r.u32();
            const std::string typed = r.str();
            const uint64_t most_bytes = r.u64();
            const uint32_t most_lines = r.u32();
            std::shared_ptr<const blit::FileIndex> ix;
            {
                std::lock_guard lock(walks_mu);
                if (auto it = walks.find(number); it != walks.end()) ix = it->second;
            }
            return on_its_own([id, ix, only = std::move(only), typed, most_bytes, most_lines] {
                const std::vector<blit::FuzzyTerm> terms = blit::fuzzy_terms(typed);
                std::vector<blit::FindHit> hits;
                std::vector<uint32_t> hit_files;
                std::atomic<bool> full{false};
                if (ix && !terms.empty()) {
                    // Several threads, each taking the next file (as the finder does).
                    const size_t count = only.empty() ? ix->size() : only.size();
                    std::atomic<size_t> next{0};
                    std::mutex m;
                    auto work = [&] {
                        std::string text;
                        std::vector<blit::FindHit> found;
                        for (size_t k; !full.load() && (k = next.fetch_add(1)) < count;) {
                            const uint32_t file = only.empty() ? static_cast<uint32_t>(k) : only[k];
                            if (file >= ix->size()) continue;
                            int64_t mt = 0;
                            if (read_file(std::format("{}/{}", ix->root, ix->path(file)), most_bytes, &text, &mt, true) != 0 || text.empty()) continue;
                            found.clear();
                            if (!blit::Finder::grep(text, terms, file, most_lines, &found) || found.empty()) continue;
                            std::lock_guard lock(m);
                            hit_files.push_back(file);
                            for (blit::FindHit& h : found) hits.push_back(std::move(h));
                            if (hits.size() >= most_lines) full.store(true);
                        }
                    };
                    {
                        std::vector<std::jthread> helpers;
                        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
                        for (unsigned t = 1; t < threads; ++t) helpers.emplace_back(work);
                        work();
                    }
                }
                std::ranges::sort(hits, [](const blit::FindHit& a, const blit::FindHit& b) { return a.file != b.file ? a.file < b.file : a.line < b.line; });
                if (hits.size() > most_lines) hits.resize(most_lines);
                std::ranges::sort(hit_files);
                Writer out;
                out.u8(full.load()).u32(static_cast<uint32_t>(hits.size()));
                for (const blit::FindHit& h : hits) out.u32(h.file).u32(h.line).u32(h.col).str(h.text);
                out.u32(static_cast<uint32_t>(hit_files.size()));
                for (const uint32_t f : hit_files) out.u32(f);
                reply(id, out);
            });
        }
        case kSpawn: {
            const uint32_t proc = r.u32();
            const std::string cwd = r.str();
            std::vector<std::string> argv(r.u32());
            for (std::string& a : argv) a = r.str();
            std::vector<std::string> env(r.u32());
            for (std::string& e : env) e = r.str();
            const uint8_t flags = r.n > 0 ? r.u8() : 0;  // (none from a blit before them)
            std::string err = argv.empty() ? "nothing to run" : "";
            const bool ok = !argv.empty() && spawn(proc, cwd, std::move(argv), env, (flags & kQuiet) != 0, &err);
            w.u8(ok).str(err);
            return reply(id, w);
        }
        case kSpawnPty: {
            const uint32_t proc = r.u32();
            const std::string cwd = r.str();
            const uint32_t rows = r.u32(), cols = r.u32();
            std::vector<std::string> argv(r.u32());
            for (std::string& a : argv) a = r.str();
            std::vector<std::string> env(r.u32());
            for (std::string& e : env) e = r.str();
            blit::Pty p;
            std::string err;
            const bool ok = blit::pty_spawn(argv, cwd, env, rows, cols, &p, &err);
            if (ok) {
                Child& c = children[proc];
                c.pid = p.pid, c.in = p.fd, c.out = p.fd, c.pty = true;
            }
            w.u8(ok).str(err);
            return reply(id, w);
        }
        case kResize: {
            const uint32_t proc = r.u32(), rows = r.u32(), cols = r.u32();
            const uint32_t px_w = r.n >= 8 ? r.u32() : 0, px_h = r.n >= 4 ? r.u32() : 0;  // (in pixels: from blit 3)
            if (auto it = children.find(proc); it != children.end() && it->second.pty && it->second.out >= 0) blit::pty_resize(it->second.out, rows, cols, px_w, px_h);
            return;
        }
        case kStdin: {
            const uint32_t proc = r.u32();
            const std::string data = r.str();
            if (auto it = children.find(proc); it != children.end()) {
                Child& c = it->second;
                if (data.empty()) c.close_in = true;
                else c.pending += data;
                if (c.pending.size() - c.pending_at > kInMost) in_flow(proc, c, true);
            }
            return;
        }
        case kKill: {
            const uint32_t proc = r.u32();
            const int sig = static_cast<int>(r.u32());
            if (auto it = children.find(proc); it != children.end() && it->second.pid > 0 && !it->second.exited) ::kill(it->second.pid, sig);
            return;
        }
        case kFlow: {
            const uint32_t proc = r.u32();
            const bool paused = r.u8() != 0;
            if (auto it = children.find(proc); it != children.end()) it->second.paused = paused;
            return;
        }
        default: {
            w.u8(0).str("unknown request");
            return reply(id, w);
        }
    }
}

// Once blit has gone, what it started is told to end; whatever has not by
// kKillAfter is killed, and whatever still holds a pipe open by kLeaveAfter
// (a program that has left its group) is left: the agent ends all the same.
constexpr auto kKillAfter = std::chrono::seconds(2), kLeaveAfter = std::chrono::seconds(3);

// Each program's process group (its own: spawn), and the program, if it is
// still there: a terminal's hung up on, another's sent `sig`. (The group
// even when the program has exited: what it started may still be running.
// Its pid is not another program's meanwhile: finish_children.)
void end_children(int sig) {
    for (auto& [id, c] : children) {
        if (c.pid <= 0) continue;
        const int s = c.pty && sig == SIGTERM ? SIGHUP : sig;  // (a shell does not end on SIGTERM; a terminal that hangs up ends it)
        ::kill(-c.pid, s);
        if (!c.exited) ::kill(c.pid, s);
    }
}

// Told to end (SIGTERM, SIGHUP: what ssh here is sent, where it is the
// agent itself, or what someone ends it with there): as when blit goes.
volatile std::sig_atomic_t told_to_end = 0;

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    for (const int sig : {SIGTERM, SIGHUP}) std::signal(sig, [](int) { told_to_end = 1; });
    ::fcntl(0, F_SETFL, ::fcntl(0, F_GETFL) | O_NONBLOCK);
    std::string in;  // what has come from blit and is not yet a whole frame
    char buf[65536];
    bool open = true;
    std::chrono::steady_clock::time_point gone{};  // (when blit went)
    bool killed = false;
    auto blit_gone = [&] {  // the programs it started go too
        open = false;
        gone = std::chrono::steady_clock::now();
        for (auto& [pid, c] : children) c.paused = false;  // (what they still write is read, so that they can end)
        end_children(SIGTERM);
    };
    while (open || working.load() > 0 || !children.empty()) {
        if (open && told_to_end) blit_gone();
        if (!open) {
            const auto waited = std::chrono::steady_clock::now() - gone;
            if (waited >= kLeaveAfter) std::_Exit(0);  // (no waiting for the finder's threads either)
            if (waited >= kKillAfter && !killed) {
                killed = true;
                end_children(SIGKILL);
            }
        }
        // What to wait for: blit, and each program's output and input.
        std::vector<pollfd> fds;
        std::vector<std::pair<uint32_t, int>> who;  // for each after blit's: (process, 0 input / 1 output / 2 errors)
        if (open) fds.push_back({0, POLLIN, 0});
        for (auto& [pid, c] : children) {
            // (A paused program's output waits in its pipe, and once that is full, the program waits.)
            if (c.out >= 0 && !c.paused) fds.push_back({c.out, POLLIN, 0}), who.emplace_back(pid, 1);
            if (c.err >= 0 && !c.paused) fds.push_back({c.err, POLLIN, 0}), who.emplace_back(pid, 2);
            if (c.in >= 0 && c.pending_at < c.pending.size()) fds.push_back({c.in, POLLOUT, 0}), who.emplace_back(pid, 0);
            else if (c.in >= 0 && c.close_in && !c.pty) ::close(c.in), c.in = -1;  // (a terminal's input is its output: it stays)
        }
        if (fds.empty()) {
            ::usleep(10'000);
            finish_children();
            continue;
        }
        if (::poll(fds.data(), fds.size(), 50) < 0 && errno != EINTR) break;
        size_t k = 0;
        if (open) {
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                for (;;) {
                    const ssize_t got = ::read(0, buf, sizeof buf);
                    if (got > 0) {
                        in.append(buf, static_cast<size_t>(got));
                    } else if (got == 0) {
                        blit_gone();
                        break;
                    } else {
                        break;
                    }
                }
                size_t at = 0;  // each whole frame there is
                while (in.size() - at >= 4) {
                    const uint32_t size = Reader(in.data() + at, 4).u32();
                    if (size < 5 || in.size() - at - 4 < size) break;
                    Reader f(in.data() + at + 4, size);
                    const uint8_t op = f.u8();
                    const uint32_t id = f.u32();
                    handle(op, id, std::string_view(in).substr(at + 9, size - 5));
                    at += 4 + size;
                }
                in.erase(0, at);
            }
            k = 1;
        }
        for (size_t i = 0; k + i < fds.size(); ++i) {
            if (!fds[k + i].revents) continue;
            auto it = children.find(who[i].first);
            if (it == children.end()) continue;
            Child& c = it->second;
            if (who[i].second == 0) {
                const ssize_t put = ::write(c.in, c.pending.data() + c.pending_at, c.pending.size() - c.pending_at);
                if (put > 0) c.pending_at += static_cast<size_t>(put);
                else if (put < 0 && errno != EAGAIN && errno != EINTR) c.pending.clear(), c.pending_at = 0, c.close_in = !c.pty;
                if (c.pending_at == c.pending.size()) c.pending.clear(), c.pending_at = 0;
                else if (c.pending_at > (64u << 10) && c.pending_at * 2 > c.pending.size()) c.pending.erase(0, c.pending_at), c.pending_at = 0;  // (taken: let go of)
                if (c.pending.size() - c.pending_at <= kInLess) in_flow(who[i].first, c, false);
                continue;
            }
            int& fd = who[i].second == 1 ? c.out : c.err;
            if (fd < 0) continue;
            const ssize_t got = ::read(fd, buf, sizeof buf);
            if (got > 0) {
                Writer w;
                w.u32(it->first).u8(static_cast<uint8_t>(who[i].second)).str(buf, static_cast<size_t>(got));
                send_frame(kOutput, 0, w.b);
            } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                ::close(fd);
                fd = -1;
                if (c.pty) c.in = -1, c.pending.clear(), c.pending_at = 0, in_flow(it->first, c, false);  // (the same descriptor)
            }
        }
        finish_children();
    }
    return 0;
}
