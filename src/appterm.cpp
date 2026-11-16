// Terminals in the editor: a tab running a shell, here or on another
// machine through blit's agent, which moves among tabs and splits as a
// file's does (Ctrl+` opens one in the folder being worked in). Its
// program runs under a pseudo-terminal (pty.hpp); what it writes is read
// on a thread of its own and drawn by term.hpp's screen; keys and text go
// to it as a terminal sends them.
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "app.hpp"
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
    std::atomic<int> status{0};
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
        if (pty.pid > 0) {
            int st = 0;
            if (::waitpid(pty.pid, &st, WNOHANG) == 0) {
                ::kill(pty.pid, SIGKILL);
                ::waitpid(pty.pid, &st, 0);
            }
        }
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

    // What the program writes, onto the screen; what the screen answers, back to it.
    void read_loop(std::stop_token stop) {
        if (!remote_dir.empty() && !start_remote(stop)) {
            status = 1;
            ended = true;
            wake();
            return;
        }
        char buf[65536];
        bool first = true;
        while (!stop.stop_requested()) {
            pollfd p{out, POLLIN, 0};
            const int r = ::poll(&p, 1, 100);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) continue;
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
                if (!told.exchange(true)) wake();
            } else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
                break;  // (it has ended: a pseudo-terminal says EIO, a pipe its end)
            }
        }
        if (stop.stop_requested()) return;
        int st = 0;
        if (conn) {
            st = conn->wait(proc.id);
        } else if (pty.pid > 0) {
            while (::waitpid(pty.pid, &st, 0) < 0 && errno == EINTR) {}
            st = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        }
        status = st;
        ended = true;
        wake();
    }

    // Bytes to the program (a key, a paste), all of them even if it is slow to take them.
    void send(std::string_view s) {
        const int fd = in;
        if (fd < 0 || ended) return;
        while (!s.empty()) {
            const ssize_t k = ::write(fd, s.data(), s.size());
            if (k > 0) {
                s.remove_prefix(static_cast<size_t>(k));
            } else if (k < 0 && (errno == EAGAIN || errno == EINTR)) {
                pollfd p{fd, POLLOUT, 0};
                if (::poll(&p, 1, 1000) <= 0) return;
            } else {
                return;
            }
        }
    }
};

// ---------------------------------------------------------------- opening one

// A terminal in a new tab, in `dir` (the folder being worked in when none
// is given): a shell there, or on the machine that folder is on.
void App::term_open(const std::string& dir_given) {
    const std::string* root = panel_root();
    const std::string dir = !dir_given.empty() ? dir_given : root ? *root : default_workdir();
    auto v = std::make_shared<TermView>();
    std::string host, there;
    const bool remote = split_remote(dir, &host, &there);
    // A login shell, the user's own (there: the one ssh would start), with
    // UTF-8 text when nothing says which (a program started from the Dock
    // has no LANG).
    std::vector<std::string> argv = {"/bin/sh", "-c",
                                     remote ? "[ -z \"$LANG$LC_ALL$LC_CTYPE\" ] && LANG=C.UTF-8 && export LANG; exec \"${SHELL:-/bin/sh}\" -l"
                                            : "[ -z \"$LANG$LC_ALL$LC_CTYPE\" ] && LANG=en_US.UTF-8 && export LANG; exec \"${SHELL:-/bin/sh}\" -l"};
#if BLIT_DEV
    if (const char* sh = SDL_getenv("BLIT_TERM_SHELL"); sh && *sh) argv = {"/bin/sh", "-c", std::string(argv[2].substr(0, argv[2].find("exec"))) + sh};  // (tests: a shell that is the same everywhere)
#endif
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
            return;
        }
        v->in = v->out = v->pty.fd;
    }
    {
        std::lock_guard lock(v->mu);
        v->term.resize(cols, rows);
    }
    TermView* raw = v.get();
    v->reader = std::jthread([raw](std::stop_token stop) { raw->read_loop(stop); });
    Doc* d = add_doc(pane_->tab + 1);
    d->title = remote ? "Terminal: " + host : "Terminal";
    d->read_only = true;  // (its text is the program's: saving it is no saving)
    d->term = v;
    tab_show(pane_->tab + 1);
    panel_focus(false);
    dirty_ = true;
}

// The program has written something, or ended: its tab draws again; one
// whose shell has ended goes (or says how it ended, if that was not well).
void App::term_sync() {
    for (const std::unique_ptr<Pane>& p : panes_) {
        for (size_t i = 0; i < p->docs.size(); ++i) {
            Doc& d = *p->docs[i];
            if (!d.term) continue;
            TermView& v = *d.term;
            v.told = false;
            // The tab's name: the program's title for itself, else the folder it is in.
            std::string title;
            {
                std::lock_guard lock(v.mu);
                title = !v.term.title().empty() ? v.term.title() : v.term.cwd();
            }
            if (!title.empty()) {
                if (title.size() > 60) title = "\xE2\x80\xA6" + title.substr(title.size() - 59);
                const std::string named = v.host.empty() ? title : title + " (" + v.host + ")";
                if (d.title != named) d.title = named;
            }
            if (v.ended && !d.term_closed) {
                d.term_closed = true;
                if (v.status == 0) {  // (exit: the tab goes, as a terminal window would)
                    Pane* const focused = pane_;
                    pane_ = p.get();
                    tab_close(i, true);
                    bool still = false;
                    for (const std::unique_ptr<Pane>& q : panes_) still |= q.get() == focused;
                    pane_ = still ? focused : panes_.front().get();
                    dirty_ = true;
                    return term_sync();  // (the tabs moved: from the start)
                }
                std::lock_guard lock(v.mu);
                v.term.feed("\r\n\x1b[2m[ended with " + std::to_string(v.status.load()) + "]\x1b[0m");
            }
        }
    }
    dirty_ = true;
}

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
        if (t.cols() != cols || t.rows() != rows) {  // the tab's size: the program is told
            t.resize(cols, rows);
            if (v.conn) v.conn->resize(v.proc.id, rows, cols);
            else if (v.pty.fd >= 0) pty_resize(v.pty.fd, rows, cols);
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
                    const u32 style = (tc.attr & TermCell::kBold ? Font::kBold : 0) | (tc.attr & TermCell::kItalic ? Font::kItalic : 0);
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
    // The editor's own shortcuts stay its (Cmd+W, Cmd+T, Cmd+1 ...), but for copying and pasting.
    if (cmd && kPrimaryMod == SDL_KMOD_GUI) {
        if (k.key == SDLK_C) return term_copy(v), true;
        if (k.key == SDLK_V) return term_paste(v), true;
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
        return false;
    }
    if (ctrl && shift && (k.key == SDLK_C || k.key == SDLK_V)) {  // (where Ctrl is the editor's key)
        if (k.key == SDLK_C) term_copy(v);
        else term_paste(v);
        return true;
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
            if (ctrl && !cmd) {  // Ctrl+letter and the like: the control characters
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
    if (!pane_->doc->term) return false;
    if (term_drop_text_) {
        term_drop_text_ = false;
        return true;
    }
    TermView& v = *pane_->doc->term;
    v.scroll = 0;
    v.selected = false;
    v.send(text);
    return true;
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

bool App::term_mouse_motion(const SDL_MouseMotionEvent& m) {
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
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->doc && p->doc->term) p->doc->term->selecting = false;
}

bool App::term_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    u64 line = 0;
    u32 col = 0;
    if (!term_hit(static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_), &pane, &line, &col)) return false;
    TermView& v = *pane->doc->term;
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
    return out + " @" + std::to_string(v.term.cursor_row()) + "," + std::to_string(v.term.cursor_col()) + " " + std::to_string(v.term.rows()) + "x" +
           std::to_string(v.term.cols()) + (v.scroll ? " back " + std::to_string(v.scroll) : "") + (v.ended ? " ended " + std::to_string(v.status.load()) : "");
}

// Waits (10 s at most) until the program of the terminal in front has ended.
void App::term_end() {
    const u64 until = now_ns() + 10'000'000'000ull;
    while (pane_->doc->term && !pane_->doc->term->ended && now_ns() < until) SDL_Delay(5);
    term_sync();
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
