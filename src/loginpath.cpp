#include "loginpath.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.hpp"

namespace blit {
namespace {

constexpr const char* kLaunchdPath = "/usr/bin:/bin:/usr/sbin:/sbin";
constexpr const char* kMark = "__blit_path__";

std::string kept_file() {
    std::string dir;
    if (const char* d = std::getenv("BLIT_CACHE_DIR"); d && *d) dir = d;
    else if (const char* home = std::getenv("HOME"); home && *home) dir = std::string(home) + "/Library/Caches/blit";
    else return {};
    return dir + "/login-path";
}

std::string read_kept(const std::string& file) {
    std::string out;
    if (FILE* f = std::fopen(file.c_str(), "rb")) {
        char buf[8192];
        const size_t n = std::fread(buf, 1, sizeof buf, f);
        out.assign(buf, n);
        std::fclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.find(':') == std::string::npos || out.find('\n') != std::string::npos) return {};
    return out;
}

void keep(const std::string& file, const std::string& path) {
    for (size_t slash = file.find('/', 1); slash != std::string::npos; slash = file.find('/', slash + 1))
        ::mkdir(file.substr(0, slash).c_str(), 0755);  // (each folder on the way that is not there yet)
    const std::string tmp = file + ".new";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        std::fwrite(path.data(), 1, path.size(), f);
        std::fclose(f);
        ::rename(tmp.c_str(), file.c_str());
    }
}

std::string shell() {
    if (const char* s = std::getenv("SHELL"); s && *s == '/') return s;
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_shell && *pw->pw_shell == '/') return pw->pw_shell;
    return "/bin/zsh";
}

// The PATH an interactive login shell ends up with (its startup files may
// print things of their own: the line after the mark is the one).
std::string ask_shell(int wait_ms) {
    int out[2];
    if (::pipe(out) != 0) return {};
    const std::string sh = shell();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out[0]);
        ::close(out[1]);
        return {};
    }
    if (pid == 0) {
        ::setsid();  // (no terminal: an interactive shell must not wait on one)
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, 0);
            ::dup2(null, 2);
        }
        ::dup2(out[1], 1);
        ::close(out[0]);
        ::close(out[1]);
        const std::string cmd = std::string("printf '\\n") + kMark + "%s\\n' \"$PATH\"";
        ::execl(sh.c_str(), sh.c_str(), "-l", "-i", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(out[1]);
    std::string text;
    const u64 until = now_ns() + static_cast<u64>(wait_ms) * 1'000'000ull;
    char buf[4096];
    for (;;) {
        const u64 t = now_ns();
        if (t >= until) break;
        pollfd p{out[0], POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>((until - t) / 1'000'000) + 1) <= 0) break;
        const ssize_t n = ::read(out[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text.append(buf, static_cast<size_t>(n));
    }
    ::close(out[0]);
    ::kill(-pid, SIGKILL);  // (its whole group, if it is still going)
    int status = 0;
    ::waitpid(pid, &status, 0);
    const size_t at = text.rfind(kMark);
    if (at == std::string::npos) return {};
    std::string path = text.substr(at + std::strlen(kMark));
    if (const size_t nl = path.find('\n'); nl != std::string::npos) path.resize(nl);
    if (path.find(':') == std::string::npos) return {};
    return path;
}

}  // namespace

void adopt_login_path() {
    const char* now = std::getenv("PATH");
    if (now && std::strcmp(now, kLaunchdPath) != 0) return;  // (from a terminal: its PATH stands)
    const std::string file = kept_file();
    if (file.empty()) return;
    if (const std::string kept = read_kept(file); !kept.empty()) {
        ::setenv("PATH", kept.c_str(), 1);
        // The shell asked again for next time, by a grandchild nobody waits
        // for (its parent goes at once and is waited for here).
        const pid_t pid = ::fork();
        if (pid == 0) {
            if (::fork() == 0) {
                const std::string fresh = ask_shell(10'000);
                if (!fresh.empty() && fresh != kept) keep(file, fresh);
            }
            ::_exit(0);
        }
        if (pid > 0) ::waitpid(pid, nullptr, 0);
        return;
    }
    if (const std::string fresh = ask_shell(3'000); !fresh.empty()) {
        ::setenv("PATH", fresh.c_str(), 1);
        keep(file, fresh);
    }
}

}  // namespace blit
