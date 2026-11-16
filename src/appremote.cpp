// Files on other machines, in the editor (remote.hpp has the connections).
// Opening one makes its tab at once, read only and empty; a thread here
// waits for the connection, reads the file, and hands its bytes to the main
// thread, which fills the tab in. Nothing on the main thread waits for the
// network but saving, which is one round trip on a connection that is up.
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

#include "app.hpp"
#include "qos.hpp"
#include "remote.hpp"

namespace blit {

struct App::RemoteUi {
    struct Job {
        u64 doc = 0;
        std::string path;
        bool reload = false;  // only if it has changed there since known
        i64 known = 0;
    };
    struct Done {
        u64 doc = 0;
        std::string path;
        int status = 0;  // as Remote::read; 6: a folder
        std::string bytes, error;
        i64 modified = 0;
        bool reload = false;
    };
    std::map<std::string, Remote::State> seen;  // (main thread) each connection as it was last time
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<Job> jobs;
    std::vector<Done> done;
    size_t busy = 0;  // jobs taken and not done
    std::jthread worker;  // (last: stopped first)

    ~RemoteUi() {
        worker.request_stop();
        Remote::close_all();  // (a call it is waiting on fails at once)
    }
};

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 12;
    SDL_PushEvent(&ev);
}

}  // namespace

void App::remote_start() {
#if defined(BLIT_NO_REMOTE)
    return;
#endif
    Remote::set_notify(wake);  // a connection is up, or has failed (or ssh has a question)
    Remote::set_askpass(program_path());  // (ssh's questions come here: see remote_asks)
    remote_ = std::make_shared<RemoteUi>();
    RemoteUi* u = remote_.get();
    u->worker = std::jthread([u](std::stop_token stop) {
        thread_soon(Soon::kWaitedFor);
        for (;;) {
            RemoteUi::Job j;
            {
                std::unique_lock lock(u->mu);
                if (!u->cv.wait(lock, stop, [&] { return !u->jobs.empty(); })) return;
                j = std::move(u->jobs.front());
                u->jobs.pop_front();
                ++u->busy;
            }
            RemoteUi::Done d{j.doc, j.path, 0, {}, {}, 0, j.reload};
            std::string there;
            bool keep = true;
            if (const std::shared_ptr<Remote> r = remote_for(j.path, &there, kRemoteWaitMs); !r) {
                d.status = 5;
                d.error = remote_error(j.path);
                keep = !j.reload;  // (a file looked at again says nothing when there is no connection)
            } else if (RemoteStat st; j.reload && (!r->stat(there, &st) || st.kind != 1 || st.modified_ns == j.known)) {
                keep = false;  // the same as when it was read
            } else {
                constexpr u64 kMost = (1ull << 30) - 4096;  // (the connection drops a frame over 1 GiB: the reply, with its header, must fit)
                d.status = r->read(there, kMost, &d.bytes, &d.modified);
                if (d.status == 2 && r->stat(there, &st) && st.kind == 2) d.status = 6;
            }
            {
                std::lock_guard lock(u->mu);
                --u->busy;
                if (keep) u->done.push_back(std::move(d));
            }
            wake();
        }
    });
}

void App::remote_load(const Doc& d, bool reload) {
    if (!remote_) return;
    {
        std::lock_guard lock(remote_->mu);
        for (const RemoteUi::Job& j : remote_->jobs)
            if (j.doc == d.id) return;  // (asked for already)
        remote_->jobs.push_back({d.id, d.ed.path, reload, d.disk_stamp});
    }
    remote_->cv.notify_one();
}

bool App::remote_busy() const {
    if (!remote_) return false;
    std::lock_guard lock(remote_->mu);
    return !remote_->jobs.empty() || remote_->busy || !remote_->done.empty();
}

// A file from another machine is in its tab: what opening a file starts.
void App::remote_opened(Pane& p, Doc& d, bool fresh) {
    d.loading = d.read_only = false;
    Pane* const focused = pane_;
    Doc* const front = p.doc;
    pane_ = &p;
    p.doc = &d;
    if (d.goto_line != ~0ull) {  // a line it was opened to go to (a search's)
        Editor& ed = d.ed;
        const u64 line = std::min<u64>(d.goto_line, ed.buf.line_count() - 1), start = ed.buf.line_start(line);
        ed.move_to(std::min(start + d.goto_col, ed.line_end(line)), false);
        d.goto_line = ~0ull;
        if (&d == front) {
            if (pane_ == focused) vim_mouse();
            const double ch = font_->cell_h();
            p.target_y = std::clamp(static_cast<double>(line) * ch - (view_h() - ch) / 2, 0.0, std::max(max_scroll_y(), 0.0));
        } else {
            d.scroll_y = std::max(0.0, static_cast<double>(line) * font_->cell_h() - view_h() / 2);
        }
    }
    start_syntax(static_cast<u64>(std::max(0.0, &d == front ? p.target_y : d.scroll_y)) / static_cast<u64>(font_->cell_h()));
    p.doc = front;
    pane_ = focused;
    p.cache_rev = ~0ull;
    if (&d == pane_->doc) {
        lsp_pending_ = true;
        grammars_pending_ = true;
    }
    if (!session_arrived(p, d) && !fresh) recent_add(d.ed.path, false);  // (one a session opened: its view as it was left, and not opened by hand)
    if (script_) script_opened(d.id, fresh);
}

// The connections that failed are made again, and what was asked of them is asked again.
void App::remote_reconnect() {
    std::vector<std::string> hosts;
    for (const std::shared_ptr<Remote>& r : Remote::all())
        if (r->state() == Remote::kFailed) hosts.push_back(r->host());
    for (const std::string& h : hosts) Remote::reconnect(h);
    remote_sync();
}

// ssh's questions: the oldest one waiting is in the box; one that is no
// longer waiting (its connection went) leaves it.
void App::remote_asks() {
    const std::vector<RemoteAsk> asks = Remote::asks();
    if (const u64 showing = find_ask_id()) {
        for (const RemoteAsk& a : asks)
            if (a.id == showing) return;
        find_drop_ask();
        return;
    }
    if (!asks.empty()) find_ask(asks[0].id, asks[0].host, asks[0].prompt, asks[0].secret);
}

void App::remote_sync() {
    ++remote_gen_;  // (the status bar says how the connections are)
    dirty_ = true;
    remote_asks();
    find_remote_ready();
    if (!remote_) return;
    // A connection that has come up (again): the files, git and the language
    // server there are asked again for what they could not give without it.
    bool up = false;
    for (const std::shared_ptr<Remote>& r : Remote::all()) {
        const Remote::State st = r->state();
        Remote::State& was = remote_->seen[r->host()];
        if (st == Remote::kReady && was != Remote::kReady) {
            up = true;
            lsp_forget_failed("@" + r->host());
        }
        was = st;
    }
    if (up) {
        panel_refresh();
        if (cfg_.git) git_.refresh();
        lsp_pending_ = true;
    }
    std::vector<RemoteUi::Done> done;
    {
        std::lock_guard lock(remote_->mu);
        done.swap(remote_->done);
    }
    for (RemoteUi::Done& x : done) {
        Pane* where = nullptr;
        size_t at = 0;
        Doc* d = find_doc(x.doc, &where, &at);
        if (!d || d->ed.path != x.path) continue;  // closed, or another file now
        if (x.reload) {  // it changed there
            if (x.status != 0 || d->loading || d->read_only) continue;
            d->disk_stamp = x.modified;
            if (!d->ed.modified()) reread(*where, *d, &x.bytes);
            continue;
        }
        if (!d->loading) continue;
        if (x.status != 0 && session_dropped(x.doc, x.status == 5 ? x.error : x.status == 1 || x.status == 6 ? std::string() : "cannot read " + x.path)) {
            Pane* const focused = pane_;  // (a session's: it goes quietly if it is no longer there)
            pane_ = where;
            tab_close(at, true);
            pane_ = focused;
            continue;
        }
        switch (x.status) {
            case 0:
                d->ed.open_bytes(x.path, x.bytes);
                d->disk_stamp = x.modified;
                remote_opened(*where, *d, false);
                break;
            case 1:  // not there: a new file, made when it is saved
                d->disk_stamp = 0;
                remote_opened(*where, *d, true);
                break;
            default: {
                // A folder is opened as one (blit ssh://host/folder); anything else that
                // cannot be read says why, and its tab goes.
                Pane* const focused = pane_;
                pane_ = where;
                tab_close(at, true);
                pane_ = focused;
                if (x.status == 6) {
                    if (!project_open(x.path) && !panel_px()) panel_toggle();  // (the folder, shown; its session, if it has one)
                    break;
                }
                const std::string why = x.status == 5 ? x.error
                                        : x.status == 2 ? "not a file: " + x.path
                                        : x.status == 3 ? "too big to open from another machine: " + x.path
                                                        : "cannot read " + x.path;
                tell(SDL_MESSAGEBOX_ERROR, why.c_str());
                break;
            }
        }
    }
    pick_theme();
}

}  // namespace blit
