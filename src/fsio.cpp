#include "fsio.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include "remote.hpp"

extern char** environ;

namespace blit {

std::vector<FsEntry> fs_list(const std::string& dir) {
    std::vector<FsEntry> out;
    if (is_remote(dir)) {
        std::string there;
        std::vector<RemoteEntry> list;
        if (const std::shared_ptr<Remote> r = remote_for(dir, &there); r && r->list(there, &list))
            for (RemoteEntry& e : list) out.push_back({std::move(e.name), e.dir});
    } else if (DIR* d = ::opendir(dir.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const char* name = e->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            bool is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) is_dir = fs_kind(dir + "/" + name) == 2;
            out.push_back({name, is_dir});
        }
        ::closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const FsEntry& a, const FsEntry& b) { return a.name < b.name; });
    return out;
}

int fs_kind(const std::string& path) {
    if (is_remote(path)) {
        std::string there;
        RemoteStat rs;
        const std::shared_ptr<Remote> r = remote_for(path, &there);
        if (!r || !r->stat(there, &rs)) return 0;
        return rs.kind == 2 ? 2 : rs.kind == 0 ? 0 : 1;
    }
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

bool fs_read(const std::string& path, std::string* text, u64 most) {
    text->clear();
    if (is_remote(path)) {
        std::string there;
        const std::shared_ptr<Remote> r = remote_for(path, &there);
        return r && r->read(there, most, text) == 0;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    for (size_t k; text->size() < most && (k = std::fread(buf, 1, sizeof buf, f)) > 0;) text->append(buf, k);
    std::fclose(f);
    if (text->size() > most) text->resize(most);
    return true;
}

bool fs_read_whole(const std::string& path, std::string* bytes, std::string* why, u64 most, std::stop_token stop, i64* modified) {
    if (is_remote(path)) return fs_read_range(path, 0, ~0ull, most, bytes, why, stop, {}, modified);
    // (A byte more than may be, to tell a file of just that size from a larger one.)
    if (!fs_read(path, bytes, most + 1)) return *why = fs_kind(path) == 0 ? "there is no such file" : "the file could not be read", false;
    if (bytes->size() > most) return *why = "the file is too big (over " + std::to_string(most >> 20) + " MB)", false;
    return true;
}

bool fs_read_range(const std::string& path, u64 offset, u64 length, u64 most, std::string* bytes, std::string* why, std::stop_token stop,
                   const std::function<void(u64 done, u64 total)>& progress, i64* modified) {
    const std::string too_big = "the file is too big (over " + std::to_string(most >> 20) + " MB)";
    bytes->clear();
    if (is_remote(path)) {
        std::string there;
        std::shared_ptr<Remote> r;
        for (int waited = 0; !r && waited < 30000 && !stop.stop_requested(); waited += 200) r = remote_for(path, &there, 200);
        if (!r) return *why = remote_error(path), false;
        switch (r->fetch(there, offset, length, most, bytes, stop, progress, modified)) {
            case 0: return true;
            case 1: *why = "there is no such file"; break;
            case 2: *why = "it is not a file"; break;
            case 3: *why = too_big; break;
            case kFetchStopped: *why = "stopped"; break;
            default: *why = remote_error(path); break;
        }
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return *why = fs_kind(path) == 0 ? "there is no such file" : "the file could not be read", false;
    std::fseek(f, 0, SEEK_END);
    const long end = std::ftell(f);
    const u64 size = end > 0 ? static_cast<u64>(end) : 0, n = offset < size ? std::min(length, size - offset) : 0;
    if (n > most) {
        std::fclose(f);
        return *why = too_big, false;
    }
    std::fseek(f, static_cast<long>(std::min(offset, size)), SEEK_SET);
    bytes->resize(static_cast<size_t>(n));
    const size_t got = n ? std::fread(bytes->data(), 1, static_cast<size_t>(n), f) : 0;
    std::fclose(f);
    bytes->resize(got);
    if (progress) progress(got, n);
    return true;
}

i64 fs_remote_stamp(const std::string& path) {
    std::string there;
    const std::shared_ptr<Remote> r = remote_for(path, &there);
    RemoteStat st;
    return r && r->stat(there, &st) && st.kind == 1 ? st.modified_ns : 0;
}

bool fs_write(const std::string& path, std::string_view text, std::string* err) {
    if (is_remote(path)) {
        std::string there;
        const std::shared_ptr<Remote> r = remote_for(path, &there);
        if (!r) return *err = remote_error(path), false;
        return r->write(there, std::string(text), err);
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return *err = std::strerror(errno), false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) return *err = std::strerror(errno), false;
    return true;
}

bool fs_mkdirs(const std::string& path, std::string* err) {
    if (is_remote(path)) {
        std::string there;
        const std::shared_ptr<Remote> r = remote_for(path, &there);
        if (!r) return *err = remote_error(path), false;
        return r->make_dir(there, true, err);
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && fs_kind(path) != 2) return *err = ec.message(), false;
    return true;
}

std::string cache_dir() {
    if (const char* d = std::getenv("BLIT_CACHE_DIR"); d && *d) return d;
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/Library/Caches/blit";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/blit";
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.cache/blit";
#endif
    return {};
}

namespace {

// What comes out of fd until it closes, or until `until` (ns).
void read_all(int fd, std::string* out, u64 until) {
    char buf[16384];
    for (;;) {
        const u64 now = now_ns();
        if (now >= until) return;
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, static_cast<int>(std::min<u64>((until - now) / 1'000'000 + 1, 1000)));
        if (r < 0 && errno != EINTR) return;
        if (r <= 0) continue;
        const ssize_t k = ::read(fd, buf, sizeof buf);
        if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) return;
        if (k > 0) out->append(buf, static_cast<size_t>(k));
        if (out->size() > (64u << 20)) return;
    }
}

}  // namespace

bool fs_run(const std::string& dir, const std::string& command, std::string* out, int ms) {
    out->clear();
    // (A login shell's: the PATH a terminal would have, where a program started from the Dock has little.)
    const std::vector<std::string> argv = {"/bin/sh", "-c", "exec \"${SHELL:-/bin/sh}\" -lc \"$1\" </dev/null 2>/dev/null", "sh", command};
    const u64 until = now_ns() + static_cast<u64>(ms) * 1'000'000;
    std::string host, there;
    if (split_remote(dir, &host, &there)) {
        const std::shared_ptr<Remote> r = remote_for(dir, &there, kRemoteWaitMs);
        RemoteProc p;
        p.errors = false;  // (the shell sends them to /dev/null anyway)
        std::string err;
        if (!r || !r->spawn(argv, there, {}, &p, &err)) return false;
        ::close(p.in);
        read_all(p.out, out, until);
        ::close(p.out);
        const int code = r->wait(p.id, static_cast<int>(std::max<i64>(0, static_cast<i64>(until - std::min(until, now_ns())) / 1'000'000)));
        if (code == -2) r->kill(p.id, SIGKILL);
        return code == 0;
    }
    int pipe_out[2];
    if (::pipe(pipe_out) != 0) return false;
    ::fcntl(pipe_out[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipe_out[1], 1);
    posix_spawn_file_actions_addclose(&fa, pipe_out[1]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (!dir.empty()) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
        posix_spawn_file_actions_addchdir(&fa, dir.c_str());
#else
        posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
#endif
    }
    std::vector<std::string> words = argv;
    std::vector<char*> args;
    for (std::string& w : words) args.push_back(w.data());
    args.push_back(nullptr);
    // Nothing else this process has open goes into the command: not the pipes of a language server
    // being started on another thread (whose reader would then wait for this command to end).
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pipe_out[1]);
    if (rc != 0) {
        ::close(pipe_out[0]);
        return false;
    }
    read_all(pipe_out[0], out, until);
    ::close(pipe_out[0]);
    int st = 0;
    while (::waitpid(pid, &st, WNOHANG) == 0) {
        if (now_ns() >= until) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, 0);
            return false;
        }
        ::usleep(2000);
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

}  // namespace blit
