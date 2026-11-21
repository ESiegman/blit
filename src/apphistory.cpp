// Local history (history.hpp): a file's text kept at each save, and every
// few minutes while it is being typed in, on a thread of its own (another
// machine's too: kept here, by its whole ssh:// or docker:// path, from the
// text here, so that the machine is never asked for it); "Show
// the file's local history" lists what was kept, each tried out as a diff
// against the text as it is now (in a tab beside it), and one chosen is
// put back, as one edit (one undo takes it back).
#include <algorithm>
#include <cstdlib>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "applua.hpp"
#include "fsio.hpp"
#include "git.hpp"
#include "grammars.hpp"
#include "history.hpp"
#include "remote.hpp"

namespace blit {

namespace {

constexpr u64 kTypedEvery = 5ull * 60 * 1'000'000'000;  // a version kept while typing goes on, this often
constexpr size_t kMost = 200;                           // versions kept of a file
constexpr u64 kLargest = 16u << 20;                     // larger files are not kept

i64 wall_ms() { return static_cast<i64>(std::time(nullptr)) * 1000 + static_cast<i64>((now_ns() / 1'000'000) % 1000); }

}  // namespace

struct HistoryWork {
    struct Job {
        std::string dir, path, text;
        bool saved = false;
        i64 when = 0;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool quit = false, writing = false;
    std::jthread thread;  // (last: it ends before what it reads goes)

    HistoryWork() {
        thread = std::jthread([this] {
            for (;;) {
                Job j;
                {
                    std::unique_lock lock(mu);
                    cv.wait(lock, [this] { return quit || !jobs.empty(); });
                    if (jobs.empty()) return;  // (quit, with nothing left to write)
                    j = std::move(jobs.front());
                    jobs.pop_front();
                    writing = true;
                }
                history_keep(j.dir, j.path, j.text, j.saved, j.when, kMost, nullptr);
                std::lock_guard lock(mu);
                writing = false;
            }
        });
    }
    ~HistoryWork() {
        {
            std::lock_guard lock(mu);
            quit = true;
        }
        cv.notify_all();
    }
    void add(Job j) {
        {
            std::lock_guard lock(mu);
            jobs.push_back(std::move(j));
        }
        cv.notify_one();
    }
    bool idle() {
        std::lock_guard lock(mu);
        return jobs.empty() && !writing;
    }
};

struct HistoryView {
    u64 source = 0, tab = 0;  // the file's document, and the tab the diffs are shown in
    std::vector<HistoryEntry> entries;
};

// Under blit's data folder; or, for a run with no window (the tests, the
// benchmarks), only where BLIT_HISTORY_DIR says, and not at all without it.
namespace {
const char* own_dir() {
    static const char* const dir = std::getenv("BLIT_HISTORY_DIR");  // (read once: it is asked each frame)
    return dir;
}
}  // namespace

std::string App::history_folder(const Doc& d) const {
    const char* own = own_dir();
    return own && *own ? history_dir(own, full_path(d.ed.path)) : history_dir(grammar_dir(), full_path(d.ed.path));
}

bool App::history_wanted(const Doc& d) const {
    const char* own = own_dir();
    if (!win_ && !(own && *own)) return false;
    return cfg_.local_history && !d.ed.path.empty() && !d.read_only && !d.nb && d.ed.buf.size() <= kLargest;
}

// A version of a document's text kept (on the history's thread).
void App::history_keep_doc(Doc& d, bool saved, std::string text) {
    if (!history_work_) history_work_ = std::make_shared<HistoryWork>();
    history_work_->add({history_folder(d), full_path(d.ed.path), std::move(text), saved, wall_ms()});
    d.hist_rev = d.ed.revision();
    d.hist_ns = now_ns();
}

// Before a file is saved: the first time this session, the text it had on
// disk is kept too (so that what was there before is never lost to it).
// Another machine's: the text it had there when it was last read, which
// its buffer still holds (none for a file not there yet, or one being
// saved there under a new name).
void App::history_before_save(Doc& d) {
    if (!history_wanted(d) || d.hist_disk) return;
    d.hist_disk = true;  // (not by hist_ns: typing, or a version kept while typing, comes before the first save)
    if (is_remote(d.ed.path)) {
        if (d.disk_stamp && !d.renamed_from) history_keep_doc(d, true, std::string(d.ed.buf.loaded()));
        return;
    }
    std::string was;
    if (fs_read(d.ed.path, &was, kLargest)) history_keep_doc(d, true, std::move(was));
}

void App::history_saved(Doc& d) {
    if (history_wanted(d)) history_keep_doc(d, true, d.ed.buf.text(0, d.ed.buf.size()));
}

// After each frame: the file in front, typed in for a while since its last
// version, kept as it is.
void App::history_tick() {
    if (!pane_ || !pane_->doc) return;
    Doc& d = *pane_->doc;
    if (!d.ed.modified() || d.hist_rev == d.ed.revision() || !history_wanted(d)) return;
    const u64 now = now_ns();
    if (!d.hist_ns) {  // (the clock starts at the first change)
        d.hist_ns = now;
        d.hist_rev = ~0ull - 1;
        return;
    }
    if (now - d.hist_ns >= kTypedEvery) history_keep_doc(d, false, d.ed.buf.text(0, d.ed.buf.size()));
}

namespace {

// A version against the text now, as a unified diff: its hunks, three
// lines around each; style 1 what is now and was not, 2 what was and is
// not, 3 a hunk's heading.
void diff_text(std::string_view was, std::string_view now, std::string* text, std::vector<u32>* off, std::vector<u8>* style, i64* first) {
    auto lines_of = [](std::string_view t) {
        std::vector<std::string_view> out;
        for (size_t at = 0; at < t.size();) {
            size_t nl = t.find('\n', at);
            if (nl == std::string_view::npos) nl = t.size();
            out.push_back(t.substr(at, nl - at));
            at = nl + 1;
        }
        return out;
    };
    const std::vector<std::string_view> a = lines_of(was), b = lines_of(now);
    const std::vector<GitHunk> hunks = diff_lines(line_hashes(was), line_hashes(now));
    auto add = [&](u8 st, char lead, std::string_view line) {
        const u32 at = static_cast<u32>(text->size());
        if (style->empty() || style->back() != st) off->push_back(at), style->push_back(st);
        *text += lead;
        *text += line;
        *text += '\n';
    };
    *first = -1;
    i64 shift = 0;  // (a line of the text now, less this, is that line in the version)
    for (size_t k = 0; k < hunks.size(); ++k) {
        const GitHunk& h = hunks[k];
        const i64 b0 = h.line, b1 = h.line + h.count, a0 = b0 - shift, a1 = a0 + h.removed;
        const i64 c0 = std::max<i64>(0, b0 - 3), c1 = std::min<i64>(static_cast<i64>(b.size()), b1 + 3);
        add(3, '@', "@ -" + std::to_string(a0 - (b0 - c0) + 1) + " +" + std::to_string(c0 + 1) + " @@");
        if (*first < 0) *first = static_cast<i64>(std::count(text->begin(), text->end(), '\n')) - 1;
        for (i64 l = c0; l < b0; ++l) add(0, ' ', b[static_cast<size_t>(l)]);
        for (i64 l = a0; l < a1 && l < static_cast<i64>(a.size()); ++l) add(2, '-', a[static_cast<size_t>(l)]);
        for (i64 l = b0; l < b1; ++l) add(1, '+', b[static_cast<size_t>(l)]);
        for (i64 l = b1; l < c1; ++l) add(0, ' ', b[static_cast<size_t>(l)]);
        shift += static_cast<i64>(h.count) - static_cast<i64>(h.removed);
    }
    if (text->empty()) {
        off->push_back(0), style->push_back(4);
        *text = "(the same as now)";
    }
}

std::string when_text(const HistoryEntry& e) {
    const std::time_t t = static_cast<std::time_t>(e.when_ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char clock[32];
    std::strftime(clock, sizeof clock, "%a %e %b %H:%M", &tm);
    return relative_age(e.when_ms / 1000, static_cast<i64>(std::time(nullptr))) + "  \xC2\xB7 " + (e.saved ? "saved" : "while typing") + "  \xC2\xB7 " + clock;
}

}  // namespace

// "Show the file's local history".
void App::history_show() {
    Doc& d = *pane_->doc;
    if (d.ed.path.empty() || d.read_only) return notify("Not a file");
    std::vector<HistoryEntry> entries = history_list(history_folder(d));
    if (entries.empty()) return notify("No local history of " + std::string(base_name(d.ed.path)));
    auto v = std::make_shared<HistoryView>();
    v->source = d.id;
    v->entries = std::move(entries);
    history_view_ = v;
    std::vector<std::string> rows;
    for (const HistoryEntry& e : v->entries) rows.push_back(when_text(e));
    find_preview_item("Local history of " + std::string(base_name(d.ed.path)), std::move(rows), 0, [this](int i) { history_try(i); },
                      [this](int i) { history_put_back(i); }, [this] { history_close_tab(); });
    history_try(0);
}

// A version tried out: its diff against now, in the tab beside the file.
void App::history_try(int i) {
    if (!history_view_ || i < 0 || static_cast<size_t>(i) >= history_view_->entries.size()) return;
    HistoryView& v = *history_view_;
    Pane* where = nullptr;
    size_t at = 0;
    Doc* src = find_doc(v.source, &where, &at);
    if (!src) return;
    std::string was;
    if (!history_read(v.entries[static_cast<size_t>(i)].file, &was)) was.clear();
    std::string text;
    std::vector<u32> off;
    std::vector<u8> style;
    i64 first = 0;
    diff_text(was, src->ed.buf.text(0, src->ed.buf.size()), &text, &off, &style, &first);
    Doc* tab = v.tab ? find_doc(v.tab) : nullptr;
    if (!tab) {
        Pane* const keep = pane_;
        pane_ = where;
        tab = add_doc(at + 1);
        tab->title = "History: " + std::string(base_name(src->ed.path));
        tab->read_only = true;
        v.tab = tab->id;
        pane_ = keep;
    }
    debug_tab_fill(*tab, std::move(text), std::move(off), std::move(style), std::max<i64>(0, first), 0);
    Pane* tp = nullptr;
    size_t ti = 0;
    if (find_doc(v.tab, &tp, &ti)) {
        Pane* const keep = pane_;
        pane_ = tp;
        tab_show(ti);
        pane_ = keep;
    }
    pick_theme();
    dirty_ = true;
}

// The tab of diffs goes, and the file is in front again.
void App::history_close_tab() {
    if (!history_view_) return;
    const HistoryView v = *history_view_;
    history_view_.reset();
    Pane* p = nullptr;
    size_t at = 0;
    if (v.tab && find_doc(v.tab, &p, &at)) {
        Pane* const keep = pane_;
        pane_ = p;
        tab_close(at, true);
        pane_ = keep;
    }
    if (find_doc(v.source, &p, &at)) {
        Pane* const keep = pane_;
        pane_ = p;
        tab_show(at);
        pane_ = keep == p ? p : keep;
    }
    dirty_ = true;
}

// Enter: the version put back in the file, as one edit.
void App::history_put_back(int i) {
    if (!history_view_ || i < 0 || static_cast<size_t>(i) >= history_view_->entries.size()) return history_close_tab();
    const u64 source = history_view_->source;
    std::string was;
    const bool read = history_read(history_view_->entries[static_cast<size_t>(i)].file, &was);
    history_close_tab();
    Pane* p = nullptr;
    size_t at = 0;
    Doc* d = find_doc(source, &p, &at);
    if (!d || !read) return;
    if (p != pane_) focus_pane(p);
    if (cfg_.modal) vim_attach();
    const u64 line = d->ed.buf.line_of(d->ed.cursor());
    d->ed.replace(0, d->ed.buf.size(), was);
    d->ed.move_to(d->ed.buf.line_start(std::min<u64>(line, d->ed.buf.line_count() - 1)), false);
    scroll_to_cursor();
    dirty_ = true;
}

// A file changed on disk (or on its machine) under a tab with changes of
// its own: the tab keeps them, a notice says so, and the next save asks
// first whether to write over what is there now.
void App::disk_changed_under(Doc& d) {
    d.disk_moved = true;
    notify(base_name(d.ed.path) + " changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two)", 1);
}

// Saving a file that changed on disk under it: asked in the box (its footer
// says why), with what is on disk compared on the way if wanted. (wait: a
// box that is open is not taken; the question waits for it to close, as
// the second file of Save all waits for the first's answer.) Closing its
// tab, or quitting, that saved it goes on once it is saved over it, and is
// called off by anything else (Compare with disk, Esc, or another box).
void App::save_ask(u64 doc, bool wait) {
    const Doc* d = find_doc(doc);
    if (!d) return;
    if (wait && find_is_open()) {
        if (std::ranges::find(save_waiting_, doc) == save_waiting_.end()) save_waiting_.push_back(doc);
        return;
    }
    const std::string name = base_name(d->ed.path);
    auto off = [this, doc] {
        if (close_after_ask_ == doc) close_after_ask_ = 0;
        quit_after_ask_ = false;
    };
    find_preview_item("Save " + name, {"Save over it", "Compare with disk"}, 0, nullptr, [this, doc, off](int i) {
        Pane* where = nullptr;
        size_t at = 0;
        Doc* const now = find_doc(doc, &where, &at);
        if (!now || i != 0) off();
        if (!now || i < 0) return;
        focus_pane(where);
        tab_show(at);
        if (i == 1) return disk_compare();
        now->save_over = true;
        const bool saved = save();
        const bool closing = close_after_ask_ == doc;
        if (closing) close_after_ask_ = 0;
        if (saved && closing && find_doc(doc, &where, &at)) {  // (a script's handler may have closed it meanwhile)
            focus_pane(where);
            tab_close(at);  // (another machine's: once it has been written there)
        }
        if (!saved) quit_after_ask_ = false;
        else if (quit_after_ask_ && !save_asks_waiting()) {  // (the last one asked about: on to what is left, or out)
            quit_after_ask_ = false;
            request_quit();
        }
    }, off);
    find_ask_footer(name + " changed on disk since it was read here");
}

// Whether a file is still waiting to be asked about.
bool App::save_asks_waiting() {
    return std::ranges::any_of(save_waiting_, [this](u64 doc) {
        const Doc* d = find_doc(doc);
        return d && d->disk_moved && d->ed.modified();
    });
}

// The box is free: the next file waiting to be asked about (one closed,
// saved or no longer changed since is passed over; a quit waiting for the
// answers goes on when there is none left to ask about).
void App::save_ask_next() {
    while (!save_waiting_.empty() && !find_is_open()) {
        const u64 doc = save_waiting_.front();
        save_waiting_.erase(save_waiting_.begin());
        if (const Doc* d = find_doc(doc); d && d->disk_moved && d->ed.modified()) save_ask(doc);
    }
    if (quit_after_ask_ && save_waiting_.empty() && !find_is_open()) {
        quit_after_ask_ = false;
        request_quit();
    }
}

// "Compare with disk": what the file in front has here against what its
// file has on disk now (another machine's is read there first, on a thread).
void App::disk_compare() {
    const Doc& d = *pane_->doc;
    if (d.ed.path.empty() || d.read_only || d.loading || d.term || d.prof || d.md || d.image || d.pdf) return notify("Not a file");
    if (is_remote(d.ed.path)) return remote_compare(d);  // (shown when it has been read: disk_compare_show)
    std::string disk;
    if (!fs_read(d.ed.path, &disk, 1ull << 30)) return notify("Cannot read " + base_name(d.ed.path));
    disk_compare_show(d.id, disk);
}

// The comparison, in a tab beside the file (the same one again, if it is still there).
void App::disk_compare_show(u64 doc, std::string_view disk) {
    Pane* where = nullptr;
    size_t at = 0;
    Doc* src = find_doc(doc, &where, &at);
    if (!src) return;
    std::string text;
    std::vector<u32> off;
    std::vector<u8> style;
    i64 first = 0;
    diff_text(disk, src->nb ? nb_json(*src) : src->ed.buf.text(0, src->ed.buf.size()), &text, &off, &style, &first);  // (a notebook: as it would be saved)
    std::string host, there;
    const std::string title = base_name(src->ed.path) + (split_remote(src->ed.path, &host, &there) ? " (against disk, " + host + ")" : " (against disk)");  // (another machine's names it)
    Pane* const keep = pane_;
    pane_ = where;
    Doc* tab = nullptr;
    for (size_t i = 0; i < where->docs.size() && !tab; ++i)
        if (where->docs[i]->read_only && where->docs[i]->title == title) tab = where->docs[i].get(), at = i;
    if (!tab) {
        tab = add_doc(++at);
        tab->title = title;
        tab->read_only = true;
    }
    debug_tab_fill(*tab, std::move(text), std::move(off), std::move(style), std::max<i64>(0, first), 0);
    tab_show(at);
    pane_ = keep;
    pick_theme();
    dirty_ = true;
}

// For the tests: the versions kept of the file in front (saved or typed,
// newest first), once what is being written has been.
std::string App::history_dump() {
    while (history_work_ && !history_work_->idle()) SDL_Delay(2);
    std::string out;
    for (const HistoryEntry& e : history_list(history_folder(*pane_->doc))) out += e.saved ? "s" : "t";
    return out.empty() ? "none" : out;
}

}  // namespace blit
