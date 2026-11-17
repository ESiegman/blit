// Files on other machines, in the editor (remote.hpp has the connections).
// Opening one makes its tab at once, read only and empty; a thread here
// waits for the connection, reads the file, and hands its bytes to the main
// thread, which fills the tab in. Saving one takes the text as it is (the
// pieces it is made of, which never change) and a thread of its own writes
// it there while typing goes on; the tab says "saving…" until it has been
// written. A folder there for the box that opens files is listed on a thread
// too. Nothing on the main thread waits for the network.
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

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
        i64 known = 0;  // (a reload: the time of the file it was asked against)
    };
    std::map<std::string, Remote::State> seen;  // (main thread) each connection as it was last time
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<Job> jobs;
    std::vector<Done> done;
    size_t busy = 0;  // jobs taken and not done

    // Saving. A save holds the pieces of the text as it was (their bytes are
    // the document's, and stay as they are while it is open); the saver copies
    // them out before it writes, and a document about to go waits for that
    // copy, or has a save still waiting its turn take its own (remote_forget).
    struct Save {
        u64 doc = 0;
        std::string path;
        std::vector<Piece> pieces;
        std::string text;  // (the copy, once there is one)
        u64 point = 0;     // where the document's history was (Editor::save_point)
    };
    struct Saved {
        u64 doc = 0;
        std::string path;
        u64 point = 0;
        bool ok = false;
        std::string error;
        i64 modified = 0;
    };
    std::deque<Save> saves;    // waiting their turn
    std::vector<Saved> saved;  // written (or not), for the main thread
    u64 copying = 0;           // the document whose text the saver is copying now
    std::condition_variable_any copied;
    // (Main thread.) The documents with saves under way: how many, and the
    // revision of the text the last of them took.
    struct Saving {
        u64 doc = 0;
        int count = 0;
        u64 revision = 0;
        std::string path;
    };
    std::vector<Saving> saving;
    // What waits for saves under way, with the box that says so open: a tab
    // to close (doc), or quitting (asking again about what is not saved, or not).
    enum : u8 { kHoldNone, kHoldClose, kHoldQuit, kHoldQuitNow };
    u8 hold = kHoldNone;
    u64 hold_doc = 0;
    u64 unheld_doc = 0;  // "Close without waiting": its tab closes now (once)

    // Listing a folder for the box that opens files: the one to list next,
    // the one being listed, and the answer for the main thread.
    struct Listing {
        std::string dir;
        bool connected = true;  // (false: no connection; it is asked again once one comes up)
        std::vector<RemoteEntry> entries;
    };
    std::string list_wanted, list_doing;
    std::optional<Listing> listed;
    std::string list_failed;  // (main thread) one that had no connection

    std::jthread worker, saver, lister;  // (last: stopped first)

    ~RemoteUi() {
        worker.request_stop();
        saver.request_stop();
        lister.request_stop();
        Remote::close_all();  // (a call they are waiting on fails at once)
    }

    Saving* saving_of(u64 doc) {
        for (Saving& x : saving)
            if (x.doc == doc) return &x;
        return nullptr;
    }
};

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 12;
    SDL_PushEvent(&ev);
}

std::string copy_of(const std::vector<Piece>& pieces) {
    size_t n = 0;
    for (const Piece& p : pieces) n += p.len;
    std::string out;
    out.resize_and_overwrite(n, [&](char* at, size_t) {
        for (const Piece& p : pieces) {
            std::memcpy(at, p.p, p.len);
            at += p.len;
        }
        return n;
    });
    return out;
}

std::string name_of(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

// The connection for a path, waited for as a worker may, but not past a
// request to stop (quitting does not wait for a host that is still coming up).
std::shared_ptr<Remote> connection(const std::string& path, std::string* there, std::stop_token stop) {
    std::shared_ptr<Remote> r;
    for (int waited = 0; !r && waited < kRemoteWaitMs && !stop.stop_requested(); waited += 200) r = remote_for(path, there, 200);
    return r;
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
            d.known = j.known;
            {
                std::lock_guard lock(u->mu);
                --u->busy;
                if (keep) u->done.push_back(std::move(d));
            }
            wake();
        }
    });
    // Saves, in the order they were asked for.
    u->saver = std::jthread([u](std::stop_token stop) {
        thread_soon(Soon::kWaitedFor);
        for (;;) {
            RemoteUi::Save s;
            {
                std::unique_lock lock(u->mu);
                if (!u->cv.wait(lock, stop, [&] { return !u->saves.empty(); })) return;
                s = std::move(u->saves.front());
                u->saves.pop_front();
                if (!s.pieces.empty()) u->copying = s.doc;
            }
            if (!s.pieces.empty()) {  // (memory speed: the document may go once this is done)
                s.text = copy_of(s.pieces);
                s.pieces = {};
                {
                    std::lock_guard lock(u->mu);
                    u->copying = 0;
                }
                u->copied.notify_all();
            }
            RemoteUi::Saved d{s.doc, s.path, s.point, false, {}, 0};
            std::string there;
            if (const std::shared_ptr<Remote> r = connection(s.path, &there, stop); !r) d.error = remote_error(s.path);
            else d.ok = r->write(there, std::move(s.text), &d.error, &d.modified);
            {
                std::lock_guard lock(u->mu);
                u->saved.push_back(std::move(d));
            }
            wake();
        }
    });
    // Folders listed for the box that opens files: only the last one asked
    // for matters (what is typed moves on).
    u->lister = std::jthread([u](std::stop_token stop) {
        thread_soon(Soon::kWaitedFor);
        for (;;) {
            RemoteUi::Listing l;
            {
                std::unique_lock lock(u->mu);
                if (!u->cv.wait(lock, stop, [&] { return !u->list_wanted.empty(); })) return;
                l.dir = u->list_doing = std::exchange(u->list_wanted, {});
            }
            std::string there;
            if (const std::shared_ptr<Remote> r = connection(l.dir, &there, stop); !r) l.connected = false;
            else if (!r->list(there, &l.entries)) l.entries.clear(), l.connected = r->state() == Remote::kReady;
            {
                std::lock_guard lock(u->mu);
                u->list_doing.clear();
                u->listed = std::move(l);
            }
            wake();
        }
    });
}

void App::remote_load(const Doc& d, bool reload) {
    if (!remote_) return;
    if (reload && remote_saving(d.id)) return;  // (being written there: what is there is what it is writing)
    {
        std::lock_guard lock(remote_->mu);
        for (const RemoteUi::Job& j : remote_->jobs)
            if (j.doc == d.id) return;  // (asked for already)
        remote_->jobs.push_back({d.id, d.ed.path, reload, d.disk_stamp});
    }
    remote_->cv.notify_all();  // (the saver and the lister wait on it too)
}

bool App::remote_busy() const {
    if (!remote_) return false;
    std::lock_guard lock(remote_->mu);
    return !remote_->jobs.empty() || remote_->busy || !remote_->done.empty() || !remote_->saving.empty() || !remote_->saved.empty() ||
           !remote_->list_wanted.empty() || !remote_->list_doing.empty();
}

// ---------------------------------------------------------------- saving

// The text as it is now, written there on the saver's thread. A save of the
// same document still waiting its turn gives way to this one.
void App::remote_save(Doc& d) {
    if (!remote_) return;
    RemoteUi& u = *remote_;
    RemoteUi::Save s;
    s.doc = d.id;
    s.path = d.ed.path;
    s.point = d.ed.save_point();
    d.ed.buf.snapshot(&s.pieces);
    bool instead = false;
    {
        std::lock_guard lock(u.mu);
        for (RemoteUi::Save& q : u.saves)
            if (q.doc == d.id) {
                q = std::move(s);
                instead = true;
                break;
            }
        if (!instead) u.saves.push_back(std::move(s));
        RemoteUi::Saving* x = u.saving_of(d.id);
        if (!x) x = &u.saving.emplace_back(RemoteUi::Saving{d.id, 0, 0, {}});
        if (!instead) ++x->count;
        x->revision = d.ed.revision();
        x->path = d.ed.path;
    }
    u.cv.notify_all();
    ++remote_gen_;  // (the status bar says "saving…")
    dirty_ = true;
}

bool App::remote_saving(u64 doc) const {
    if (!remote_) return false;
    for (const RemoteUi::Saving& x : remote_->saving)
        if (x.doc == doc) return true;
    return false;
}

// Changed by nothing but saves under way: its text is the one the last of them is writing.
bool App::remote_saving_all(const Doc& d) const {
    if (!remote_) return false;
    for (const RemoteUi::Saving& x : remote_->saving)
        if (x.doc == d.id) return x.revision == d.ed.revision();
    return false;
}

// A document's text is about to go (its tab closes, or it is read again): a
// save copying it now finishes the copy first, and one waiting its turn
// takes a copy of its own now (closing a tab never loses a save asked for).
void App::remote_forget(u64 doc) {
    if (!remote_) return;
    RemoteUi& u = *remote_;
    std::unique_lock lock(u.mu);
    u.copied.wait(lock, [&] { return u.copying != doc; });
    for (RemoteUi::Save& s : u.saves)
        if (s.doc == doc && !s.pieces.empty()) {
            s.text = copy_of(s.pieces);
            s.pieces = {};
        }
}

// Closing a tab, or quitting, while a save is under way waits for it: a box
// says so, and offers to go on without waiting (the save carries on, on its
// own copy of the text, for as long as blit runs). Esc, or any other box in
// its place, calls it off. True: it waits.
bool App::remote_hold_close(size_t i) {
    if (!remote_ || i >= pane_->docs.size()) return false;
    RemoteUi& u = *remote_;
    const Doc& d = *pane_->docs[i];
    if (std::exchange(u.unheld_doc, 0) == d.id || !remote_saving(d.id)) return false;
    remote_hold_box(RemoteUi::kHoldClose, d.id);
    return true;
}

// (asked: what is not saved has been saved, and once the saves are written
// whatever is still not saved is asked about again; else blit quits then.
// Either way all else there was to ask about has been: "Quit without
// waiting" quits.)
bool App::remote_hold_quit(bool asked) {
    if (!remote_ || remote_->saving.empty()) return false;
    remote_hold_box(asked ? RemoteUi::kHoldQuit : RemoteUi::kHoldQuitNow, 0);
    return true;
}

void App::remote_hold_box(u8 what, u64 doc) {
    RemoteUi& u = *remote_;
    // "Saving notes.txt on devbox…", or how many files.
    std::string names;
    size_t files = 0;
    for (const RemoteUi::Saving& x : u.saving) {
        if (what == RemoteUi::kHoldClose && x.doc != doc) continue;
        ++files;
        std::string host, there;
        split_remote(x.path, &host, &there);
        names = name_of(there) + " on " + host;
    }
    const std::string prompt = (files > 1 ? "Saving " + std::to_string(files) + " files" : "Saving " + names) + "\xE2\x80\xA6";
    const bool closing = what == RemoteUi::kHoldClose;
    find_choose_item(prompt, {closing ? "Close without waiting" : "Quit without waiting"}, [this, what, doc](int) {
        RemoteUi& v = *remote_;
        v.hold = RemoteUi::kHoldNone;
        if (what == RemoteUi::kHoldClose) {
            Pane* where = nullptr;
            size_t at = 0;
            Doc* d = find_doc(doc, &where, &at);
            if (!d) return;
            // Changed only by the save under way: it goes; else what was typed since is asked about.
            const bool force = remote_saving_all(*d);
            v.unheld_doc = doc;
            focus_pane(where);
            tab_close(at, force);
            v.unheld_doc = 0;
        } else {
            running_ = false;
        }
    });
    u.hold = what;  // (once the box is open: see remote_asks)
    u.hold_doc = doc;
}

// Saves written (or not): the document is saved as it was when each was
// asked for (typing since leaves it changed), or the bottom line says why
// not; and what was waiting for them goes on.
void App::remote_saved() {
    RemoteUi& u = *remote_;
    std::vector<RemoteUi::Saved> saved;
    {
        std::lock_guard lock(u.mu);
        saved.swap(u.saved);
        for (const RemoteUi::Saved& x : saved)
            if (RemoteUi::Saving* s = u.saving_of(x.doc); s && --s->count <= 0) std::erase_if(u.saving, [&](const RemoteUi::Saving& y) { return y.doc == x.doc; });
    }
    if (saved.empty()) return;
    ++remote_gen_;
    dirty_ = true;
    for (RemoteUi::Saved& x : saved) {
        Doc* d = find_doc(x.doc);
        if (!d || d->ed.path != x.path) continue;  // closed, or saved under another name since
        if (!x.ok) {
            const std::string why = x.error.starts_with("cannot ") ? x.error : "cannot save " + name_of(x.path) + ": " + x.error;
            notify(why);
            continue;
        }
        d->ed.mark_saved_at(x.point);
        d->disk_stamp = x.modified;
        if (lsp_doc_ == d && lsp_.attached()) lsp_.did_save();
        else if (d == pane_->doc) lsp_pending_ = true;  // (a new file has a name now: a server can have it)
        if (cfg_.git) git_.refresh();  // git has something new to say about it
        if (script_) script_write(true, x.doc);
    }
    // What waited for them, once they are all written.
    const u8 hold = u.hold;
    const u64 doc = u.hold_doc;
    if (hold == RemoteUi::kHoldNone || (hold == RemoteUi::kHoldClose ? remote_saving(doc) : !u.saving.empty())) return;
    u.hold = RemoteUi::kHoldNone;
    find_drop_script();  // (the box that said so)
    if (hold == RemoteUi::kHoldQuitNow) {
        running_ = false;
    } else if (hold == RemoteUi::kHoldQuit) {
        request_quit();
    } else {
        Pane* where = nullptr;
        size_t at = 0;
        if (!find_doc(doc, &where, &at)) return;
        focus_pane(where);
        tab_close(at);  // (asking about what was typed since, if anything was)
    }
}

// ---------------------------------------------------------------- listing

// A folder there, for the box that opens files: true with what is in it
// (nothing, if it could not be listed) once it has been listed; false while
// that is on its way, after which find_remote_ready() asks again.
bool App::remote_listing(const std::string& dir, std::vector<RemoteEntry>* out) {
    if (!remote_) return false;
    RemoteUi& u = *remote_;
    {
        std::lock_guard lock(u.mu);
        if (u.listed && u.listed->dir == dir) {
            RemoteUi::Listing l = std::move(*u.listed);
            u.listed.reset();
            if (!l.connected) {
                u.list_failed = dir;  // (not asked again until a connection comes up)
                return false;
            }
            *out = std::move(l.entries);
            return true;
        }
        if (u.list_failed == dir || u.list_wanted == dir || u.list_doing == dir) return false;
        u.list_wanted = dir;
    }
    u.cv.notify_all();
    return false;
}

bool App::remote_listing_busy() const {
    if (!remote_) return false;
    std::lock_guard lock(remote_->mu);
    return !remote_->list_wanted.empty() || !remote_->list_doing.empty();
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
// longer waiting (its connection went) leaves it. (Called too whenever the
// box shuts: a close or quit that was waiting for saves in it is called off.)
void App::remote_asks() {
    if (remote_ && remote_->hold != RemoteUi::kHoldNone && !find_is_open()) remote_->hold = RemoteUi::kHoldNone;
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
    if (!remote_) return find_remote_ready();
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
        std::lock_guard lock(remote_->mu);
        remote_->list_failed.clear();
    }
    find_remote_ready();  // (a folder typed into the box, listed now)
    remote_saved();
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
            // (Not if it was written from here since it was asked about: what was read may be from before that.)
            if (x.status != 0 || d->loading || d->read_only || remote_saving(d->id) || d->disk_stamp != x.known) continue;
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
