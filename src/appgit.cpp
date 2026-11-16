// Git in the editor: the Git tab of the side panel (what has changed, a
// commit message, push and pull, branches, history), the marks in the line
// numbers for lines changed since the last commit, blame, and tabs that
// show a diff. git itself runs on its own thread (git.cpp); this file asks
// it things and puts the answers on screen.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "app.hpp"
#include "remote.hpp"
#include "textbox.hpp"

namespace blit {

namespace {

constexpr u64 kBlameAfterNs = 400'000'000;  // a blame is asked for this long after the text last changed
constexpr u64 kGitMaxBytes = 16u << 20;     // files larger than this are not compared or blamed

enum : u32 {
    kActNone,
    kActCommit,
    kActPull,
    kActPush,
    kActFetch,
    kActCancel,
    kActAbortMerge,
    kActStage,
    kActUnstage,
    kActDiscard,
    kActOpenFile,
    kActOpenDiff,
    kActSwitch,
    kActMerge,
    kActDeleteBranch,
    kActNewBranch,
    kActStageAll,
    kActShowCommit,
    kActBlame,
    kActRefresh,
};

enum : u8 { kRowMessage, kRowActions, kRowContext, kRowNote, kRowHeader, kRowEntry, kRowBranch, kRowCommit, kRowBlank };
enum : u32 { kSecConflicts, kSecStaged, kSecChanges, kSecBranches, kSecHistory, kSecCount };
const char* const kSections[] = {"Conflicts", "Staged", "Changes", "Branches", "History"};
constexpr u32 kStagedBit = 0x80000000u;  // an entry row: the file's staged side

std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

std::string_view name_of(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

i64 stamp_of(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) ? static_cast<i64>(info.modify_time) : 0;
}

// Colours for the text of a diff, by what each line starts with.
std::unique_ptr<Highlights> diff_marks(const std::string& text) {
    auto h = std::make_unique<Highlights>();
    h->base = 0;
    h->end = text.size();
    u8 last = 0;
    for (size_t at = 0; at < text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        const std::string_view line(text.data() + at, nl - at);
        u8 style = 0;
        if (line.starts_with("+++") || line.starts_with("---") || line.starts_with("diff ") || line.starts_with("index ") ||
            line.starts_with("commit ") || line.starts_with("Author:") || line.starts_with("Date:") || line.starts_with("new file") ||
            line.starts_with("deleted file") || line.starts_with("rename ") || line.starts_with("similarity "))
            style = 4;
        else if (line.starts_with("@@")) style = 3;
        else if (line.starts_with("+")) style = 1;
        else if (line.starts_with("-")) style = 2;
        if (style != last || at == 0) {
            h->off.push_back(static_cast<u32>(at));
            h->style.push_back(style);
            last = style;
        }
        at = nl + 1;
    }
    h->seal();
    return h;
}

}  // namespace

struct App::GitUi {
    GitStatus status;
    std::vector<GitBranch> branches;
    std::vector<GitCommit> log;
    // Where git was last pointed: the working directory, or while there
    // is none, the project of this document. `known`: git has answered.
    std::string dir;
    u64 doc = 0;
    bool started = false, rooted = false, known = false;
    std::string busy;   // the command that is running
    std::string note;   // how the last one went
    bool note_bad = false;

    // The Git tab.
    struct Row {
        u8 kind = kRowBlank;
        u32 index = 0;
    };
    struct Word {  // a word in the panel that does something when clicked
        u32 row, c0, c1, action;
    };
    std::vector<Row> rows;
    u64 rows_gen = 0;
    std::vector<Word> words;
    // The row that is picked, and what it is a row of: when git reports
    // again the rows are made again, and the same thing is picked in them.
    int sel = -1;
    u8 sel_kind = kRowBlank;
    bool sel_staged = false;
    std::string sel_name;
    std::string name_of_row(const Row& r) const {
        const u32 i = r.index & ~kStagedBit;
        if (r.kind == kRowEntry) return i < status.entries.size() ? status.entries[i].path : std::string();
        if (r.kind == kRowBranch) return i < branches.size() ? branches[i].name : std::string();
        if (r.kind == kRowCommit) return i < log.size() ? log[i].hash : std::string();
        return {};
    }
    void pick(int i) {
        sel = i;
        sel_kind = kRowBlank;
        sel_name.clear();
        if (i < 0 || static_cast<size_t>(i) >= rows.size()) return;
        sel_kind = rows[static_cast<size_t>(i)].kind;
        sel_staged = rows[static_cast<size_t>(i)].index & kStagedBit;
        sel_name = name_of_row(rows[static_cast<size_t>(i)]);
    }
    u32 top = 0, body_rows = 0;
    bool fold[kSecCount] = {};
    u8 input = 0;  // 0 none, 1 the commit message, 2 a new branch's name
    std::string message, branch_name;

    // Blame of the cursor's line, as cells.
    std::vector<u32> note_cells;
    GridNote blame_note;
    u64 note_key = 0;
};

void App::git_create() { gitui_ = std::make_shared<GitUi>(); }

bool App::confirm(const std::string& what, const char* yes) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING;
    box.window = win_;
    box.title = "blit";
    box.message = what.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int hit = 0;
#if BLIT_DEV
    if (!win_) return true;  // scripted runs have nobody to ask
#endif
    SDL_ShowMessageBox(&box, &hit);
    return hit == 1;
}

// ---------------------------------------------------------------- talking to git

// Takes in what git has finished.
void App::sync_git() {
    GitUi& g = *gitui_;
    // A frame is drawn only for news that shows: an answer that changes
    // nothing on screen (a status while the Git tab is shut, changed lines
    // that are the ones already marked) costs no frame.
    bool shows = false;
    for (Git::Result& r : git_.poll()) {
        switch (r.kind) {
            case Git::kStatus: {
                if (!r.ok) {
                    g.note = first_line(r.text);
                    g.note_bad = true;
                    break;
                }
                g.known = true;
                const bool moved = r.status.head != g.status.head || r.status.root != g.status.root;
                g.status = std::move(r.status);
                g.branches = std::move(r.branches);
                g.log = std::move(r.log);
                shows |= panel_px() > 0;
                if (moved)  // another commit to compare with: ask again
                    for (const std::unique_ptr<Pane>& p : panes_)
                        for (const std::unique_ptr<Doc>& d : p->docs) d->hunks_asked = d->hunks_rev = d->blame_asked = d->blame_rev = ~0ull;
                break;
            }
            case Git::kAction: {
                g.note = r.ok ? r.label + ": done" : r.label + ": " + first_line(r.text);
                if (r.ok && !r.text.empty() && r.text.size() < 120 && r.text.find('\n') == std::string::npos) g.note = r.label + ": " + r.text;
                g.note_bad = !r.ok;
                if (!r.ok) std::fprintf(stderr, "blit: git %s: %s\n", r.label.c_str(), r.text.c_str());
                if (r.ok && r.label == "Commit") g.message.clear();
                shows = true;
                reload_changed();  // a pull, a merge, a change of branch: files may be other files now
                for (const std::unique_ptr<Pane>& p : panes_)
                    for (const std::unique_ptr<Doc>& d : p->docs) d->hunks_asked = d->hunks_rev = d->blame_asked = d->blame_rev = ~0ull;
                break;
            }
            case Git::kHunks: {
                Doc* d = find_doc(r.doc);
                if (!d) break;
                d->hunks_rev = r.revision;
                if (r.hunks != d->hunks) {
                    d->hunks = std::move(r.hunks);
                    ++d->hunks_gen;
                    shows = true;
                }
                break;
            }
            case Git::kBlame: {
                Doc* d = find_doc(r.doc);
                if (!d) break;
                d->blame_rev = r.revision;
                d->blame = r.ok ? std::move(r.blame) : nullptr;
                ++d->blame_gen;
                shows |= blame_shown(*d);
                break;
            }
            case Git::kText: {
                Pane* where = nullptr;
                Doc* d = find_doc(r.doc, &where);
                if (!d) break;
                d->ed.insert(r.ok ? r.text : "git: " + r.text + "\n");
                d->ed.move_to(0, false);
                d->ed.mark_saved();
                if (r.ok) d->marks = diff_marks(r.text);
                where->cache_rev = ~0ull;
                pick_theme();
                shows = true;
                break;
            }
            default: break;
        }
    }
    const std::string busy = git_.busy();
    shows |= busy != g.busy;
    g.busy = busy;
    ++git_gen_;
    git_rows();
    dirty_ = dirty_ || shows;
}

// Once a frame for each document on show: asks for its changed lines when
// the text is not the one they were worked out for, and for its blame a
// little after the text last changed. One question at a time per document;
// the next goes out when the answer is in.
App::GitSummary App::git_summary() const {
    GitSummary s;
    if (!cfg_.git || !gitui_) return s;
    const GitStatus& st = gitui_->status;
    s.repo = st.repo;
    s.merging = st.merging;
    s.root = st.root;
    s.branch = st.branch;
    s.head = st.head;
    s.ahead = st.ahead;
    s.behind = st.behind;
    s.changed = st.entries.size();
    return s;
}

void App::git_want(Doc& d) {
    const GitUi& g = *gitui_;
    if (!cfg_.git || !g.status.repo || d.ed.path.empty() || d.read_only || d.ed.buf.size() > kGitMaxBytes) return;
    const u64 rev = d.ed.revision();
    const u64 now = now_ns();
    if (d.seen_rev != rev) {
        d.seen_rev = rev;
        d.changed_ns = now;
    }
    if ((cfg_.line_numbers || cfg_.status_bar) && d.hunks_asked != rev && d.hunks_asked == d.hunks_rev) {
        std::vector<Piece> pieces;
        d.ed.buf.snapshot(&pieces);
        d.git = &git_;
        d.hunks_asked = rev;
        git_.hunks(d.id, rev, full_path(d.ed.path), std::move(pieces));
    }
    if (blame_shown(d) && d.blame_asked != rev && d.blame_asked == d.blame_rev) {  // (git is not asked until blame is turned on)
        if (now - d.changed_ns < kBlameAfterNs) {  // still typing: come back to it
            const u64 due = d.changed_ns + kBlameAfterNs;
            if (!git_due_ns_ || due < git_due_ns_) git_due_ns_ = due;
            return;
        }
        std::vector<Piece> pieces;
        d.ed.buf.snapshot(&pieces);
        d.git = &git_;
        d.blame_asked = rev;
        git_.blame(d.id, rev, full_path(d.ed.path), std::move(pieces));
    }
}

// How long the idle loop may sleep before git_tick() has something to do.
int App::git_wait_ms() const {
    if (!git_due_ns_) return -1;
    const u64 now = now_ns();
    return now >= git_due_ns_ ? 0 : static_cast<int>((git_due_ns_ - now) / 1'000'000) + 1;
}

void App::git_tick() {
    if (git_due_ns_ && now_ns() >= git_due_ns_) {
        git_due_ns_ = 0;
        dirty_ = true;  // the next frame asks (see git_want)
    }
}

// Files that changed on disk under an open tab (git checked out another
// branch, pulled, merged; or another program wrote them) are read again,
// unless the tab has changes of its own that reading would lose.
void App::reload_changed() {
    for (const std::unique_ptr<Pane>& p : panes_) {
        for (const std::unique_ptr<Doc>& d : p->docs) {
            if (d->ed.path.empty() || d->read_only) continue;
            if (is_remote(d->ed.path)) {  // (another machine's: looked at on a thread, read again there if it changed)
                remote_load(*d, true);
                continue;
            }
            const i64 stamp = stamp_of(d->ed.path);
            if (!stamp || stamp == d->disk_stamp) continue;
            d->disk_stamp = stamp;
            if (d->ed.modified()) continue;
            reread(*p, *d, nullptr);
        }
    }
    picture_recheck();  // (and pictures, for their tabs and the Markdown preview)
    pdf_recheck();      // (and PDFs)
    pick_theme();
    dirty_ = true;
}

// Reads a file again (or takes these bytes for it), keeping the cursor's line and the view.
void App::reread(Pane& p, Doc& d, const std::string* bytes) {
    Pane* const focused = pane_;
    Doc* const front = p.doc;
    const u64 line = d.ed.buf.line_of(d.ed.cursor());
    const u32 col = d.ed.col_between(d.ed.buf.line_start(line), d.ed.cursor());
    pane_ = &p;
    p.doc = &d;
    if (lsp_doc_ == &d) {
        stop_lsp(true);
        lsp_pending_ = true;
    }
    stop_syntax();
    git_.forget(d.id);
    d.measurer = {};
    std::string err;
    bool read = true;
    if (bytes) d.ed.open_bytes(d.ed.path, *bytes);
    else read = d.ed.open(d.ed.path.c_str(), &err);
    if (read) {
        const u64 last = d.ed.buf.line_count() - 1;
        d.ed.move_to(d.ed.off_at(std::min(line, last), col), false);
    }
    start_syntax(static_cast<u64>(std::max(0.0, &d == front ? p.target_y : d.scroll_y)) / static_cast<u64>(font_->cell_h()));
    d.hunks_asked = d.hunks_rev = d.blame_asked = d.blame_rev = d.widest_rev = d.indent_rev = ~0ull;
    d.blame.reset();
    ++d.blame_gen;
    p.cache_rev = ~0ull;
    p.doc = front;
    pane_ = focused;
    dirty_ = true;
}

// A new tab holding what a git command prints (a diff, a commit). The text
// arrives in a moment, see sync_git().
void App::git_open_text(std::vector<std::string> args, const std::string& title) {
    Doc* d = add_doc(pane_->tab + 1);
    d->title = title;
    d->read_only = true;
    const u64 id = d->id;
    tab_show(pane_->tab + 1);
    panel_focus(false);
    git_.text(id, std::move(args), title);
}

// ---------------------------------------------------------------- doing things

void App::git_do(u32 action, const std::string& arg) {
    GitUi& g = *gitui_;
    if (!cfg_.git || !g.status.repo) return;
    // What the action is about: its argument, or the row picked in the Git tab.
    const GitUi::Row* row = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    const GitEntry* entry = nullptr;
    bool staged_side = false;
    std::string name = arg;
    if (!arg.empty()) {
        for (const GitEntry& e : g.status.entries)
            if (e.path == arg) entry = &e;
    } else if (row && row->kind == kRowEntry) {
        entry = &g.status.entries[row->index & ~kStagedBit];
        staged_side = row->index & kStagedBit;
        name = entry->path;
    } else if (row && row->kind == kRowBranch) {
        name = g.branches[row->index].name;
    } else if (row && row->kind == kRowCommit) {
        name = g.log[row->index].hash;
    }
    auto run = [&](std::vector<std::string> args, const char* label, std::string input = {}) {
        g.busy = label;
        g.note.clear();
        git_.run(std::move(args), label, std::move(input));
    };
    const std::string full = g.status.root + "/" + name;
    switch (action) {
        case kActCommit: {
            std::string text = arg.empty() ? g.message : arg;
            while (!text.empty() && text.back() == ' ') text.pop_back();
            if (text.empty()) {  // it needs saying what the commit is
                g.input = 1;
                g.note = "Type a message for the commit, then Enter";
                g.note_bad = false;
                break;
            }
            bool any_staged = false;
            for (const GitEntry& e : g.status.entries) any_staged |= e.staged();
            // With nothing staged, a commit takes every change to a file git already knows.
            run(any_staged ? std::vector<std::string>{"commit", "-q", "-F", "-"} : std::vector<std::string>{"commit", "-q", "-a", "-F", "-"}, "Commit",
                text + "\n");
            g.input = 0;
            break;
        }
        case kActPull: run({"pull", "--no-edit"}, "Pull"); break;
        case kActPush:
            // A branch that follows nothing yet is published under its own name.
            run(g.status.upstream.empty() ? std::vector<std::string>{"push", "-u", "origin", "HEAD"} : std::vector<std::string>{"push"}, "Push");
            break;
        case kActFetch: run({"fetch", "--all", "--prune"}, "Fetch"); break;
        case kActCancel: git_.cancel(); break;
        case kActAbortMerge: run({"merge", "--abort"}, "Abort merge"); break;
        case kActStage:
            if (!name.empty()) run({"add", "--", name}, "Stage");
            break;
        case kActStageAll: run({"add", "-A"}, "Stage all"); break;
        case kActUnstage:
            if (name.empty()) break;
            if (g.status.head.empty()) {  // no commit yet to go back to
                run({"rm", "--cached", "-q", "--", name}, "Unstage");
            } else {
                std::vector<std::string> args = {"restore", "--staged", "--", name};
                if (entry && !entry->from.empty()) args.push_back(entry->from);
                run(std::move(args), "Unstage");
            }
            break;
        case kActDiscard:
            if (!entry || entry->index == '?') {
                g.note = "A file git does not know is not thrown away from here (the Files tab can move it to the Trash)";
                g.note_bad = false;
                break;
            }
            if (confirm("Throw away the changes to " + std::string(name_of(name)) + " that are not staged? This cannot be undone.", "Discard"))
                run({"restore", "--", name}, "Discard");
            break;
        case kActOpenFile:
            if (!name.empty()) open_path(full.c_str());
            break;
        case kActOpenDiff:
            if (!entry) break;
            if (entry->index == '?' || entry->conflict) {  // nothing to compare with, or markers to deal with: the file itself
                open_path(full.c_str());
            } else {
                const bool cached = arg.empty() ? staged_side : !entry->unstaged();
                git_open_text(cached ? std::vector<std::string>{"diff", "--cached", "--", name} : std::vector<std::string>{"diff", "--", name},
                              std::string(name_of(name)) + (cached ? " (staged)" : " (changes)"));
            }
            break;
        case kActSwitch:
            if (name.empty()) break;
            // A branch on a server gets a local one of the same name that follows it.
            if (row && arg.empty() && g.branches[row->index].remote) run({"switch", "-q", "--track", name}, "Switch");
            else run({"switch", "-q", name}, "Switch");
            break;
        case kActMerge:
            if (!name.empty()) run({"merge", "--no-edit", name}, "Merge");
            break;
        case kActDeleteBranch:
            if (!name.empty() && confirm("Delete the branch " + name + "?", "Delete")) run({"branch", "-d", name}, "Delete branch");
            break;
        case kActNewBranch:
            if (arg.empty()) {
                g.input = 2;
                g.branch_name.clear();
            } else {
                run({"switch", "-q", "-c", arg}, "New branch");
                g.input = 0;
            }
            break;
        case kActShowCommit:
            if (!name.empty()) git_open_text({"show", "--stat", "--patch", name}, name);
            break;
        case kActBlame:
            pane_->doc->blame_on = !pane_->doc->blame_on;
            pane_->cache_rev = ~0ull;
            break;
        case kActRefresh: git_.refresh(); break;
        default: break;
    }
    ++git_gen_;
    dirty_ = true;
}

// ---------------------------------------------------------------- in the text view

// Who last changed the cursor's line, after the end of that line.
void App::git_note(FrameDesc& fd) {
    GitUi& g = *gitui_;
    const Doc& d = *pane_->doc;
    if (!blame_shown(d) || focus_view_ >= fd.view_count || !d.blame || d.blame_rev != d.ed.revision() || panel_has_keyboard()) return;
    GridSpec& grid = fd.views[focus_view_].grid;
    if (!grid.notes.empty() || pane_->cur_line >= d.blame->lines.size()) return;  // (a problem's message has the line already)
    const GitBlame::Commit& c = d.blame->commits[d.blame->lines[pane_->cur_line]];
    const u64 key = (d.id * 1000003 + d.blame_gen) * 1000003 + pane_->cur_line + (static_cast<u64>(font_->generation()) << 48);
    if (key != g.note_key) {
        g.note_key = key;
        std::string text = c.time ? c.author + ", " + relative_age(c.time, static_cast<i64>(std::time(nullptr))) + " \xC2\xB7 " + c.summary : c.author;
        if (text.size() > 120) text.resize(120);
        TextBox box;
        box.resize(width_of(text), 1);
        box.put(*font_, 0, 0, text, kInkMuted, Font::kItalic);
        g.note_cells = std::move(box.cells);
    }
    const u64 start = d.ed.buf.line_start(pane_->cur_line);
    g.blame_note = {pane_->cur_line, d.ed.col_between(start, d.ed.line_end(pane_->cur_line)) + 4, g.note_cells.data(), static_cast<u32>(g.note_cells.size())};
    grid.notes = {&g.blame_note, 1};
}

// ---------------------------------------------------------------- the Git tab

// The rows of the Git tab, from what git last said. Made again whenever
// that changes, so that a row never points at something that has gone.
void App::git_rows() {
    GitUi& g = *gitui_;
    g.rows_gen = git_gen_;
    g.rows.clear();
    g.rows.push_back({kRowMessage, 0});
    g.rows.push_back({kRowActions, 0});
    g.rows.push_back({kRowContext, 0});
    if (!g.note.empty()) g.rows.push_back({kRowNote, 0});
    auto section = [&](u32 sec, auto&& wanted, bool staged) {
        u32 n = 0;
        for (const GitEntry& e : g.status.entries) n += wanted(e);
        if (!n) return;
        g.rows.push_back({kRowHeader, sec});
        if (g.fold[sec]) return;
        for (u32 i = 0; i < g.status.entries.size(); ++i)
            if (wanted(g.status.entries[i])) g.rows.push_back({kRowEntry, i | (staged ? kStagedBit : 0)});
    };
    section(kSecConflicts, [](const GitEntry& e) { return e.conflict; }, false);
    section(kSecStaged, [](const GitEntry& e) { return e.staged(); }, true);
    section(kSecChanges, [](const GitEntry& e) { return e.unstaged(); }, false);
    g.rows.push_back({kRowHeader, kSecBranches});
    if (!g.fold[kSecBranches])
        for (u32 i = 0; i < g.branches.size(); ++i) g.rows.push_back({kRowBranch, i});
    if (!g.log.empty()) {
        g.rows.push_back({kRowHeader, kSecHistory});
        if (!g.fold[kSecHistory])
            for (u32 i = 0; i < g.log.size(); ++i) g.rows.push_back({kRowCommit, i});
    }
    // (What was picked stays picked if it is still there.)
    g.sel = -1;
    for (size_t i = 0; i < g.rows.size() && g.sel_kind != kRowBlank; ++i) {
        const GitUi::Row& r = g.rows[i];
        if (r.kind == g.sel_kind && static_cast<bool>(r.index & kStagedBit) == (r.kind == kRowEntry && g.sel_staged) && g.name_of_row(r) == g.sel_name)
            g.sel = static_cast<int>(i);
    }
}

// Draws the tab into the panel's cells: the branch on the line under the
// panel's tabs, then rows that scroll.
void App::git_panel_draw(TextBox& box, bool focused) {
    GitUi& g = *gitui_;
    Font& font = *font_;
    const u32 cols = box.cols, rows = box.rows;
    constexpr u32 kHead = 2;
    g.body_rows = rows > kHead ? rows - kHead : 0;
    g.words.clear();
    if (!cfg_.git) {
        box.put(font, 1, 0, "Git is switched off in the settings", kInkMuted);
        return;
    }
    if (!g.status.repo) {
        // Say which folder was looked at: that is the first thing to check.
        if (g.known) {
            const u32 c = box.put(font, 1, 0, "Not a git repository: ", kInkMuted);
            box.put(font, 1, c, name_of(g.dir), 0);
            if (rows > 3) box.put(font, 3, 0, "Git follows the working directory,", kInkMuted);
            if (rows > 4) box.put(font, 4, 0, "the folder the Files tab shows.", kInkMuted);
        } else {
            box.put(font, 1, 0, "Looking...", kInkMuted);
        }
        return;
    }
    // The branch, and how far it is from the one it follows.
    {
        u32 c = box.put(font, 1, 0, "\xE2\x8E\x87 ", kInkAccent);  // a branch sign
        c = box.put(font, 1, c, g.status.branch.empty() ? "(no branch) " + g.status.head : g.status.branch, 0, Font::kBold);
        if (g.status.ahead) c = box.put(font, 1, c + 1, "\xE2\x86\x91" + std::to_string(g.status.ahead), kInkAccent);
        if (g.status.behind) c = box.put(font, 1, c + 1, "\xE2\x86\x93" + std::to_string(g.status.behind), kInkAccent);
        if (g.status.merging) box.put(font, 1, c + 1, "merging", kInkDiagnostic + 1);
    }

    if (g.rows_gen != git_gen_) git_rows();
    g.top = std::min<u32>(g.top, g.rows.size() > g.body_rows ? static_cast<u32>(g.rows.size()) - g.body_rows : 0);

    const GitUi::Row* picked = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    // A row of words to click, each remembered with what it does.
    auto words = [&](u32 r, std::initializer_list<std::pair<const char*, u32>> list) {
        u32 c = 1;
        for (const auto& [label, action] : list) {
            if (!label) continue;
            const u32 end = box.put(font, r, c, label, action == kActNone ? kInkMuted : kInkAccent);
            if (action != kActNone) g.words.push_back({r, c, end, action});
            c = end + 2;
        }
    };
    auto typed = [&](u32 r, u32 c, const std::string& text) {  // text being typed, with a mark where the next letter goes
        const u32 end = box.put(font, r, c, text, 0);
        if (end < cols) box.cells[static_cast<size_t>(r) * cols + end] |= kCellSelected;
    };
    for (u32 r = kHead; r < rows && g.top + (r - kHead) < g.rows.size(); ++r) {
        const size_t i = g.top + (r - kHead);
        const GitUi::Row& row = g.rows[i];
        switch (row.kind) {
            case kRowMessage: {
                const u32 c = box.put(font, r, 0, "\xE2\x9C\x8E ", kInkAccent);  // a pencil
                if (g.input == 1) typed(r, c, g.message);
                else box.put(font, r, c, g.message.empty() ? "Message (type it, Enter commits)" : g.message, g.message.empty() ? kInkMuted : 0);
                break;
            }
            case kRowActions:
                if (!g.busy.empty()) words(r, {{(g.busy + "...").c_str(), kActNone}, {"Cancel", kActCancel}});
                else words(r, {{"Commit", kActCommit}, {"Pull", kActPull}, {"Push", kActPush}, {"Fetch", kActFetch}, {g.status.merging ? "Abort merge" : nullptr, kActAbortMerge}});
                break;
            case kRowContext:
                if (g.input == 2) {
                    typed(r, box.put(font, r, 1, "New branch: ", kInkMuted), g.branch_name);
                } else if (picked && picked->kind == kRowEntry) {
                    const GitEntry& e = g.status.entries[picked->index & ~kStagedBit];
                    if (picked->index & kStagedBit) words(r, {{"Unstage", kActUnstage}, {"Diff", kActOpenDiff}, {"Open", kActOpenFile}});
                    else words(r, {{"Stage", kActStage}, {e.index == '?' || e.conflict ? nullptr : "Discard", kActDiscard}, {e.index == '?' || e.conflict ? nullptr : "Diff", kActOpenDiff}, {"Open", kActOpenFile}});
                } else if (picked && picked->kind == kRowBranch) {
                    if (g.branches[picked->index].current) words(r, {{"New branch", kActNewBranch}});
                    else if (g.branches[picked->index].remote) words(r, {{"Switch", kActSwitch}, {"Merge", kActMerge}, {"New branch", kActNewBranch}});
                    else words(r, {{"Switch", kActSwitch}, {"Merge", kActMerge}, {"Delete", kActDeleteBranch}, {"New branch", kActNewBranch}});
                } else if (picked && picked->kind == kRowCommit) {
                    words(r, {{"Show", kActShowCommit}, {"New branch", kActNewBranch}});
                } else {
                    words(r, {{"Stage all", kActStageAll}, {"New branch", kActNewBranch}, {"Blame", kActBlame}});
                }
                break;
            case kRowNote: box.put(font, r, 1, g.note, g.note_bad ? kInkDiagnostic : kInkMuted); break;
            case kRowHeader: {
                u32 n = 0;
                if (row.index == kSecBranches) n = static_cast<u32>(g.branches.size());
                else if (row.index == kSecHistory) n = static_cast<u32>(g.log.size());
                else
                    for (const GitEntry& e : g.status.entries) n += row.index == kSecConflicts ? e.conflict : row.index == kSecStaged ? e.staged() : e.unstaged();
                const u32 c = box.put(font, r, 0, g.fold[row.index] ? "\xE2\x96\xB8 " : "\xE2\x96\xBE ", kInkMuted);
                box.put(font, r, c, std::string(kSections[row.index]) + " (" + std::to_string(n) + ")", 0, Font::kBold);
                break;
            }
            case kRowEntry: {
                const GitEntry& e = g.status.entries[row.index & ~kStagedBit];
                const bool staged = row.index & kStagedBit;
                const char letter = e.conflict ? 'U' : staged ? e.index : e.index == '?' ? '?' : e.work;
                const u32 ink = letter == 'D' || letter == 'U' ? kInkDiagnostic : letter == 'A' || letter == '?' ? kInkAdded : letter == 'R' ? kInkAccent : kInkChanged;
                box.put(font, r, 2, std::string(1, letter), ink, Font::kBold);
                // The name, then the folder it is in, quieter.
                const std::string_view name = name_of(e.path);
                u32 c = box.put(font, r, 4, name, 0);
                if (name.size() < e.path.size() && c + 2 < cols) box.put(font, r, c + 1, std::string_view(e.path).substr(0, e.path.size() - name.size()), kInkMuted);
                if (cols > 3) {  // and at the end of the row, the sign that stages or unstages it
                    const u32 at = cols - 2;
                    box.cells[static_cast<size_t>(r) * cols + at - 1] = 0;
                    box.put(font, r, at, staged ? "\xE2\x88\x92" : "+", kInkAccent, Font::kBold);
                    box.cells[static_cast<size_t>(r) * cols + at + 1] = 0;
                    g.words.push_back({r, at - 1, at + 2, staged ? static_cast<u32>(kActUnstage) : static_cast<u32>(kActStage)});
                }
                break;
            }
            case kRowBranch: {
                const GitBranch& b = g.branches[row.index];
                box.put(font, r, 2, b.current ? "*" : " ", kInkAccent, Font::kBold);
                box.put(font, r, 4, b.name, b.current ? kInkAccent : b.remote ? kInkMuted : 0, b.current ? Font::kBold : 0);
                break;
            }
            case kRowCommit: {
                const GitCommit& c = g.log[row.index];
                const u32 at = box.put(font, r, 2, c.hash, kInkMuted);
                box.put(font, r, at + 1, c.subject, 0);
                break;
            }
            default: break;
        }
        if (static_cast<int>(i) == g.sel && focused) box.select_row(r);
        else if (static_cast<int>(i) == g.sel) {
            // (picked, but the keyboard is elsewhere: a quieter sign, on the first cell)
            box.cells[static_cast<size_t>(r) * cols] |= kCellSelected;
        }
    }
}

bool App::git_panel_text(std::string_view text) {
    GitUi& g = *gitui_;
    if (!g.status.repo) return true;
    if (g.input == 0) g.input = 1;  // typing with nothing else asked for is the commit message
    std::string& into = g.input == 2 ? g.branch_name : g.message;
    if (into.size() < 500) into += text;
    ++git_gen_;
    dirty_ = true;
    return true;
}

bool App::git_panel_key(const SDL_KeyboardEvent& k) {
    GitUi& g = *gitui_;
    const bool cmd = k.mod & kPrimaryMod;
    auto changed = [&] {
        ++git_gen_;
        dirty_ = true;
        return true;
    };
    if (g.input) {
        std::string& text = g.input == 2 ? g.branch_name : g.message;
        switch (k.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (g.input == 2) {
                    if (!text.empty()) git_do(kActNewBranch, text);
                    g.input = 0;
                } else {
                    git_do(kActCommit);
                }
                return changed();
            case SDLK_ESCAPE: g.input = 0; return changed();
            case SDLK_BACKSPACE:
                while (!text.empty() && is_cont(static_cast<u8>(text.back()))) text.pop_back();
                if (!text.empty()) text.pop_back();
                return changed();
            default: return !cmd;
        }
    }
    // Up and down go from one thing that can be picked to the next.
    auto step = [&](int by) {
        for (int i = g.sel + by; i >= 0 && static_cast<size_t>(i) < g.rows.size(); i += by)
            if (g.rows[static_cast<size_t>(i)].kind >= kRowEntry && g.rows[static_cast<size_t>(i)].kind <= kRowCommit) {
                g.pick(i);
                if (static_cast<u32>(i) < g.top) g.top = static_cast<u32>(i);
                if (g.body_rows && static_cast<u32>(i) >= g.top + g.body_rows) g.top = static_cast<u32>(i) - g.body_rows + 1;
                return;
            }
    };
    const GitUi::Row* row = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    switch (k.key) {
        case SDLK_UP: step(-1); return changed();
        case SDLK_DOWN: step(1); return changed();
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (cmd) git_do(kActCommit);
            else if (row) git_do(row->kind == kRowEntry ? kActOpenDiff : row->kind == kRowBranch ? kActSwitch : kActShowCommit);
            return changed();
        case SDLK_RIGHT:  // towards "staged"
            if (row && row->kind == kRowEntry && !(row->index & kStagedBit)) git_do(kActStage);
            return changed();
        case SDLK_LEFT:
            if (row && row->kind == kRowEntry && (row->index & kStagedBit)) git_do(kActUnstage);
            return changed();
        case SDLK_DELETE:
        case SDLK_BACKSPACE:
            if (row && row->kind == kRowEntry && !(row->index & kStagedBit)) git_do(kActDiscard);
            else if (row && row->kind == kRowBranch && !g.branches[row->index].current && !g.branches[row->index].remote) git_do(kActDeleteBranch);
            return changed();
        case SDLK_F5: git_do(kActRefresh); return changed();
        default: break;
    }
    return false;
}

// A click in the panel while the Git tab shows; (row, col) are cells of the panel.
void App::git_panel_click(int row, int col, const SDL_MouseButtonEvent& b) {
    GitUi& g = *gitui_;
    if (b.button != SDL_BUTTON_LEFT || row < 2) return;
    ++git_gen_;
    dirty_ = true;
    const size_t i = g.top + static_cast<size_t>(row - 2);
    if (i >= g.rows.size()) {
        g.pick(-1);
        g.input = 0;
        return;
    }
    const GitUi::Row r = g.rows[i];
    if (r.kind >= kRowEntry && r.kind <= kRowCommit) g.pick(static_cast<int>(i));
    // A word that does something?
    for (const GitUi::Word& w : g.words)
        if (w.row == static_cast<u32>(row) && static_cast<u32>(col) >= w.c0 && static_cast<u32>(col) < w.c1) {
            if (g.input == 2 && w.action != kActNewBranch) g.input = 0;
            git_do(w.action);
            return;
        }
    switch (r.kind) {
        case kRowMessage: g.input = 1; break;
        case kRowHeader: g.fold[r.index] = !g.fold[r.index]; break;
        case kRowNote:
            g.note.clear();
            break;
        case kRowEntry:
            g.input = 0;
            if (b.clicks >= 2) git_do(kActOpenDiff);
            break;
        case kRowBranch:
            g.input = 0;
            if (b.clicks >= 2 && !g.branches[r.index].current) git_do(kActSwitch);
            break;
        case kRowCommit:
            g.input = 0;
            if (b.clicks >= 2) git_do(kActShowCommit);
            break;
        default: break;
    }
}

void App::git_panel_wheel(float dy) {
    GitUi& g = *gitui_;
    const i64 most = g.rows.size() > g.body_rows ? static_cast<i64>(g.rows.size() - g.body_rows) : 0;
    g.top = static_cast<u32>(std::clamp<i64>(static_cast<i64>(g.top) - std::lround(dy * 3), 0, most));
    ++git_gen_;
    dirty_ = true;
}

// Points git at the working directory: the folder the side panel's Files
// tab shows. Before there is one (the panel has not been opened, no folder
// chosen), at what that folder would be: the project of the file in front,
// or for a new file the folder blit was started in. Called every frame;
// does something only when one of those has changed.
void App::git_follow() {
    GitUi& g = *gitui_;
    if (!cfg_.git) return;
    const std::string* root = panel_root();
    if (g.started) {
        if (root && g.rooted && g.dir == *root) return;
        if (!root && !g.rooted && (g.doc == pane_->doc->id || pane_->doc->read_only)) return;  // (a diff tab is nobody's project)
    } else {
        git_.set_notify([] {  // on git's thread: wake the event loop
            SDL_Event ev{};
            ev.type = SDL_EVENT_USER;
            ev.user.code = 9;
            SDL_PushEvent(&ev);
        });
    }
    g.started = true;
    g.rooted = root != nullptr;
    g.doc = pane_->doc->id;
    std::string dir = root ? *root : default_workdir();
    if (dir == g.dir) return;
    g.dir = std::move(dir);
    g.known = false;
    git_.open(g.dir);
    ++git_gen_;
}

void App::git_command(std::string_view name, const std::string& arg) {
    static const struct {
        const char* name;
        u32 action;
    } known[] = {{"commit", kActCommit},   {"pull", kActPull},         {"push", kActPush},       {"fetch", kActFetch},
                 {"stage", kActStage},     {"unstage", kActUnstage},   {"discard", kActDiscard}, {"diff", kActOpenDiff},
                 {"open", kActOpenFile},   {"switch", kActSwitch},     {"merge", kActMerge},     {"delete", kActDeleteBranch},
                 {"branch", kActNewBranch}, {"stageall", kActStageAll}, {"abort", kActAbortMerge}, {"show", kActShowCommit},
                 {"blame", kActBlame},     {"refresh", kActRefresh},   {"cancel", kActCancel}};
    for (const auto& k : known)
        if (name == k.name) return git_do(k.action, arg);
    std::fprintf(stderr, "blit: no git action called %.*s\n", static_cast<int>(name.size()), name.data());
}

// What git has told the editor, on one line (for the tests).
std::string App::git_dump() {
    const GitUi& g = *gitui_;
    if (!g.status.repo) return "no repository";
    std::string out = g.status.branch.empty() ? "(no branch)" : g.status.branch;
    if (g.status.ahead) out += " +" + std::to_string(g.status.ahead);
    if (g.status.behind) out += " -" + std::to_string(g.status.behind);
    if (g.status.merging) out += " merging";
    auto list = [&](const char* label, auto&& wanted, auto&& letter) {
        std::string s;
        for (const GitEntry& e : g.status.entries)
            if (wanted(e)) s += std::string(" ") + letter(e) + ":" + e.path;
        if (!s.empty()) out += std::string(" | ") + label + s;
    };
    list("conflicts", [](const GitEntry& e) { return e.conflict; }, [](const GitEntry&) { return 'U'; });
    list("staged", [](const GitEntry& e) { return e.staged(); }, [](const GitEntry& e) { return e.index; });
    list("changes", [](const GitEntry& e) { return e.unstaged(); }, [](const GitEntry& e) { return e.index == '?' ? '?' : e.work; });
    out += " | branches";
    for (const GitBranch& b : g.branches) out += std::string(" ") + (b.current ? "*" : "") + b.name;
    out += " | commits " + std::to_string(g.log.size());
    if (!g.log.empty()) out += " \"" + g.log[0].subject + "\"";
    const Doc& d = *pane_->doc;
    if (!d.ed.path.empty()) {
        out += " | lines";
        for (const GitHunk& h : d.hunks)
            out += " " + std::to_string(h.line + 1) + (h.kind == GitHunk::kAdded ? "+" : h.kind == GitHunk::kModified ? "~" : "-") + (h.count > 1 ? std::to_string(h.count) : "");
        if (blame_shown(d) && d.blame && pane_->cur_line < d.blame->lines.size()) {  // (what shows: the note and the column)
            const GitBlame::Commit& c = d.blame->commits[d.blame->lines[pane_->cur_line]];
            out += " | blame " + (c.time ? c.author + ": " + c.summary : c.author);
        }
    }
    if (!g.note.empty()) out += " | note " + g.note;
    return out;
}

}  // namespace blit
