#include "pty.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>

extern char** environ;

namespace blit {

namespace {

// The program's full path (PATH searched now: between fork and exec
// nothing may allocate).
std::string find_program(const std::string& name) {
    if (name.find('/') != std::string::npos) return name;
    const char* path = std::getenv("PATH");
    const std::string_view dirs = path && *path ? path : "/usr/bin:/bin";
    for (size_t at = 0; at <= dirs.size();) {
        const size_t colon = dirs.find(':', at);
        const std::string_view d = dirs.substr(at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
        const std::string full = std::string(d.empty() ? "." : d) + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(full.c_str(), X_OK) == 0) return full;
        if (colon == std::string_view::npos) break;
        at = colon + 1;
    }
    return {};
}

}  // namespace

bool pty_spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env, unsigned rows, unsigned cols, Pty* out,
               std::string* err) {
    if (argv.empty()) return *err = "nothing to run", false;
    const std::string program = find_program(argv[0]);
    if (program.empty()) return *err = "cannot find " + argv[0], false;
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return *err = std::string("cannot open a pseudo-terminal: ") + std::strerror(errno), false;
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        ::close(master);
        return *err = "cannot open a pseudo-terminal", false;
    }
    const char* name = ::ptsname(master);
    if (!name) {
        ::close(master);
        return *err = "cannot open a pseudo-terminal", false;
    }
    const std::string slave_name = name;
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows ? rows : 24);
    ws.ws_col = static_cast<unsigned short>(cols ? cols : 80);
    ::ioctl(master, TIOCSWINSZ, &ws);

    // Everything the child needs, made before it exists.
    std::vector<std::string> args_store(argv.begin(), argv.end());
    std::vector<char*> args;
    for (std::string& a : args_store) args.push_back(a.data());
    args.push_back(nullptr);
    std::vector<std::string> envs;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view var(*e);
        const std::string_view key = var.substr(0, var.find('=') + 1);
        bool replaced = false;
        for (const std::string& mine : env) replaced |= mine.starts_with(key);
        if (!replaced) envs.emplace_back(var);
    }
    envs.insert(envs.end(), env.begin(), env.end());
    std::vector<char*> envp;
    for (std::string& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(master);
        return *err = std::string("cannot start ") + argv[0] + ": " + std::strerror(errno), false;
    }
    if (pid == 0) {
        // The child: a session of its own, the terminal its controlling
        // one, and it as its input, output and errors. (Only calls that
        // are safe after fork in a program with threads.)
        ::setsid();
        const int slave = ::open(slave_name.c_str(), O_RDWR);
        if (slave < 0) ::_exit(126);
#if defined(TIOCSCTTY)
        ::ioctl(slave, TIOCSCTTY, 0);
#endif
        ::ioctl(slave, TIOCSWINSZ, &ws);  // (macOS forgets the size given before the terminal's first open)
        ::dup2(slave, 0);
        ::dup2(slave, 1);
        ::dup2(slave, 2);
        if (slave > 2) ::close(slave);
        ::close(master);
        if (!cwd.empty()) (void)::chdir(cwd.c_str());
        // (Signals the parent ignores are not the program's to ignore.)
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGCHLD, SIG_DFL);
        ::execve(program.c_str(), args.data(), envp.data());
        ::_exit(127);
    }
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    out->pid = pid;
    out->fd = master;
    return true;
}

void pty_resize(int fd, unsigned rows, unsigned cols) {
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ::ioctl(fd, TIOCSWINSZ, &ws);
}

}  // namespace blit
