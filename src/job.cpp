#include "job.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "missing.hpp"

extern char** environ;

namespace {
// Has the child start in a folder: the call's name from macOS 26 on, the
// older one before that and with glibc.
inline int spawn_chdir(posix_spawn_file_actions_t* fa, const char* dir) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
    return posix_spawn_file_actions_addchdir(fa, dir);
#else
    return posix_spawn_file_actions_addchdir_np(fa, dir);
#endif
}
}  // namespace
#endif

namespace blit {

// The most of a program's output that is kept when it is not passed on as it comes (each of stdout and stderr).
static constexpr size_t kMaxOutput = 64u << 20;

Jobs::Jobs() = default;

Jobs::Job::~Job() {
    if (thread.joinable()) thread.join();
#if !defined(_WIN32)
    if (wake[0] >= 0) close(wake[0]);
    if (wake[1] >= 0) close(wake[1]);
#endif
}

Jobs::~Jobs() {
    stop_all();
    jobs_.clear();  // (each thread is waited for: a job that will not end is killed, see run())
}

void Jobs::set_notify(std::function<void()> fn) {
    std::lock_guard lock(mu_);
    notify_ = std::move(fn);
}

void Jobs::push(JobEvent e) {
    std::function<void()> tell;
    {
        std::lock_guard lock(mu_);
        // Told once for a run of events: take() takes all that are waiting,
        // so a program that pours out output is not one wake-up per read.
        if (events_.empty()) tell = notify_;
        events_.push_back(std::move(e));
    }
    if (tell) tell();
}

std::vector<JobEvent> Jobs::take() {
    std::vector<JobEvent> out;
    {
        std::lock_guard lock(mu_);
        out.swap(events_);
    }
    // Jobs that have ended are let go of here, on the thread that started them.
    std::erase_if(jobs_, [](const std::unique_ptr<Job>& j) { return j->done.load(); });
    return out;
}

u64 Jobs::start(JobSpec spec) {
    auto job = std::make_unique<Job>();
    job->id = next_++;
    Job* j = job.get();
#if !defined(_WIN32)
    if (pipe(j->wake) == 0)
        for (const int fd : j->wake) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
#endif
    ++running_;
    j->thread = std::jthread([this, j, s = std::move(spec)](std::stop_token) mutable { run(j, std::move(s)); });
    jobs_.push_back(std::move(job));
    return j->id;
}

bool Jobs::stop(u64 id) {
    for (const std::unique_ptr<Job>& j : jobs_)
        if (j->id == id && !j->done.load()) {
            j->thread.request_stop();
#if !defined(_WIN32)
            {
                std::lock_guard lock(j->pid_mu);
                if (const int pid = j->pid.load(); pid > 0) kill(-pid, SIGTERM);  // (its group: what it started too)
            }
            if (j->wake[1] >= 0) (void)!write(j->wake[1], "x", 1);
#endif
            return true;
        }
    return false;
}

void Jobs::stop_all() {
    for (const std::unique_ptr<Job>& j : jobs_)
        if (!j->done.load()) stop(j->id);
}

#if defined(_WIN32)

void Jobs::run(Job* job, JobSpec) {
    JobEvent e;
    e.id = job->id;
    e.code = -1;
    e.err = "running programs is not there yet on Windows";
    --running_;
    job->done = true;
    push(std::move(e));
}

#else

void Jobs::run(Job* job, JobSpec spec) {
    JobEvent last;
    last.id = job->id;
    last.kind = JobEvent::kExit;
    last.code = -1;
    // (A job with a sink of its own gives it what happens, here on its thread; the rest are queued for take().)
    auto give = [&](JobEvent e) {
        if (spec.sink) spec.sink(std::move(e));
        else push(std::move(e));
    };
    auto finish = [&] {
        --running_;
        job->done = true;
        give(std::move(last));
    };
    if (spec.argv.empty() || spec.argv[0].empty()) {
        last.err = "no program to run";
        return finish();
    }
    int in[2], out[2], err[2];
    if (pipe(in) != 0) {
        last.err = std::strerror(errno);
        return finish();
    }
    if (pipe(out) != 0) {
        last.err = std::strerror(errno);
        close(in[0]), close(in[1]);
        return finish();
    }
    if (pipe(err) != 0) {
        last.err = std::strerror(errno);
        close(in[0]), close(in[1]), close(out[0]), close(out[1]);
        return finish();
    }
    for (const int fd : {in[1], out[0], err[0]}) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    for (const int fd : {in[0], out[1], err[1]}) posix_spawn_file_actions_addclose(&fa, fd);

    std::vector<std::string> words = spec.argv;
#if defined(__APPLE__) || defined(__GLIBC__)
    if (!spec.cwd.empty()) spawn_chdir(&fa, spec.cwd.c_str());
#else
    // No way to say "in this folder" to posix_spawn here: a shell goes there first.
    if (!spec.cwd.empty()) {
        words = {"/bin/sh", "-c", "cd \"$1\" && shift && exec \"$@\"", "sh", spec.cwd};
        words.insert(words.end(), spec.argv.begin(), spec.argv.end());
    }
#endif
    std::vector<char*> argv;
    for (std::string& w : words) argv.push_back(w.data());
    argv.push_back(nullptr);
    // The environment: this process's, with the job's own on top.
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view var(*e);
        const std::string_view name = var.substr(0, var.find('=') + 1);
        bool replaced = false;
        for (const std::string& mine : spec.env) replaced |= std::string_view(mine).starts_with(name);
        if (!replaced) env.emplace_back(var);
    }
    for (const std::string& mine : spec.env) env.push_back(mine);
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    // Nothing else this process has open goes into the program: not the
    // pipes of another program being started at the same time on another
    // thread (whose reader would then wait for this one to end).
    // (In a process group of its own, which it leads: asked to end, the
    // program and what it started end together, nothing left running.)
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
#if defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: CLOEXEC_DEFAULT's work, as the last of the file actions)
#endif
#endif
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]), close(out[1]), close(err[1]);
    if (rc != 0) {
        close(in[1]), close(out[0]), close(err[0]);
        // (A folder that is not there fails as the program would: the folder is to blame.)
        struct stat st;
        if (!spec.cwd.empty() && (::stat(spec.cwd.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
            last.err = "cannot run " + spec.argv[0] + " in " + spec.cwd + ": no such folder" +
                       (spec.cwd.find("://") != std::string::npos ? " here (programs are run on this machine)" : "");
        else if (rc == ENOENT && spec.argv[0].find('/') == std::string::npos)
            last.err = not_installed(spec.argv[0], {});  // (not on the PATH)
        else
            last.err = "cannot run " + spec.argv[0] + ": " + std::strerror(rc);
        return finish();
    }
    job->pid = pid;
    if (job->thread.get_stop_token().stop_requested()) kill(-pid, SIGTERM);  // (asked to end before it had begun)

    size_t sent = 0;
    bool in_open = true, out_open = true, err_open = true;
    if (spec.input.empty()) close(in[1]), in_open = false;
    char buf[65536];
    // Once it has been asked to end it gets a moment to; then it is killed,
    // and whatever it left holding its pipes open is not waited for.
    const std::stop_token stop = job->thread.get_stop_token();
    int waited_ms = 0;
    bool killed = false;
    while (out_open || err_open) {
        // (A job asked to stop is looked at every 50 ms; one that is not
        // sleeps until its program says something or stop() wakes it.)
        const bool stopping = stop.stop_requested();
        pollfd fds[4] = {{in_open ? in[1] : -1, POLLOUT, 0},
                         {out_open ? out[0] : -1, POLLIN, 0},
                         {err_open ? err[0] : -1, POLLIN, 0},
                         {stopping ? -1 : job->wake[0], POLLIN, 0}};
        const int n = ::poll(fds, 4, stopping ? 50 : -1);
        if (n < 0 && errno != EINTR) break;
        if (stopping && n == 0) {
            waited_ms += 50;
            if (waited_ms >= 300) {
                if (killed) break;
                kill(-pid, SIGKILL);
                killed = true;
                waited_ms = 200;
            }
        }
        if (in_open && (fds[0].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const ssize_t w = write(in[1], spec.input.data() + sent, spec.input.size() - sent);
            if (w > 0) sent += static_cast<size_t>(w);
            if (sent >= spec.input.size() || (w < 0 && errno != EAGAIN && errno != EINTR)) close(in[1]), in_open = false;
        }
        auto drain = [&](int fd, std::string& into, bool& open, bool stream, u8 kind) {
            for (;;) {
                const ssize_t r = read(fd, buf, sizeof buf);
                if (r > 0) {
                    if (stream) {
                        JobEvent e;
                        e.id = job->id;
                        e.kind = kind;
                        e.out.assign(buf, static_cast<size_t>(r));
                        give(std::move(e));
                    } else {
                        if (into.size() < kMaxOutput) {
                            into.append(buf, std::min<size_t>(static_cast<size_t>(r), kMaxOutput - into.size()));
                            if (into.size() >= kMaxOutput) into += "\n[blit: output cut at 64 MB]\n";
                        }
                    }
                } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                    close(fd);
                    open = false;
                    return;
                } else {
                    return;
                }
            }
        };
        if (out_open && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) drain(out[0], last.out, out_open, spec.stream_out, JobEvent::kOut);
        if (err_open && (fds[2].revents & (POLLIN | POLLERR | POLLHUP))) drain(err[0], last.err, err_open, spec.stream_err, JobEvent::kErr);
    }
    if (in_open) close(in[1]);
    if (out_open) close(out[0]);
    if (err_open) close(err[0]);
    int status = 0;
    // Waited for without letting go of it, so that stop() can still signal it; then reaped, and forgotten, under the lock.
    siginfo_t info{};
    while (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT) < 0 && errno == EINTR) {}
    {
        std::lock_guard lock(job->pid_mu);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        job->pid = 0;
    }
    last.code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    finish();
}

#endif

}  // namespace blit
