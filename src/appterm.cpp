// Terminals in the editor: a tab running a shell, here or on another
// machine through blit's agent, which moves among tabs and splits as a
// file's does. Ctrl+` shows them in a split along the bottom, the one the
// build's output uses, and hides it again (its programs keep running). Its
// program runs under a pseudo-terminal (pty.hpp); what it writes is read
// on a thread of its own and drawn by term.hpp's screen; keys and text go
// to it as a terminal sends them.
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <deque>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "buildscan.hpp"
#include "look.hpp"
#include "picture.h"
#include "pty.hpp"
#include "remote.hpp"
#include "term.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

constexpr u32 kSelectionTone = 7;  // (Popup::tones: 1-6 the programs' backgrounds, 7 what is selected)

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 14;
    SDL_PushEvent(&ev);
}

// A PNG a program sends a terminal (kitty's pictures), read by the system's reader.
bool read_picture(std::string_view bytes, std::vector<u8>* rgba, u32* w, u32* h) {
    BlitPicture p{};
    char err[128];
    if (!blit_picture_read(bytes.data(), bytes.size(), 8192, &p, err, sizeof err)) return false;
    rgba->assign(p.rgba, p.rgba + static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 4);
    *w = static_cast<u32>(p.w), *h = static_cast<u32>(p.h);
    blit_picture_free(&p);
    return true;
}

// The 16 colours programs name, for a dark background and a light one.
const u32 kDark16[16] = {0x1E1E1E, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xCCCCCC,
                         0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xE5E5E5};
const u32 kLight16[16] = {0x000000, 0xCD3131, 0x00BC00, 0x949800, 0x0451A5, 0xBC05BC, 0x0598BC, 0x555555,
                          0x666666, 0xCD3131, 0x14CE14, 0xB5BA00, 0x0451A5, 0xBC05BC, 0x0598BC, 0xA5A5A5};

}  // namespace

// A terminal's tab: the program, its screen, and how it is drawn.
struct TermView {
    std::mutex mu;  // (the screen: written by the reader, drawn by the main thread)
    Term term{80, 24};
    // The program: here (a pseudo-terminal) or there (through the agent).
    Pty pty;
    std::shared_ptr<Remote> conn;
    RemoteProc proc;
    std::string host;  // empty: here
    std::atomic<int> in{-1};  // (set by the reader once a program on another machine has started)
    int out = -1;
    std::jthread reader;
    std::atomic<bool> told{false}, ended{false};
    bool unstarted = false;  // (there: it never started, and the screen says why; set before ended)
    bool keep = false;    // a command's (a build's): the tab stays when it ends, and says how it went
    std::string command;  // (that command)
    std::string title;    // the tab's name, whatever the program calls itself
    std::string dir;      // the folder it was started in
    // A build's: its output read for problems, on the reader's thread, and
    // what that found, for the main thread to take (term_sync).
    std::unique_ptr<BuildScan> scan;
    std::mutex scan_mu;
    std::vector<BuildProblem> scanned;   // (scan_mu)
    std::vector<std::string> scan_dirs;  // (scan_mu) the folders the build said it went into
    u64 scan_dirs_gen = 0;
    std::atomic<bool> scan_news{false};
    u64 started_ns = 0;
    std::atomic<int> status{0};
    // The Python console's: what was sent to it before it could take a
    // paste (it has not started yet), for when it can.
    bool python = false;
    std::string waiting;
    // Drawn as.
    TextBox box;
    u64 drawn = ~0ull;
    u32 cols = 0, rows = 0;
    int x = 0, y = 0, pad_x = 0, pad_y = 0;
    float palette[32][4] = {};
    float tones[8][4] = {};
    u64 scroll = 0;  // lines of the history scrolled into view (0: the screen)
    // What is selected, from (line, column) to (line, column), lines counted
    // from the oldest in the history.
    bool selecting = false, selected = false;
    u64 sel_line0 = 0, sel_line1 = 0, anchor_line = 0;
    u32 sel_col0 = 0, sel_col1 = 0, anchor_col = 0;
    // The mouse, told to a program that asked for it: the button held (-1:
    // none), the cell it was last told of, and the wheel's part notches.
    int mouse_button = -1;
    u32 mouse_row = ~0u, mouse_col = ~0u;
    float wheel_left = 0;
    // Its pictures on the GPU, one for each picture's pixels (kitty's image
    // put there twice is one), as of the screen's count of pictures.
    Renderer* gpu = nullptr;
    std::vector<std::pair<std::shared_ptr<const std::vector<u8>>, u32>> pics;
    u64 pics_gen = ~0ull;

    ~TermView() {
        // The program is hung up on (a shell ends), and the reader stops.
        if (conn && proc.id) conn->kill(proc.id, SIGHUP);
        if (pty.pid > 0 && !ended) {
            ::kill(-pty.pid, SIGHUP);
            ::kill(pty.pid, SIGHUP);
        }
        reader.request_stop();
        if (reader.joinable()) reader.join();
        if (conn && proc.id) conn->kill(proc.id, SIGHUP);  // (started meanwhile)
        if (in >= 0 && in != out) ::close(in);
        if (out >= 0) ::close(out);
        if (pty.pid > 0 && !ended) {  // (not one the reader has seen end and reaped)
            int st = 0;
            if (::waitpid(pty.pid, &st, WNOHANG) == 0) {
                // A shell that ignores the hangup does not get to keep the tab, or its programs, alive.
                ::kill(-pty.pid, SIGKILL);
                ::kill(pty.pid, SIGKILL);
                ::waitpid(pty.pid, &st, 0);
            }
        }
        if (conn && proc.id && !ended) {
            // The program there is reaped, on a thread of its own: the connection keeps a record
            // and descriptors for it until it is, and it may be slow to end.
            std::thread([c = conn, id = proc.id] {
                if (c->wait(id, 2000) == -2) {
                    c->kill(id, SIGKILL);
                    c->wait(id, 3000);
                }
            }).detach();
        }
        if (gpu)
            for (const auto& [pixels, id] : pics) gpu->image_drop(id);
    }

    // On another machine: its connection (made now if it is not there yet)
    // and the program started there, before anything can be read.
    std::string remote_dir, remote_err;
    std::vector<std::string> remote_argv, remote_env;
    bool start_remote(std::stop_token& stop) {
        auto say = [&](const std::string& what) {
            std::lock_guard lock(mu);
            term.feed(what);
        };
        say("\x1b[2mConnecting to " + host + "\xE2\x80\xA6\x1b[0m\r");
        if (!told.exchange(true)) wake();
        std::string there;
        std::shared_ptr<Remote> c;
        for (int waited = 0; !c && waited < kRemoteWaitMs && !stop.stop_requested(); waited += 250) {
            c = remote_for(remote_dir, &there, 250);
            if (!c) {
                std::string h;
                split_remote(remote_dir, &h, &there);
                bool failed = false;
                for (const std::shared_ptr<Remote>& r : Remote::all()) failed |= r->host() == h && r->state() == Remote::kFailed;
                if (failed) break;
            }
        }
        if (!c) {
            say("\x1b[K\x1b[31m" + remote_error(remote_dir) + "\x1b[0m\r\n");
            return false;
        }
        u32 h, w;
        {
            std::lock_guard lock(mu);
            h = term.rows(), w = term.cols();
        }
        std::string err;
        RemoteProc p;
        if (!c->spawn(remote_argv, there, remote_env, &p, &err, h, w)) {
            say("\x1b[K\x1b[31mCannot start a shell on " + host + ": " + err + "\x1b[0m\r\n");
            return false;
        }
        ::close(p.err);  // (a terminal's errors come with its output)
        ::fcntl(p.in, F_SETFL, ::fcntl(p.in, F_GETFL) | O_NONBLOCK);
        conn = c;
        proc = p;
        out = p.out;
        in = p.in;
        say("\x1b[K");
        {
            std::lock_guard lock(mu);  // (the size may have changed while connecting)
            if (term.rows() != h || term.cols() != w) conn->resize(proc.id, term.rows(), term.cols());
        }
        return true;
    }

    // What the scan has found, handed over (on the reader's thread).
    void scan_hand_over() {
        if (scan->found().empty() && scan->dirs_gen() == scan_dirs_gen) return;
        {
            std::lock_guard lock(scan_mu);
            for (BuildProblem& p : scan->found()) scanned.push_back(std::move(p));
            if (scan->dirs_gen() != scan_dirs_gen) scan_dirs = scan->dirs();
        }
        scan->found().clear();
        scan_dirs_gen = scan->dirs_gen();
        scan_news = true;
    }

    // What the program writes, onto the screen; what the screen answers, back to it.
    void read_loop(std::stop_token stop) {
        if (!remote_dir.empty() && !start_remote(stop)) {
            status = 1;
            unstarted = true;
            ended = true;
            wake();
            return;
        }
        char buf[65536];
        bool first = true;
        while (!stop.stop_requested()) {
            // (What the program has not yet taken of what was written to it is offered again whenever it can.)
            const bool backed_up = backed.load();
            const int wi = in == out ? 0 : 1;
            pollfd p[2] = {{out, POLLIN, 0}, {in, 0, 0}};
            if (backed_up) p[wi].events |= POLLOUT;
            int wait = 100;
            {
                std::lock_guard lock(out_mu);
                if (!later.empty()) wait = static_cast<int>(std::clamp<i64>((static_cast<i64>(later.front().first) - static_cast<i64>(now_ns())) / 1'000'000, 0, 100));
            }
            const int r = ::poll(p, wi + 1, wait);
            if (r < 0 && errno != EINTR) break;
            send_due();
            if (r <= 0) continue;
            if (backed_up && (p[wi].revents & (POLLOUT | POLLERR | POLLHUP))) flush_backlog();
            if (!(p[0].revents & ~POLLOUT)) continue;
            const ssize_t k = ::read(out, buf, sizeof buf);
            if (k > 0 && first && pty.fd >= 0) {
                // (The tab's size again, now the program has its terminal
                // open: a size set before that may not have held.)
                first = false;
                std::lock_guard lock(mu);
                pty_resize(pty.fd, term.rows(), term.cols());
            }
            if (k > 0) {
                std::string reply;
                {
                    std::lock_guard lock(mu);
                    term.feed(std::string_view(buf, static_cast<size_t>(k)));
                    reply = term.take_reply();
                }
                if (!reply.empty()) send(reply);
                if (scan) {  // (before the main thread is told: it takes what was found when it is)
                    scan->feed(std::string_view(buf, static_cast<size_t>(k)));
                    scan_hand_over();
                }
                if (!told.exchange(true)) wake();
            } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                break;  // (it has ended: a pseudo-terminal says EIO, a pipe its end)
            }
        }
        if (stop.stop_requested()) return;
        if (scan) {
            scan->finish();
            scan_hand_over();
        }
        int st = 0;
        if (conn) {
            while ((st = conn->wait(proc.id, 100)) == -2) {  // (as below: the tab closing is not held up)
                if (stop.stop_requested()) return;
            }
        } else if (pty.pid > 0) {
            // (Looked at now and then: a program that outlives its terminal must not hold up closing the tab.)
            for (;;) {
                const pid_t r = ::waitpid(pty.pid, &st, WNOHANG);
                if (r == pty.pid) break;
                if (r < 0 && errno != EINTR) {
                    st = 0;
                    break;
                }
                if (stop.stop_requested()) return;
                ::usleep(20'000);
            }
            st = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        }
        status = st;
        ended = true;
        wake();
    }

    // Bytes to the program (a key, a paste): what it takes now is written
    // now, the rest is kept for the reader's thread to hand over as it can,
    // so a long paste never holds up the one that draws.
    std::mutex out_mu;
    std::string backlog;
    size_t backlog_at = 0;
    std::atomic<bool> backed{false};

    void send(std::string_view s) {
        const int fd = in;
        if (fd < 0 || ended) return;
        std::lock_guard lock(out_mu);
        if (backlog_at < backlog.size()) {  // (what is waiting goes first)
            backlog.append(s);
            return;
        }
        while (!s.empty()) {
            const ssize_t k = ::write(fd, s.data(), s.size());
            if (k > 0) {
                s.remove_prefix(static_cast<size_t>(k));
            } else if (k < 0 && errno == EINTR) {
                continue;
            } else if (k < 0 && errno == EAGAIN) {
                backlog.assign(s);
                backlog_at = 0;
                backed = true;
                return;
            } else {
                return;
            }
        }
    }

    // Bytes to send at a time of their own (the Returns after a paste to
    // Python's prompt: one that comes in the same read as the paste is taken
    // as part of it).
    std::deque<std::pair<u64, std::string>> later;  // (out_mu) in time's order

    void send_later(u64 due, std::string s) {
        std::lock_guard lock(out_mu);
        later.emplace_back(due, std::move(s));
    }

    void send_due() {
        std::vector<std::string> due;
        {
            std::lock_guard lock(out_mu);
            const u64 now = now_ns();
            while (!later.empty() && later.front().first <= now) {
                due.push_back(std::move(later.front().second));
                later.pop_front();
            }
        }
        for (const std::string& d : due) send(d);
    }

    void flush_backlog() {
        std::lock_guard lock(out_mu);
        while (backlog_at < backlog.size()) {
            const ssize_t k = ::write(in, backlog.data() + backlog_at, backlog.size() - backlog_at);
            if (k > 0) {
                backlog_at += static_cast<size_t>(k);
            } else if (k < 0 && errno == EINTR) {
                continue;
            } else if (k < 0 && errno == EAGAIN) {
                return;
            } else {
                break;  // it does not take input any more
            }
        }
        backlog.clear();
        backlog_at = 0;
        backed = false;
    }
};

// The split along the bottom where Ctrl+` shows the terminals and the
// build's output runs (below). Hidden, it is out of the arrangement but
// whole, here: its terminals keep running, and it comes back as it was.
struct App::TermDock {
    u64 id = 0;      // its pane (0: none made yet)
    u64 below = 0;   // the pane it is below, to go back below
    u64 back = 0;    // the pane the keyboard came from, to give it back to
    u64 term = 0;    // the terminal it last had the keyboard on
    std::unique_ptr<Pane> away;  // while hidden
    float room = 0.3f;           // its share of the height
    static void enter(App& a, bool fresh);
};

// ---------------------------------------------------------------- opening one

// A terminal in a new tab, in `dir` (the folder being worked in when none
// is given): a shell there, or on the machine that folder is on. A hint
// shows dimmed on its first line, above what the shell writes.
u64 App::term_open(const std::string& dir_given, const std::string& command, const std::string& title, std::string_view hint, bool console) {
    const std::string* root = panel_root();
    const std::string dir = !dir_given.empty() ? dir_given : root ? *root : default_workdir();
    auto v = std::make_shared<TermView>();
    std::string host, there;
    const bool remote = split_remote(dir, &host, &there);
    // A login shell, the user's own (there: the one ssh would start), with
    // UTF-8 text when nothing says which (a program started from the Dock
    // has no LANG).
    const std::string lang = remote ? "[ -z \"$LANG$LC_ALL$LC_CTYPE\" ] && LANG=C.UTF-8 && export LANG; "
                                    : "[ -z \"$LANG$LC_ALL$LC_CTYPE\" ] && LANG=en_US.UTF-8 && export LANG; ";
    std::vector<std::string> argv = {"/bin/sh", "-c", lang + "exec \"${SHELL:-/bin/sh}\" -l"};
#if BLIT_DEV
    if (const char* sh = SDL_getenv("BLIT_TERM_SHELL"); sh && *sh) argv = {"/bin/sh", "-c", lang + sh};  // (tests: a shell that is the same everywhere)
#endif
    // A build's: the command, run by the user's login shell (its PATH is a
    // terminal's, where a blit started from the Dock has little).
    if (!command.empty()) argv = {"/bin/sh", "-c", lang + "exec \"${SHELL:-/bin/sh}\" -lc \"$1\"", "sh", command};
    v->keep = !command.empty();
    v->python = console;
    v->command = command;
    v->title = title;
    v->dir = dir;
    if (v->keep && !console)  // (a file is looked for there when a relative path could be from more than one folder; not on another machine: a round trip each)
        v->scan = std::make_unique<BuildScan>(dir, remote ? std::function<bool(const std::string&)>{} : [](const std::string& p) {
            struct stat st;
            return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        });
    v->started_ns = now_ns();
    const std::vector<std::string> env = {"TERM=xterm-256color", "COLORTERM=truecolor", "TERM_PROGRAM=blit"};
    std::string err;
    const u32 rows = 24, cols = 80;  // (the tab's own size follows at its first frame)
    if (remote) {  // (connected to, and the shell started, on the reader's thread)
        v->host = host;
        v->remote_dir = dir;
        v->remote_argv = argv;
        v->remote_env = env;
    } else {
        if (!pty_spawn(argv, dir, env, rows, cols, &v->pty, &err)) {
            tell(SDL_MESSAGEBOX_ERROR, ("Cannot start a terminal: " + err).c_str());
            return 0;
        }
        v->in = v->out = v->pty.fd;
    }
    static const bool reader = (Term::set_picture_reader(read_picture), true);
    (void)reader;
    v->gpu = &gpu_;
    {
        std::lock_guard lock(v->mu);
        v->term.resize(cols, rows);
        v->term.set_cell_px(static_cast<u32>(font_->cell_w()), static_cast<u32>(font_->cell_h()));
        if (!hint.empty()) v->term.feed("\x1b[2m" + std::string(hint) + "\x1b[0m\r\n");  // (before the reader: the shell's first line comes after it)
    }
    TermView* raw = v.get();
    v->reader = std::jthread([raw](std::stop_token stop) { raw->read_loop(stop); });
    const size_t at = std::min(pane_->tab + 1, pane_->docs.size());  // (after the tab in front; first in a split with none yet)
    Doc* d = add_doc(at);
    d->title = !title.empty() ? title : remote ? "Terminal: " + host : "Terminal";
    d->read_only = true;  // (its text is the program's: saving it is no saving)
    d->term = v;
    tab_show(at);
    panel_focus(false);
    dirty_ = true;
    return d->id;
}

// The program has written something, or ended: its tab draws again; one
// whose shell has ended goes (or says how it ended, if that was not well).
// The terminals of the hidden split too.
void App::term_sync() {
    for (size_t k = 0; k <= panes_.size(); ++k) {
        Pane* const p = k < panes_.size() ? panes_[k].get() : dock_ ? dock_->away.get() : nullptr;  // (the hidden one last)
        if (!p) continue;
        for (size_t i = 0; i < p->docs.size(); ++i) {
            Doc& d = *p->docs[i];
            if (!d.term) continue;
            TermView& v = *d.term;
            v.told = false;
            if (!v.waiting.empty()) py_flush(v);  // (the console may take what was sent to it now)
            if (v.scan && v.scan_news.exchange(false)) {  // problems the build reported (before it is seen to end: the last ones count)
                std::vector<BuildProblem> found;
                {
                    std::lock_guard lock(v.scan_mu);
                    found.swap(v.scanned);
                }
                if (!found.empty()) build_found(d.id, std::move(found));
            }
            // The tab's name: the program's title for itself, else the folder it is in.
            std::string title;
            {
                std::lock_guard lock(v.mu);
                title = !v.term.title().empty() ? v.term.title() : v.term.cwd();
            }
            if (!title.empty() && v.title.empty()) {
                if (title.size() > 60) title = "\xE2\x80\xA6" + title.substr(title.size() - 59);
                const std::string named = v.host.empty() ? title : title + " (" + v.host + ")";
                if (d.title != named) d.title = named;
            }
            if (v.ended && !d.term_closed) {
                d.term_closed = true;
                // (One on another machine whose connection went did not fail or exit: it is not known how it went.)
                const bool lost = v.status == -1 && v.conn && v.conn->state() == Remote::kFailed;
                if (v.keep) {  // (a build's: how it went, and the time it took)
                    char took[48];
                    std::snprintf(took, sizeof took, "%.1f s", static_cast<double>(now_ns() - v.started_ns) * 1e-9);
                    std::lock_guard lock(v.mu);
                    v.term.feed(v.status == 0 ? std::string("\r\n\x1b[32m[done in ") + took + "]\x1b[0m"
                                : lost        ? "\r\n\x1b[31m[connection to " + v.host + " lost after " + took + "]\x1b[0m"
                                              : "\r\n\x1b[31m[failed with " + std::to_string(v.status.load()) + " after " + took + "]\x1b[0m");
                    build_ended(d.id, v.status);
                    continue;
                }
                if (v.status == 0) {  // (exit: the tab goes, as a terminal window would)
                    if (dock_ && p == dock_->away.get()) {  // (hidden: and the split, with its last)
                        forget_doc(p->docs[i].get());
                        const bool front = p->docs[i].get() == p->doc;
                        p->docs.erase(p->docs.begin() + static_cast<std::ptrdiff_t>(i));
                        if (p->docs.empty()) {
                            dock_->away.reset();
                        } else {
                            if (front) p->tab = std::min(i, p->docs.size() - 1);
                            else if (i < p->tab) --p->tab;
                            p->doc = p->docs[p->tab].get();
                        }
                        return term_sync();
                    }
                    Pane* const focused = pane_;
                    pane_ = p;
                    tab_close(i, true);
                    bool still = false;
                    for (const std::unique_ptr<Pane>& q : panes_) still |= q.get() == focused;
                    pane_ = still ? focused : panes_.front().get();
                    dirty_ = true;
                    return term_sync();  // (the tabs moved: from the start)
                }
                if (v.unstarted) continue;  // (why is on the screen already: it did not exit)
                std::lock_guard lock(v.mu);
                v.term.feed(lost ? "\r\n\x1b[31m[connection to " + v.host + " lost]\x1b[0m" : "\r\n\x1b[2m[exited with code " + std::to_string(v.status.load()) + "]\x1b[0m");
            }
        }
    }
    dirty_ = true;
}

// ---------------------------------------------------------------- the terminals' split

namespace {

constexpr std::string_view kDockHint = "Ctrl+` back to the file";

}  // namespace

App::TermDock& App::term_dock() {
    if (!dock_) dock_ = std::make_shared<TermDock>();
    return *dock_;
}

Pane* App::dock_pane() const {
    if (!dock_ || !dock_->id || panes_.size() < 2) return nullptr;  // (the only split left is not along the bottom of anything)
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->id == dock_->id) return p.get();
    return nullptr;
}

Pane* App::dock_show(Pane* above) {
    TermDock& k = term_dock();
    if (Pane* p = dock_pane()) return p;
    Pane* below = nullptr;
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->id == k.below) below = p.get();
    if (!below) below = above ? above : pane_;
    if (k.away) {
        Pane* const p = k.away.get();
        if (!put_pane(k.away, below, kEdgeBottom, k.room)) return nullptr;
        k.below = below->id;
        return p;
    }
    Pane* const p = split_pane(below, kEdgeBottom);  // (with no tabs yet: the caller gives it one)
    if (!p) return nullptr;
    split_room(p, 0.3f);
    k.id = p->id;
    k.below = below->id;
    return p;
}

// Out of the way, with what runs in it; the keyboard goes back to the split
// it came from. (A file's tab dragged into it stays on show, above.)
void App::dock_hide() {
    TermDock& k = term_dock();
    Pane* p = dock_pane();
    if (!p) return;
    Pane *back = nullptr, *above = nullptr, *other = nullptr;
    for (const std::unique_ptr<Pane>& q : panes_) {
        if (q.get() == p) continue;
        if (!other) other = q.get();
        if (q->id == k.back) back = q.get();
        if (q->id == k.below) above = q.get();
    }
    if (!above) above = back ? back : other;
    for (size_t i = p->docs.size(); i-- > 0 && above;) {
        if (p->docs[i]->term) continue;
        const bool last = p->docs.size() == 1;
        move_doc(p, i, above, above->docs.size());
        if (last) {  // (and the split went with it)
            p = nullptr;
            break;
        }
    }
    if (p) k.away = take_pane(p, &k.room);
    if (back) focus_pane(back);
    dirty_ = true;
}

// The split on show with the keyboard on a terminal there: a new one
// (fresh), else the one it last had the keyboard on, else a shell there,
// else a new one.
void App::TermDock::enter(App& a, bool fresh) {
    TermDock& k = a.term_dock();
    Pane* const from = a.pane_;
    const bool from_dock = from == a.dock_pane();
    const std::string* root = a.panel_root();
    const std::string dir = root ? *root : a.default_workdir();  // (the folder being worked in: found from the split with the keyboard)
    Pane* const p = a.dock_show(from);
    if (!p) {  // (no room for another split: a terminal tab here)
        a.term_open();
        return;
    }
    if (!from_dock) k.back = from->id;
    size_t at = SIZE_MAX;
    if (!fresh) {
        for (size_t i = 0; i < p->docs.size(); ++i) {
            const Doc& d = *p->docs[i];
            if (d.id == k.term && d.term && (d.term->keep || !d.term->ended)) at = i;
        }
        for (size_t i = p->docs.size(); at == SIZE_MAX && i-- > 0;) {
            const Doc& d = *p->docs[i];
            if (d.term && !d.term->keep && !d.term->ended) at = i;
        }
    }
    a.pane_ = p;
    const bool opened = at != SIZE_MAX ? (a.tab_show(at), true) : a.term_open(dir, {}, {}, kDockHint) != 0;
    a.pane_ = from;
    if (!opened && p->docs.empty()) {  // (it could not start, and said why)
        a.remove_pane(p);
        return;
    }
    a.focus_pane(p);
    a.panel_focus(false);
    k.term = p->doc ? p->doc->id : 0;
    a.dirty_ = true;
}

// Ctrl+`: the terminal with the keyboard; from there, the text again.
void App::term_toggle() {
    Pane* const p = dock_pane();
    if (p && p == pane_ && !panel_has_keyboard() && !find_open_) {
        if (p->doc && p->doc->term) term_dock().term = p->doc->id;
        dock_hide();
        return;
    }
    TermDock::enter(*this, false);
}

// Ctrl+Shift+`: another terminal there.
void App::term_new() { TermDock::enter(*this, true); }

// ---------------------------------------------------------------- the Python console

// The Python for a folder: its virtual environment's (for the console,
// IPython, if it has it), uv's for a uv project, else the one on the PATH.
// (A notebook's kernel on another machine has the same chosen there: kernel.cpp.)
std::string App::python_for(const std::string& root, bool console) {
    // On another machine, chosen there by the command itself, in the folder:
    // asking it from here would wait on the connection.
    if (is_remote(root))
        return std::string("sh -c '") + (console ? "[ -x .venv/bin/ipython ] && exec .venv/bin/ipython; " : "") +
               "[ -x .venv/bin/python ] && exec .venv/bin/python; { [ -e uv.lock ] || [ -e pyproject.toml ]; } && exec uv run python; exec python3'";
    auto quoted = [](const std::string& path) { return "'" + path + "'"; };  // (a folder's path with no quote in it)
    if (root.find('\'') == std::string::npos) {
        if (console && fs_kind(root + "/.venv/bin/ipython") == 1) return quoted(root + "/.venv/bin/ipython");
        if (fs_kind(root + "/.venv/bin/python") == 1) return quoted(root + "/.venv/bin/python");
    }
    if (fs_kind(root + "/uv.lock") == 1 || fs_kind(root + "/pyproject.toml") == 1) return "uv run python";
    return "python3";
}

// The Python console's tab: brought into view in the terminals' split
// (started there, the first time, with the folder's Python); the keyboard
// on it if `keyboard`. Its terminal, or null if it could not start.
TermView* App::py_console(bool keyboard) {
    Pane* const editing = pane_;
    for (size_t k = 0; k <= panes_.size(); ++k) {
        Pane* const p = k < panes_.size() ? panes_[k].get() : nullptr;
        for (size_t i = 0; p && i < p->docs.size(); ++i) {
            Doc& d = *p->docs[i];
            if (!d.term || !d.term->python || d.term->ended) continue;
            pane_ = p;
            tab_show(i);
            pane_ = editing;
            if (keyboard) focus_pane(p);
            dirty_ = true;
            return d.term.get();
        }
    }
    // (In the hidden split: it comes back, with the console in front.)
    if (dock_ && dock_->away)
        for (const std::shared_ptr<Doc>& d : dock_->away->docs)
            if (d->term && d->term->python && !d->term->ended) {
                dock_show(editing);
                return py_console(keyboard);
            }
    const std::string* root = panel_root();
    const std::string dir = root ? *root : default_workdir();
    Pane* into = dock_show(editing);
    if (!into) into = editing;
    pane_ = into;
    const u64 doc = term_open(dir, python_for(dir), "Python", {}, true);
    if (!doc && into->docs.empty() && into != editing) remove_pane(into);
    pane_ = editing;
    Doc* d = doc ? find_doc(doc) : nullptr;
    if (!d) return nullptr;
    if (keyboard) {
        Pane* p = nullptr;
        find_doc(doc, &p);
        if (p) focus_pane(p);
    }
    dirty_ = true;
    return d->term.get();
}

// Code to the console, as a paste (Python's own prompt, from 3.13, and
// IPython take one whole, indented as it is), and run. Kept until the
// console can take a paste, for a while after it starts.
void App::py_send(std::string code) {
    TermView* v = py_console(false);
    if (!v) return;
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r' || code.back() == ' ' || code.back() == '\t')) code.pop_back();
    if (code.empty()) return;
    v->waiting += (v->waiting.empty() ? "" : "\n") + code;
    py_flush(*v);
}

void App::py_flush(TermView& v) {
    if (v.waiting.empty() || v.ended) return;
    bool bracketed;
    {
        std::lock_guard lock(v.mu);
        bracketed = v.term.bracketed_paste();
    }
    const bool late = now_ns() - v.started_ns > 5'000'000'000ull;  // (a Python that never asks for pastes: line by line)
    if (!bracketed && !late) return;
    std::string code;
    code.swap(v.waiting);
    std::string out;
    bool indented = false;  // (the last line in a block: a blank line ends it)
    size_t at = 0;
    while (at <= code.size()) {
        size_t nl = code.find('\n', at);
        if (nl == std::string::npos) nl = code.size();
        std::string_view line(code.data() + at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find_first_not_of(" \t") == std::string_view::npos && !bracketed) continue;  // (line by line, a blank line would end a block early)
        indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        if (!out.empty()) out += '\r';
        out += line;
    }
    v.scroll = 0;
    if (!bracketed) {
        v.send(out + (indented ? "\r\r" : "\r"));
    } else {
        // Each Return a moment after (in a read of its own: with the paste
        // it would be part of it): one runs a line; more than one line wants
        // a second, on the empty line the first leaves.
        v.send("\x1b[200~" + out + "\x1b[201~");
        const bool lines = out.find('\r') != std::string::npos;
        const u64 now = now_ns();
        v.send_later(now + 80'000'000ull, "\r");
        if (lines) v.send_later(now + 160'000'000ull, "\r");
    }
    dirty_ = true;
}

// The lines selected (or the cursor's line, and the cursor to the next) to
// the Python console.
void App::py_send_lines() {
    Editor& ed = pane_->doc->ed;
    if (pane_->doc->term || pane_->doc->read_only) return;
    u64 lo = ed.sel_min(), hi = ed.sel_max();
    const bool chosen = cfg_.modal ? vim_selection(&lo, &hi) : ed.has_selection();
    if (!chosen) {
        const u64 line = ed.buf.line_of(ed.cursor());
        lo = ed.buf.line_start(line), hi = ed.line_end(line);
        if (line + 1 < ed.buf.line_count()) {
            const u64 next = ed.buf.line_start(line + 1);
            ed.move_to(next, false);
            scroll_to_cursor();
        }
    }
    py_send(ed.buf.text(lo, hi - lo));
}

// ---------------------------------------------------------------- what runs in one

namespace {

// A program's arguments, as it was started (none: it is gone, or not ours to see).
std::vector<std::string> args_of(pid_t pid) {
    std::vector<std::string> out;
#if defined(__APPLE__)
    int max = 0;
    size_t size = sizeof max;
    int which[2] = {CTL_KERN, KERN_ARGMAX};
    if (::sysctl(which, 2, &max, &size, nullptr, 0) != 0 || max <= 0) return out;
    std::string buf(static_cast<size_t>(max), '\0');
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    size = buf.size();
    if (::sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return out;
    buf.resize(size);
    int argc = 0;
    std::memcpy(&argc, buf.data(), sizeof argc);
    size_t at = buf.find('\0', sizeof argc);  // (after the program's path, and the padding after it)
    while (at < buf.size() && buf[at] == '\0') ++at;
    for (int i = 0; i < argc && at < buf.size(); ++i) {
        const size_t end = std::min(buf.find('\0', at), buf.size());
        out.emplace_back(buf, at, end - at);
        at = end + 1;
    }
#elif defined(__linux__)
    const int fd = ::open(("/proc/" + std::to_string(pid) + "/cmdline").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return out;
    std::string buf;
    char chunk[4096];
    for (ssize_t k; (k = ::read(fd, chunk, sizeof chunk)) > 0;) buf.append(chunk, static_cast<size_t>(k));
    ::close(fd);
    for (size_t at = 0; at < buf.size();) {
        const size_t end = std::min(buf.find('\0', at), buf.size());
        out.emplace_back(buf, at, end - at);
        at = end + 1;
    }
#else
    (void)pid;
#endif
    return out;
}

// A program in words, as it would have been typed: a script by its own
// name, not its interpreter's (node .../bin/npm run dev is "npm run dev").
std::string named(std::vector<std::string> argv) {
    auto base = [](const std::string& s) {
        std::string b = s.substr(s.find_last_of('/') == std::string::npos ? 0 : s.find_last_of('/') + 1);
        if (b.starts_with('-')) b.erase(0, 1);  // (a login shell's)
        return b;
    };
    static constexpr std::string_view kInterpreters[] = {"node", "nodejs", "python", "ruby", "perl", "php", "sh", "bash", "zsh", "dash", "ksh", "fish", "lua", "luajit"};
    if (argv.size() > 1 && !argv[1].starts_with('-')) {
        std::string first = base(argv[0]);
        if (first.starts_with("python")) first = "python";  // (python3, python3.12 ...)
        if (std::ranges::find(kInterpreters, first) != std::end(kInterpreters)) argv.erase(argv.begin());
    }
    if (argv.empty()) return {};
    std::string out = base(argv[0]);
    for (size_t i = 1; i < argv.size(); ++i) out += " " + argv[i];
    return fit_text(out, 48);
}

// Whether a terminal runs something besides its shell, and what, in
// words: a build's command until it ends; in a shell here, the program in
// the foreground (the one the terminal's keys go to). What a shell on
// another machine runs cannot be seen from here. `what` stays empty when
// the program's name cannot be read.
bool busy_with(const TermView& v, std::string* what) {
    if (v.ended) return false;
    if (v.keep) return *what = fit_text(v.command, 48), true;
    if (v.pty.fd < 0 || v.pty.pid <= 0) return false;
    const pid_t fg = ::tcgetpgrp(v.pty.fd);
    if (fg <= 0 || fg == v.pty.pid) return false;
    *what = named(args_of(fg));
    return true;
}

}  // namespace

// Closing a terminal whose program still runs ends it: asked first.
bool App::term_may_close(size_t i) {
    if (i >= pane_->docs.size() || !pane_->docs[i]->term) return true;
    std::string what;
    if (!busy_with(*pane_->docs[i]->term, &what)) return true;
    tab_show(i);  // what the question is about
    return confirm((what.empty() ? std::string("A program") : "'" + what + "'") + " is still running in this terminal. Close it?", "Close");
}

size_t App::term_busy_count() {
    size_t n = 0;
    std::string what;
    auto count = [&](const Pane& p) {
        for (const std::shared_ptr<Doc>& d : p.docs)
            if (d->term && busy_with(*d->term, &what)) ++n;
    };
    for (const std::unique_ptr<Pane>& p : panes_) count(*p);
    if (dock_ && dock_->away) count(*dock_->away);
    return n;
}

// The folder a terminal was started in (ssh://host/... for one on a host).
const std::string& App::term_folder(const TermView& v) { return v.dir; }

// ---------------------------------------------------------------- drawing

namespace {

void rgb_of(u32 c, float out[4]) {
    out[0] = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(c & 0xFF) / 255.0f;
    out[3] = 1;
}

u32 pack(const float c[4]) {
    auto b = [](float f) { return static_cast<u32>(std::clamp(std::lround(f * 255.0f), 0L, 255L)); };
    return b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
}

// A colour of the 256 programs name, as red-green-blue.
u32 indexed(u32 i, bool dark) {
    if (i < 16) return (dark ? kDark16 : kLight16)[i];
    if (i < 232) {
        i -= 16;
        auto level = [](u32 v) { return v ? 55 + v * 40 : 0; };
        return level(i / 36) << 16 | level(i / 6 % 6) << 8 | level(i % 6);
    }
    const u32 g = 8 + (i - 232) * 10;
    return g << 16 | g << 8 | g;
}

u32 distance(u32 a, u32 b) {
    const int dr = static_cast<int>(a >> 16 & 0xFF) - static_cast<int>(b >> 16 & 0xFF), dg = static_cast<int>(a >> 8 & 0xFF) - static_cast<int>(b >> 8 & 0xFF),
              db = static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF);
    return static_cast<u32>(dr * dr * 2 + dg * dg * 4 + db * db * 3);
}

// Colours for one frame of a screen: the popup's palette takes 32 inks and
// 7 backgrounds; the colours on screen take the first places free, and any
// beyond those the nearest taken.
struct Colours {
    std::vector<u32> inks, backs;  // red-green-blue
    u32 ink(u32 rgb) {
        for (u32 i = 0; i < inks.size(); ++i)
            if (inks[i] == rgb) return i;
        if (inks.size() < 32) {
            inks.push_back(rgb);
            return static_cast<u32>(inks.size() - 1);
        }
        u32 best = 0;
        for (u32 i = 1; i < inks.size(); ++i)
            if (distance(inks[i], rgb) < distance(inks[best], rgb)) best = i;
        return best;
    }
    u32 back(u32 rgb, u32 deflt) {  // 0: the default background; 1-6 a tone
        if (rgb == deflt) return 0;
        for (u32 i = 0; i < backs.size(); ++i)
            if (backs[i] == rgb) return i + 1;
        if (backs.size() < kSelectionTone - 1) {
            backs.push_back(rgb);
            return static_cast<u32>(backs.size());
        }
        u32 best = 0;
        u32 best_d = distance(deflt, rgb);
        for (u32 i = 0; i < backs.size(); ++i)
            if (distance(backs[i], rgb) < best_d) best = i + 1, best_d = distance(backs[i], rgb);
        return best;
    }
};

}  // namespace

// Draws each terminal tab in front in its pane, over the text area: one
// box of the screen's cells, with the screen's colours.
void App::term_layout(FrameDesc& fd) {
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->term || fd.popup_count >= std::size(fd.popups)) continue;
        TermView& v = *pane->doc->term;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 4 || h < ch * 2) continue;
        const int margin = cw / 2;
        const u32 cols = static_cast<u32>(std::max(2, (w - 2 * margin) / cw)), rows = static_cast<u32>(std::max(1, h / ch));
        v.x = x, v.y = y, v.pad_x = margin, v.pad_y = (h - static_cast<int>(rows) * ch) / 2;
        const bool focused = pane.get() == pane_ && !find_open_ && !panel_has_keyboard();
        std::lock_guard lock(v.mu);
        Term& t = v.term;
        t.set_cell_px(static_cast<u32>(cw), static_cast<u32>(ch));
        if (t.cols() != cols || t.rows() != rows) {  // the tab's size: the program is told (in pixels too, for its pictures)
            if (t.cols() != cols) v.selected = v.selecting = false;  // (its lines are wrapped again: the places chosen are not where they were)
            t.resize(cols, rows);
            if (v.conn) v.conn->resize(v.proc.id, rows, cols, cols * static_cast<u32>(cw), rows * static_cast<u32>(ch));
            else if (v.pty.fd >= 0) pty_resize(v.pty.fd, rows, cols, cols * static_cast<u32>(cw), rows * static_cast<u32>(ch));
        }
        v.scroll = std::min<u64>(v.scroll, t.history());
        u64 key = t.generation() * 1000003ull ^ v.scroll * 7919 ^ font.generation() * 31 ^ (focused ? 1 : 0) ^ static_cast<u64>(cols) << 40 ^
                  (v.selected ? v.sel_line0 * 131 + v.sel_line1 * 137 + v.sel_col0 * 139 + v.sel_col1 * 149 + 1 : 0) ^ static_cast<u64>(pack(theme_.fg)) << 8 ^
                  static_cast<u64>(pack(theme_.bg)) << 20;
        if (key != v.drawn || v.cols != cols || v.rows != rows) {
            v.drawn = key;
            v.cols = cols, v.rows = rows;
            v.box.resize(cols, rows);
            const bool dark = 0.299f * theme_.bg[0] + 0.587f * theme_.bg[1] + 0.114f * theme_.bg[2] < 0.5f;
            const u32 fg0 = pack(theme_.fg), bg0 = pack(theme_.bg);
            Colours pal;
            pal.inks.push_back(fg0);  // ([0] the default text colour)
            const u64 hist = t.history(), first = hist - v.scroll;  // the line at the top of the view, counted from the oldest
            for (u32 r = 0; r < rows; ++r) {
                const u64 line = first + r;
                const TermCell* cells = line < hist ? t.history_line(line).data() : &t.at(static_cast<u32>(line - hist), 0);
                const u32 n = line < hist ? static_cast<u32>(std::min<size_t>(t.history_line(line).size(), cols)) : cols;
                u32* out = v.box.cells.data() + static_cast<size_t>(r) * cols;
                for (u32 c = 0; c < n; ++c) {
                    const TermCell& tc = cells[c];
                    if (tc.attr & TermCell::kWideTail) continue;  // (drawn with its left half)
                    u32 fg = tc.fg == kTermDefault ? fg0 : (tc.fg >> 24) == 1 ? indexed(tc.fg & 0xFF, dark) : tc.fg & 0xFFFFFF;
                    u32 bg = tc.bg == kTermDefault ? bg0 : (tc.bg >> 24) == 1 ? indexed(tc.bg & 0xFF, dark) : tc.bg & 0xFFFFFF;
                    if (tc.attr & TermCell::kInverse) std::swap(fg, bg);
                    if (tc.attr & TermCell::kDim) {  // (halfway to the background)
                        fg = ((fg >> 16 & 0xFF) + (bg >> 16 & 0xFF)) / 2 << 16 | ((fg >> 8 & 0xFF) + (bg >> 8 & 0xFF)) / 2 << 8 | ((fg & 0xFF) + (bg & 0xFF)) / 2;
                    }
                    u32 style = (tc.attr & TermCell::kBold ? Font::kBold : 0) | (tc.attr & TermCell::kItalic ? Font::kItalic : 0) |
                                (tc.attr & TermCell::kUnderline ? Font::kUnderline : 0) | (tc.attr & TermCell::kStrike ? Font::kStrike : 0);
                    const u32 cp = (tc.attr & TermCell::kHidden) || tc.cp < 0x20 ? ' ' : tc.cp;
                    u32 g = font.glyph(cp, style && font.ensure_style(style) ? style : 0);
                    const u32 slot = g & (Font::kWide - 1);
                    u32 cell = slot | pal.ink(fg) << kCellInkShift;
                    if (const u32 tone = pal.back(bg, bg0)) cell |= kCellSelected | tone << kCellDecoShift;
                    // Selected: its own tone.
                    auto in_selection = [&](u32 col) {
                        if (!v.selected) return false;
                        const bool after_start = line > v.sel_line0 || (line == v.sel_line0 && col >= v.sel_col0);
                        const bool before_end = line < v.sel_line1 || (line == v.sel_line1 && col < v.sel_col1);
                        return after_start && before_end;
                    };
                    if (in_selection(c)) cell = (cell & ~(kCellDecoMask)) | kCellSelected | kSelectionTone << kCellDecoShift;
                    out[c] = cell;
                    if ((g & Font::kWide) || (tc.attr & TermCell::kWide)) {
                        if (c + 1 < cols) out[c + 1] = (cell & ~0xFFFFu) | ((g & Font::kWide) ? slot + 1 : font.glyph(' ') & (Font::kWide - 1));
                    }
                }
                // The rest of a line from the history that is shorter than the view.
                for (u32 c = n; c < cols; ++c) out[c] = font.glyph(' ') & (Font::kWide - 1);
            }
            // The cursor: a block where it is, the character on it in the background's colour.
            if (v.scroll == 0 && t.cursor_visible() && !v.ended) {
                u32& cell = v.box.cells[static_cast<size_t>(t.cursor_row()) * cols + t.cursor_col()];
                if (focused) cell = (cell & 0xFFFFu) | pal.ink(bg0) << kCellInkShift | kCellSelected;
                else cell = (cell & ~(kCellDecoMask | kCellSelected)) | kCellSelected | kSelectionTone << kCellDecoShift;  // (an outline would be better: dim)
            }
            for (u32 i = 0; i < 32; ++i) rgb_of(i < pal.inks.size() ? pal.inks[i] : fg0, v.palette[i]);
            for (u32 i = 1; i < kSelectionTone; ++i) rgb_of(i - 1 < pal.backs.size() ? pal.backs[i - 1] : bg0, v.tones[i]);
            std::memcpy(v.tones[kSelectionTone], theme_.sel, sizeof v.tones[kSelectionTone]);
        }
        Popup& q = fd.popups[fd.popup_count++];
        q = Popup{};
        q.x = x, q.y = y, q.w = w, q.h = h;
        q.pad_x = v.pad_x, q.pad_y = v.pad_y;
        q.cols = v.box.cols, q.rows = v.box.rows;
        q.cells = v.box.cells.data();
        q.border_px = 0;
        q.bg = theme_.bg;
        q.sel = theme_.cursor;
        q.tones = &v.tones[0][0];
        q.palette = &v.palette[0][0];
        if (!t.pictures().empty() || !v.pics.empty()) term_pictures(fd, v, x, y, w, h);
    }
}

// A terminal's pictures (sixel's, kitty's), over its box: on the GPU once
// each (and let go of when none on the screen or in the history has those
// pixels any more), and drawn where their lines are on show, each pixel a
// pixel unless it fills the cells it was given. (v.mu is held.)
void App::term_pictures(FrameDesc& fd, TermView& v, int x, int y, int w, int h) {
    const Term& t = v.term;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    if (v.pics_gen != t.pictures_generation()) {
        v.pics_gen = t.pictures_generation();
        std::erase_if(v.pics, [&](const auto& g) {
            const bool used = std::ranges::any_of(t.pictures(), [&](const TermPicture& p) { return p.rgba == g.first; });
            if (!used) gpu_.image_drop(g.second);
            return !used;
        });
        for (const TermPicture& p : t.pictures())
            if (p.rgba && std::ranges::none_of(v.pics, [&](const auto& g) { return g.first == p.rgba; }))
                if (const u32 id = gpu_.image_add(p.rgba->data(), static_cast<int>(p.w), static_cast<int>(p.h))) v.pics.emplace_back(p.rgba, id);
    }
    const bool alt = t.alt_screen();
    const u64 first = t.lines_gone() + t.history() - v.scroll;  // the line at the top of the view
    for (const TermPicture& p : t.pictures()) {
        if (p.alt != alt || fd.picture_count >= std::size(fd.pictures)) continue;
        const i64 row = alt ? static_cast<i64>(p.line) : static_cast<i64>(p.line) - static_cast<i64>(first);
        if (row + static_cast<i64>(p.rows) <= 0 || row >= static_cast<i64>(v.rows)) continue;
        u32 image = 0;
        for (const auto& [pixels, id] : v.pics)
            if (pixels == p.rgba) image = id;
        if (!image) continue;
        PictureDraw& d = fd.pictures[fd.picture_count++];
        d = PictureDraw{};
        d.image = image;
        const float left = static_cast<float>(x + v.pad_x + static_cast<int>(p.col) * cw + static_cast<int>(p.off_x));
        const float top = static_cast<float>(y + v.pad_y + static_cast<int>(row) * ch + static_cast<int>(p.off_y));
        d.rect[0] = left, d.rect[1] = top;
        d.rect[2] = left + static_cast<float>(p.fit ? static_cast<int>(p.cols) * cw : static_cast<int>(p.w));
        d.rect[3] = top + static_cast<float>(p.fit ? static_cast<int>(p.rows) * ch : static_cast<int>(p.h));
        d.clip[0] = x, d.clip[1] = y, d.clip[2] = x + w, d.clip[3] = y + h;
        d.after = fd.popup_count;  // (over the terminal's box, under what is over that)
        d.clear = true;
    }
}

// ---------------------------------------------------------------- keys, text, the mouse

namespace {

// The clipboard: the system's, but a scripted run's own (the tests never touch the real one).
std::string g_scripted_clipboard;

}  // namespace

void App::term_copy(TermView& v) {
    if (!v.selected) return;
    std::string text;
    {
        std::lock_guard lock(v.mu);
        const Term& t = v.term;
        const u64 hist = t.history();
        for (u64 line = v.sel_line0; line <= v.sel_line1; ++line) {
            const TermCell* cells = line < hist ? t.history_line(line).data() : &t.at(static_cast<u32>(std::min<u64>(line - hist, t.rows() - 1)), 0);
            const u32 n = line < hist ? static_cast<u32>(t.history_line(line).size()) : t.cols();
            const u32 c0 = line == v.sel_line0 ? std::min(v.sel_col0, n) : 0, c1 = line == v.sel_line1 ? std::min(v.sel_col1, n) : n;
            if (line > v.sel_line0) text += '\n';
            if (c1 > c0) text += Term::line_text(cells + c0, c1 - c0);
        }
    }
    if (win_) SDL_SetClipboardText(text.c_str());
    else g_scripted_clipboard = text;
}

void App::term_paste(TermView& v) {
    std::string text;
    if (win_) {
        if (char* c = SDL_GetClipboardText()) {
            text = c;
            SDL_free(c);
        }
    } else {
        text = g_scripted_clipboard;
    }
    if (text.empty()) return;
    std::string out;
    for (const char c : text) out += c == '\n' ? '\r' : c;  // (a terminal sends Return)
    bool bracketed;
    {
        std::lock_guard lock(v.mu);
        bracketed = v.term.bracketed_paste();
    }
    if (bracketed) out = "\x1b[200~" + out + "\x1b[201~";
    v.scroll = 0;
    v.send(out);
}

bool App::term_key(const SDL_KeyboardEvent& k) {
    if (!pane_->doc->term) return false;
    TermView& v = *pane_->doc->term;
    const bool cmd = k.mod & kPrimaryMod, ctrl = k.mod & SDL_KMOD_CTRL, alt = k.mod & SDL_KMOD_ALT, shift = k.mod & SDL_KMOD_SHIFT;
    if (v.keep && k.key == SDLK_F8 && !ctrl && !alt && !(k.mod & SDL_KMOD_GUI)) return false;  // (a build's output: F8 is the editor's, its problems)
    // The editor's own shortcuts stay its (Cmd+W, Cmd+T, Cmd+1 ...), but for
    // copying, pasting, choosing all and clearing. Where Ctrl is the editor's
    // key those are Ctrl+Shift's, as in other terminals, and Ctrl alone is
    // the program's (Ctrl+F4 still closes the tab).
    if (kCtrlShared && ctrl && !shift && !alt && k.key == SDLK_F4) return false;
    if (kCtrlShared && ctrl && alt && (k.key == SDLK_LEFT || k.key == SDLK_RIGHT || k.key == SDLK_UP || k.key == SDLK_DOWN)) return false;  // (the splits)
    if (kCtrlShared ? ctrl && shift : cmd) {
        if (k.key == SDLK_C) return term_copy(v), true;
        if (k.key == SDLK_V) return term_paste(v), true;
        if (k.key == SDLK_A) {  // everything, the history too (then Cmd+C)
            std::lock_guard lock(v.mu);
            v.selected = true;
            v.sel_line0 = 0, v.sel_col0 = 0;
            v.sel_line1 = v.term.history() + v.term.rows() - 1, v.sel_col1 = v.term.cols();
            dirty_ = true;
            return true;
        }
        if (k.key == SDLK_K) {  // the history goes, and the shell draws its prompt again
            {
                std::lock_guard lock(v.mu);
                v.term.clear_history();
            }
            v.scroll = 0;
            v.send("\x0c");
            dirty_ = true;
            return true;
        }
        if (!kCtrlShared || (k.key != SDLK_PAGEUP && k.key != SDLK_PAGEDOWN)) return false;  // (Shift+Page Up there is still the history)
    }
    if (shift && (k.key == SDLK_PAGEUP || k.key == SDLK_PAGEDOWN)) {  // the history
        std::lock_guard lock(v.mu);
        const u64 page = std::max<u32>(1, v.term.rows() - 1);
        v.scroll = k.key == SDLK_PAGEUP ? std::min<u64>(v.scroll + page, v.term.history()) : v.scroll > page ? v.scroll - page : 0;
        dirty_ = true;
        return true;
    }
    bool app_keys;
    {
        std::lock_guard lock(v.mu);
        app_keys = v.term.app_cursor_keys();
    }
    // A key's modifiers, as xterm numbers them in "\e[1;<n>X".
    const int mods = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    auto cursor = [&](char c) {
        if (mods > 1) return std::string("\x1b[1;") + std::to_string(mods) + c;
        return std::string(app_keys ? "\x1bO" : "\x1b[") + c;
    };
    auto tilde = [&](int n) { return "\x1b[" + std::to_string(n) + (mods > 1 ? ";" + std::to_string(mods) : "") + "~"; };
    std::string s;
    switch (k.key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: s = alt ? "\x1b\r" : "\r"; break;
        case SDLK_BACKSPACE: s = ctrl ? "\x08" : alt ? "\x1b\x7f" : "\x7f"; break;
        case SDLK_TAB:
            if (ctrl) return false;  // (the editor's: the next tab)
            s = shift ? "\x1b[Z" : "\t";
            break;
        case SDLK_ESCAPE: s = "\x1b"; break;
        case SDLK_UP: s = cursor('A'); break;
        case SDLK_DOWN: s = cursor('B'); break;
        case SDLK_RIGHT: s = cursor('C'); break;
        case SDLK_LEFT: s = cursor('D'); break;
        case SDLK_HOME: s = cursor('H'); break;
        case SDLK_END: s = cursor('F'); break;
        case SDLK_INSERT: s = tilde(2); break;
        case SDLK_DELETE: s = tilde(3); break;
        case SDLK_PAGEUP: s = tilde(5); break;
        case SDLK_PAGEDOWN: s = tilde(6); break;
        case SDLK_F1: s = mods > 1 ? "\x1b[1;" + std::to_string(mods) + "P" : "\x1bOP"; break;
        case SDLK_F2: s = mods > 1 ? "\x1b[1;" + std::to_string(mods) + "Q" : "\x1bOQ"; break;
        case SDLK_F3: s = mods > 1 ? "\x1b[1;" + std::to_string(mods) + "R" : "\x1bOR"; break;
        case SDLK_F4: s = mods > 1 ? "\x1b[1;" + std::to_string(mods) + "S" : "\x1bOS"; break;
        case SDLK_F5: s = tilde(15); break;
        case SDLK_F6: s = tilde(17); break;
        case SDLK_F7: s = tilde(18); break;
        case SDLK_F8: s = tilde(19); break;
        case SDLK_F9: s = tilde(20); break;
        case SDLK_F10: s = tilde(21); break;
        case SDLK_F11: s = tilde(23); break;
        case SDLK_F12: s = tilde(24); break;
        default:
            if (ctrl && !(k.mod & SDL_KMOD_GUI)) {  // Ctrl+letter and the like: the control characters
                const SDL_Keycode key = k.key;
                if (key >= SDLK_A && key <= SDLK_Z) s = std::string(1, static_cast<char>(key - SDLK_A + 1));
                else if (key == SDLK_SPACE || key == SDLK_2) s = std::string(1, '\0');
                else if (key == SDLK_LEFTBRACKET || key == SDLK_3) s = "\x1b";
                else if (key == SDLK_BACKSLASH || key == SDLK_4) s = "\x1c";
                else if (key == SDLK_RIGHTBRACKET || key == SDLK_5) s = "\x1d";
                else if (key == SDLK_6) s = "\x1e";
                else if (key == SDLK_SLASH || key == SDLK_MINUS || key == SDLK_7) s = "\x1f";
                if (!s.empty() && alt) s = "\x1b" + s;
            } else if (alt && kPrimaryMod != SDL_KMOD_GUI && k.key >= SDLK_A && k.key <= SDLK_Z) {  // (Alt is Meta; on a Mac, Option types letters of its own)
                s = std::string("\x1b") + static_cast<char>(shift ? 'A' + (k.key - SDLK_A) : 'a' + (k.key - SDLK_A));
                term_drop_text_ = true;
            }
            break;
    }
    // Keys that type (their text comes as text) and the rest are the
    // terminal's too, so that Vim's keys and the editor's do not act on its
    // tab; but Ctrl+Tab still goes between tabs.
    if (s.empty()) return !(ctrl && k.key == SDLK_TAB);
    v.scroll = 0;
    v.selected = false;
    v.send(s);
    return true;
}

bool App::term_text(const char* text) {
    if (term_drop_text_) {  // (a key the terminal took as itself, or Ctrl+`: its text is not typed, wherever the keyboard went)
        term_drop_text_ = false;
        return true;
    }
    if (!pane_->doc->term) return false;
    TermView& v = *pane_->doc->term;
    v.scroll = 0;
    v.selected = false;
    v.send(text);
    return true;
}

namespace {

// What a terminal sends a program that asked for the mouse: a button
// (0-2 left, middle, right; 64 and 65 the wheel up and down) pressed,
// released or moved with, at a cell of the screen, in SGR's form if it
// asked for that, else in X10's (which cannot say where past column 223).
std::string mouse_report(const Term& t, int button, bool release, bool motion, u32 row, u32 col, SDL_Keymod mod) {
    int b = button + (motion ? 32 : 0) + (mod & SDL_KMOD_SHIFT ? 4 : 0) + (mod & SDL_KMOD_ALT ? 8 : 0) + (mod & SDL_KMOD_CTRL ? 16 : 0);
    if (t.mouse_mode() == 9) b = button;  // (X10's own: presses alone, no keys)
    char buf[48];
    if (t.mouse_sgr()) {
        std::snprintf(buf, sizeof buf, "\x1b[<%d;%u;%u%c", b, col + 1, row + 1, release ? 'm' : 'M');
        return buf;
    }
    if (release) b = (b & ~3) | 3;  // (X10's release does not say which)
    if (col > 222 || row > 222) return {};
    return {'\x1b', '[', 'M', static_cast<char>(32 + b), static_cast<char>(33 + col), static_cast<char>(33 + row)};
}

}  // namespace

// The screen cell under a point in a terminal's tab, if it shows the screen (not the history) and its program wants the mouse.
bool App::term_mouse_cell(TermView& v, int px, int py, u32* row, u32* col) {
    if (!v.cols || v.scroll != 0) return false;
    *col = static_cast<u32>(std::clamp((px - v.x - v.pad_x) / font_->cell_w(), 0, static_cast<int>(v.cols) - 1));
    *row = static_cast<u32>(std::clamp((py - v.y - v.pad_y) / font_->cell_h(), 0, static_cast<int>(v.rows) - 1));
    std::lock_guard lock(v.mu);
    return v.term.mouse_mode() != 0;
}

// The terminal tab in front of the pane under a point, and the cell there (line counted from the oldest in the history).
bool App::term_hit(int px, int py, Pane** pane, u64* line, u32* col) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->term) continue;
        TermView& v = *p->doc->term;
        if (px < v.x || py < v.y || px >= p->x1 || py >= p->y1 || !v.cols) continue;
        const int c = std::clamp((px - v.x - v.pad_x) / font_->cell_w(), 0, static_cast<int>(v.cols) - 1);
        const int r = std::clamp((py - v.y - v.pad_y) / font_->cell_h(), 0, static_cast<int>(v.rows) - 1);
        std::lock_guard lock(v.mu);
        *pane = p.get();
        *line = v.term.history() - v.scroll + static_cast<u64>(r);
        *col = static_cast<u32>(c);
        return true;
    }
    return false;
}

bool App::term_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    u64 line = 0;
    u32 col = 0;
    if (!term_hit(static_cast<int>(b.x * density_), static_cast<int>(b.y * density_), &pane, &line, &col)) return false;
    focus_pane(pane);
    TermView& v = *pane->doc->term;
    const SDL_Keymod mod = SDL_GetModState();
    if (b.button == SDL_BUTTON_LEFT && (mod & kPrimaryMod) && term_go_place(v, line, col)) return true;  // Cmd+click on a path:line
    // A program that asked for the mouse is told of it (with Shift, it is the terminal's own: choosing text).
    u32 mrow = 0, mcol = 0;
    const int button = b.button == SDL_BUTTON_LEFT ? 0 : b.button == SDL_BUTTON_MIDDLE ? 1 : b.button == SDL_BUTTON_RIGHT ? 2 : -1;
    if (button >= 0 && !(mod & SDL_KMOD_SHIFT) && term_mouse_cell(v, static_cast<int>(b.x * density_), static_cast<int>(b.y * density_), &mrow, &mcol)) {
        std::string s;
        {
            std::lock_guard lock(v.mu);
            s = mouse_report(v.term, button, false, false, mrow, mcol, mod);
        }
        v.mouse_button = button;
        v.mouse_row = mrow, v.mouse_col = mcol;
        v.selected = false;
        v.send(s);
        dirty_ = true;
        return true;
    }
    if (b.button == SDL_BUTTON_LEFT) {
        v.selecting = true;
        v.selected = false;
        v.sel_line0 = v.sel_line1 = v.anchor_line = line;
        v.sel_col0 = v.sel_col1 = v.anchor_col = col;
        if (b.clicks >= 2) {  // a word, or (three) the line
            std::lock_guard lock(v.mu);
            const Term& t = v.term;
            const u64 hist = t.history();
            const TermCell* cells = line < hist ? t.history_line(line).data() : &t.at(static_cast<u32>(std::min<u64>(line - hist, t.rows() - 1)), 0);
            const u32 n = line < hist ? static_cast<u32>(t.history_line(line).size()) : t.cols();
            if (b.clicks >= 3) {
                v.sel_col0 = 0, v.sel_col1 = n;
            } else {
                auto word = [&](u32 c) { const u32 cp = cells[c].cp; return cp > ' ' && !std::strchr("\"'`()[]{}<>,;|", static_cast<int>(cp < 0x80 ? cp : 'a')); };
                u32 a = std::min(col, n ? n - 1 : 0), e = a;
                while (a > 0 && word(a - 1)) --a;
                while (e < n && word(e)) ++e;
                v.sel_col0 = a, v.sel_col1 = std::max(e, a + 1);
            }
            v.selected = true;
            v.selecting = false;
        }
    } else if (b.button == SDL_BUTTON_MIDDLE || b.button == SDL_BUTTON_RIGHT) {
        term_paste(v);
    }
    dirty_ = true;
    return true;
}

// Cmd+click: a "path:line[:col]" (or Python's File "path", line N) where
// the click was opens there, in a split with text. A relative path is from
// the shell's folder (as it tells it), a folder the build said it went
// into, or the folder the tab started in: the first it is in.
bool App::term_go_place(TermView& v, u64 line, u32 col) {
    std::string text, cwd;
    size_t at = std::string::npos;
    std::vector<std::string> dirs;
    {
        std::lock_guard lock(v.mu);
        const Term& t = v.term;
        const u64 hist = t.history(), end = hist + t.rows();
        if (line >= end) return false;
        auto row = [&](u64 l, u32* n) {
            *n = l < hist ? static_cast<u32>(t.history_line(l).size()) : t.cols();
            return l < hist ? t.history_line(l).data() : &t.at(static_cast<u32>(l - hist), 0);
        };
        auto wrapped = [&](u64 l) {  // (it goes on into the next row)
            u32 n = 0;
            const TermCell* cells = row(l, &n);
            return n > 0 && (cells[n - 1].attr & TermCell::kWrapped);
        };
        // The whole line, from the row it starts on to the one it ends on
        // (a long one wraps), and the byte the clicked cell is.
        u64 first = line, last = line;
        while (first > 0 && line - first < 64 && wrapped(first - 1)) --first;
        while (last + 1 < end && last - line < 64 && wrapped(last)) ++last;
        for (u64 l = first; l <= last; ++l) {
            u32 n = 0;
            const TermCell* cells = row(l, &n);
            if (l < last) n = std::min(n, t.cols());
            for (u32 c = 0; c < n; ++c) {
                if (l == line && c == col) at = text.size();
                if (cells[c].attr & TermCell::kWideTail) continue;
                if (cells[c].cp <= ' ') text += ' ';  // (line_text drops a blank at the end: one cell alone would be nothing)
                else text += Term::line_text(cells + c, 1);
            }
        }
        cwd = t.cwd();
    }
    TextPlace place;
    if (at == std::string::npos || !place_in_text(text, at, &place)) return false;

    // The machine it is on: here, or "ssh://host" for one there.
    std::string prefix, there;
    std::string host;
    if (split_remote(v.dir, &host, &there)) prefix = v.dir.substr(0, v.dir.size() - there.size());
    if (!cwd.empty()) dirs.push_back(prefix + cwd);
    {
        std::lock_guard lock(v.scan_mu);
        for (size_t i = v.scan_dirs.size(); i-- > 0;) dirs.push_back(v.scan_dirs[i]);
    }
    dirs.push_back(v.dir);
    std::string path;
    if (place.path.starts_with('/')) {
        path = prefix + tidy_path(place.path);
    } else {
        for (const std::string& d : dirs) {
            const std::string full = prefix + tidy_path(d.substr(std::min(prefix.size(), d.size())) + "/" + place.path);
            if (!prefix.empty() || SDL_GetPathInfo(full.c_str(), nullptr)) {
                path = full;
                break;
            }
        }
    }
    if (path.empty() || (prefix.empty() && !SDL_GetPathInfo(path.c_str(), nullptr))) return false;  // (not a file: the click is a click)
    go_to_place(path, place.line, place.col);
    return true;
}

bool App::term_mouse_motion(const SDL_MouseMotionEvent& m) {
    // A program that asked to be told of moves: with a button held (1002), or any (1003), as the mouse comes to another cell.
    const int px = static_cast<int>(m.x * density_), py = static_cast<int>(m.y * density_);
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->term) continue;
        TermView& v = *p->doc->term;
        const bool held = v.mouse_button >= 0;
        if (!held && (px < v.x || py < v.y || px >= p->x1 || py >= p->y1)) continue;
        u32 row = 0, col = 0;
        if (!term_mouse_cell(v, px, py, &row, &col)) continue;
        std::string s;
        {
            std::lock_guard lock(v.mu);
            const u32 mode = v.term.mouse_mode();
            if ((mode == 1002 && held) || mode == 1003) {
                if (row == v.mouse_row && col == v.mouse_col) return true;
                s = mouse_report(v.term, held ? v.mouse_button : 3, false, true, row, col, SDL_GetModState());
            }
        }
        if (s.empty()) {
            if (held) return true;  // (a press it was told of: the drag is not choosing text)
            continue;
        }
        v.mouse_row = row, v.mouse_col = col;
        v.send(s);
        return true;
    }
    if (!(m.state & SDL_BUTTON_LMASK)) return false;
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->term || !p->doc->term->selecting) continue;
        TermView& v = *p->doc->term;
        Pane* over = nullptr;
        u64 line = 0;
        u32 col = 0;
        if (!term_hit(static_cast<int>(m.x * density_), static_cast<int>(m.y * density_), &over, &line, &col) || over != p.get()) return true;
        // From where the press was to here, whichever way round.
        v.selected = true;
        const bool before = line < v.anchor_line || (line == v.anchor_line && col < v.anchor_col);
        if (before) v.sel_line0 = line, v.sel_col0 = col, v.sel_line1 = v.anchor_line, v.sel_col1 = v.anchor_col;
        else v.sel_line0 = v.anchor_line, v.sel_col0 = v.anchor_col, v.sel_line1 = line, v.sel_col1 = col + 1;
        dirty_ = true;
        return true;
    }
    return false;
}

void App::term_mouse_up() {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->term) continue;
        TermView& v = *p->doc->term;
        v.selecting = false;
        if (v.mouse_button < 0) continue;
        // The button let go of, where the mouse is now (on the screen, as the press was).
        const int button = std::exchange(v.mouse_button, -1);
        u32 row = v.mouse_row, col = v.mouse_col;
        term_mouse_cell(v, static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_), &row, &col);
        std::string s;
        {
            std::lock_guard lock(v.mu);
            const u32 mode = v.term.mouse_mode();
            if (mode && mode != 9) s = mouse_report(v.term, button, true, false, row, col, SDL_GetModState());
        }
        if (!s.empty()) v.send(s);
    }
}

bool App::term_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    u64 line = 0;
    u32 col = 0;
    if (!term_hit(static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_), &pane, &line, &col)) return false;
    TermView& v = *pane->doc->term;
    // A program that asked for the mouse: the wheel's notches, as buttons 64 (up) and 65 (down).
    u32 mrow = 0, mcol = 0;
    if (term_mouse_cell(v, static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_), &mrow, &mcol)) {
        v.wheel_left += w.y;
        std::string s;
        {
            std::lock_guard lock(v.mu);
            for (; v.wheel_left >= 1 || v.wheel_left <= -1; v.wheel_left -= v.wheel_left > 0 ? 1.0f : -1.0f)
                s += mouse_report(v.term, v.wheel_left > 0 ? 64 : 65, false, false, mrow, mcol, SDL_GetModState());
        }
        if (!s.empty()) v.send(s);
        return true;
    }
    const int n = static_cast<int>(std::lround(w.y * 3));
    if (n == 0) return true;
    bool alt, app_keys;
    u64 hist;
    {
        std::lock_guard lock(v.mu);
        alt = v.term.alt_screen();
        app_keys = v.term.app_cursor_keys();
        hist = v.term.history();
    }
    if (alt) {  // a full-screen program (less, an editor): the wheel is its arrows
        std::string s;
        for (int k = 0; k < std::abs(n); ++k) s += std::string(app_keys ? "\x1bO" : "\x1b[") + (n > 0 ? 'A' : 'B');
        v.send(s);
    } else {
        v.scroll = static_cast<u64>(std::clamp<i64>(static_cast<i64>(v.scroll) + n, 0, static_cast<i64>(hist)));
    }
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- for the tests

// The screen of the terminal in front, its non-empty rows joined by "|", and the cursor.
std::string App::term_dump() {
    if (!pane_->doc->term) return "no terminal";
    TermView& v = *pane_->doc->term;
    std::lock_guard lock(v.mu);
    std::string out;
    for (u32 r = 0; r < v.term.rows(); ++r) {
        const std::string t = v.term.row_text(r);
        if (!t.empty()) out += (out.empty() ? "" : "|") + t;
    }
    out += " @" + std::to_string(v.term.cursor_row()) + "," + std::to_string(v.term.cursor_col()) + " " + std::to_string(v.term.rows()) + "x" +
           std::to_string(v.term.cols()) + (v.scroll ? " back " + std::to_string(v.scroll) : "") + (v.ended ? " ended " + std::to_string(v.status.load()) : "");
    // Its pictures: each one's size, and its row (on the screen: less than 0, in the history) and column.
    for (const TermPicture& p : v.term.pictures()) {
        const i64 row = p.alt ? static_cast<i64>(p.line) : static_cast<i64>(p.line - v.term.lines_gone()) - static_cast<i64>(v.term.history());
        out += " picture " + std::to_string(p.w) + "x" + std::to_string(p.h) + "@" + std::to_string(row) + "," + std::to_string(p.col);
    }
    return out;
}

// Waits (10 s at most) until the program of the terminal in front has ended.
void App::term_end() {
    const u64 until = now_ns() + 10'000'000'000ull;
    while (pane_->doc->term && !pane_->doc->term->ended && now_ns() < until) SDL_Delay(5);
    term_sync();
}

// Cmd+click on the first place this text shows in a terminal on show (in
// front in its split), its first letter, as a real click would be: on the
// screen, else in the history, scrolled to the top of the view.
bool App::term_click_text(const std::string& text) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->term) continue;
        TermView& v = *p->doc->term;
        u32 row = 0, col = 0;
        bool found = false;
        {
            std::lock_guard lock(v.mu);
            v.scroll = 0;
            for (u32 r = 0; r < v.term.rows() && !found; ++r) {
                const std::string t = v.term.row_text(r);
                if (const size_t at = t.find(text); at != std::string::npos) row = r, col = static_cast<u32>(at), found = true;  // (ASCII before it: a byte a column)
            }
            const size_t hist = v.term.history();
            for (size_t i = 0; i < hist && !found; ++i) {
                const std::vector<TermCell>& l = v.term.history_line(i);
                const std::string t = Term::line_text(l.data(), static_cast<u32>(l.size()));
                if (const size_t at = t.find(text); at != std::string::npos) row = 0, col = static_cast<u32>(at), found = true, v.scroll = hist - i;
            }
        }
        if (!found || !v.cols) continue;
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.clicks = 1;
        e.button.x = static_cast<float>(v.x + v.pad_x + static_cast<int>(col) * font_->cell_w() + font_->cell_w() / 2) / density_;
        e.button.y = static_cast<float>(v.y + v.pad_y + static_cast<int>(row) * font_->cell_h() + font_->cell_h() / 2) / density_;
        const SDL_Keymod was = SDL_GetModState();
        SDL_SetModState(kPrimaryMod);
        handle(e);
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        handle(e);
        SDL_SetModState(was);
        return true;
    }
    return false;
}

// Waits (10 s at most) until the screen of the terminal in front shows this text.
bool App::term_wait(const std::string& text) {
    if (!pane_->doc->term) return false;
    const u64 until = now_ns() + 10'000'000'000ull;
    TermView& v = *pane_->doc->term;
    while (now_ns() < until) {
        {
            std::lock_guard lock(v.mu);
            for (u32 r = 0; r < v.term.rows(); ++r)
                if (v.term.row_text(r).find(text) != std::string::npos) return true;
        }
        SDL_Delay(5);
    }
    return false;
}

}  // namespace blit
