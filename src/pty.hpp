#pragma once
// Programs run under a pseudo-terminal: what a terminal tab runs (a shell),
// here or, through blit's agent, on another machine. The agent is built
// with this file too (agent/build.sh), so it stays plain POSIX.
#include <sys/types.h>

#include <string>
#include <vector>

namespace blit {

struct Pty {
    pid_t pid = -1;
    int fd = -1;  // the terminal's side: what the program writes is read here, and what is written here it reads
};

// Starts argv[0] (looked for on PATH) in `cwd` (none: this one), with
// `env` ("NAME=value") added to this process's environment, on a new
// pseudo-terminal of rows by cols that is its controlling terminal. The
// descriptor is non-blocking and closed on exec.
bool pty_spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env, unsigned rows, unsigned cols, Pty* out,
               std::string* err);

// The terminal's new size (the program is told with SIGWINCH).
void pty_resize(int fd, unsigned rows, unsigned cols);

}  // namespace blit
