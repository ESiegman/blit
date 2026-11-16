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
#include "look.hpp"
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
    kActInit,  // git init in the folder, which is not a repository yet
    kActLook,  // the folder looked at afresh for a repository (one may have been made there)
};

// The Git tab's rows: the message field, a row for each thing to do, how the
// last command went, and the sections of files, branches and commits.
enum : u8 { kRowMessage, kRowAction, kRowNote, kRowHeader, kRowEntry, kRowBranch, kRowCommit, kRowBlank };
enum : u32 { kSecConflicts, kSecStaged, kSecChanges, kSecBranches, kSecHistory, kSecCount };
const char* const kSections[] = {"Conflicts", "Staged", "Changes", "Branches", "History"};
constexpr u32 kStagedBit = 0x80000000u;  // an entry row: the file's staged side
constexpr u32 kFieldTone = 1;            // the panel's tone for a field typed into (see panel_layout)

// Rows Up and Down go to (the message field is reached with Tab).
bool pickable(u8 kind) { return kind == kRowAction || kind == kRowEntry || kind == kRowBranch || kind == kRowCommit; }

// A row that is picked: lit as selected while the list has the keyboard,
// in the quieter tone of the mouse's while it does not.
void light_row(TextBox& box, u32 row, bool focused) {
    if (focused) return box.select_row(row);
    if (row >= box.rows) return;
    for (u32 c = 0; c < box.cols; ++c) box.cells[static_cast<size_t>(row) * box.cols + c] |= kCellHover;
}

// Text in lines of at most `cols` columns, broken between words.
std::vector<std::string> wrapped(std::string_view text, u32 cols) {
    std::vector<std::string> out(1);
    while (!text.empty()) {
        const size_t space = text.find(' ');
        const std::string_view word = text.substr(0, space);
        text = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
        if (!out.back().empty() && width_of(out.back()) + 1 + width_of(word) > cols) out.emplace_back();
        if (!out.back().empty()) out.back() += ' ';
        out.back() += fit_text(word, cols);
    }
    return out;
}

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
    // (The message field to start with: it has the keyboard, see `input`.)
    int sel = -1;
    u8 sel_kind = kRowMessage;
    bool sel_staged = false;
    std::string sel_name;
    std::string name_of_row(const Row& r) const {
        const u32 i = r.index & ~kStagedBit;
        if (r.kind == kRowEntry) return i < status.entries.size() ? status.entries[i].path : std::string();
        if (r.kind == kRowBranch) return i < branches.size() ? branches[i].name : std::string();
        if (r.kind == kRowCommit) return i < log.size() ? log[i].hash : std::string();
        if (r.kind == kRowAction) return std::to_string(r.index);
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
    // The first row of this kind (and index: an action's, a section's), or -1.
    int first(u8 kind, u32 index) const {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == kind && rows[i].index == index) return static_cast<int>(i);
        return -1;
    }
    u32 top = 0, body_rows = 0;
    u32 foot_row = 0;  // the panel's row the keys for what is picked are on (the last)
    bool fold[kSecCount] = {};
    // What has the keyboard while the tab does: 0 the list, 1 the commit
    // message (to start with), 2 a new branch's name.
    u8 input = 1;
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
    if (!cfg_.git) return;
    if (action == kActInit && !g.status.repo && g.known && !g.dir.empty()) {
        // As a new project's is made: in the build's output terminal, after
        // which the folder is looked at again (build_ended).
        build_command(g.dir, "git init -q", "git init");
        return;
    }
    if (action == kActLook) {
        if (g.status.repo) {
            git_.refresh();
        } else if (!g.dir.empty()) {  // (the same folder: pointed away and back, so that it is looked at afresh)
            git_.open({});
            git_.open(g.dir);
        }
        ++git_gen_;
        dirty_ = true;
        return;
    }
    if (!g.status.repo) return;
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
            if (text.empty()) {  // it needs saying what the commit is: the message field, in view
                panel_show_git();
                g.input = 1;
                g.pick(g.first(kRowMessage, 0));
                g.top = 0;
                g.note = "Type a message for the commit, then Enter";
                g.note_bad = false;
                break;
            }
            bool any_staged = false;
            for (const GitEntry& e : g.status.entries) any_staged |= e.staged();
            // With nothing staged, a commit takes every change to a file git already knows.
            // (The keyboard stays where it was: in the field, for the next message.)
            run(any_staged ? std::vector<std::string>{"commit", "-q", "-F", "-"} : std::vector<std::string>{"commit", "-q", "-a", "-F", "-"}, "Commit",
                text + "\n");
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
            if (arg.empty()) {  // its name, typed in its row
                panel_show_git();
                g.input = 2;
                g.branch_name.clear();
                g.pick(g.first(kRowAction, kActNewBranch));
                g.top = 0;
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
        text = fit_text(text, 120);
        TextBox box;
        box.resize(width_of(text), 1);
        put(box, *font_, 0, 0, text, ink::kMuted);  // (upright: the font's italic may be a script)
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
    if (!g.status.repo) {
        if (g.known && cfg_.git) g.rows.push_back({kRowAction, kActInit});  // (the one thing to do: make it one)
    } else {
        // The message, then a row for each thing to do (reached with Up and
        // Down like the files), then how the last of them went.
        g.rows.push_back({kRowMessage, 0});
        for (const u32 a : {kActCommit, kActPull, kActPush, kActFetch, kActStageAll, kActNewBranch, kActBlame}) g.rows.push_back({kRowAction, a});
        if (g.status.merging) g.rows.push_back({kRowAction, kActAbortMerge});
        if (!g.busy.empty() || !g.note.empty()) g.rows.push_back({kRowNote, 0});
        if (!g.busy.empty()) g.rows.push_back({kRowAction, kActCancel});
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
    }
    // (What was picked stays picked if it is still there.)
    g.sel = -1;
    for (size_t i = 0; i < g.rows.size() && g.sel_kind != kRowBlank; ++i) {
        const GitUi::Row& r = g.rows[i];
        if (r.kind == g.sel_kind && static_cast<bool>(r.index & kStagedBit) == (r.kind == kRowEntry && g.sel_staged) && g.name_of_row(r) == g.sel_name)
            g.sel = static_cast<int>(i);
    }
    if (!g.status.repo && !g.rows.empty() && g.sel < 0) g.sel = 0;  // (its one row, ready for Enter)
}

// Draws the tab into the panel's cells: the branch on the line under the
// panel's tabs, then rows that scroll, and on the last line the keys for
// what is picked.
void App::git_panel_draw(TextBox& box, bool focused) {
    GitUi& g = *gitui_;
    Font& font = *font_;
    const u32 cols = box.cols, rows = box.rows;
    constexpr u32 kHead = 2;
    g.foot_row = rows > kHead + 1 ? rows - 1 : rows;  // (no line of keys where there is no room for it)
    g.body_rows = g.foot_row > kHead ? g.foot_row - kHead : 0;
    g.words.clear();
    if (!cfg_.git) {
        put(box, font, 1, 0, fit_text("Git is switched off in the settings", cols), ink::kMuted);
        return;
    }
    if (g.rows_gen != git_gen_) git_rows();
    if (!g.status.repo && !g.known) {
        put(box, font, 1, 0, "Looking\xE2\x80\xA6", ink::kMuted);
        return;
    }
    const bool list = focused && (g.input == 0 || !g.status.repo);  // the list has the keyboard
    if (!g.status.repo) {
        // Say which folder was looked at: that is the first thing to check.
        const u32 c = put(box, font, 1, 0, "Not a git repository: ", ink::kMuted);
        put(box, font, 1, c, fit_text(name_of(g.dir), cols > c ? cols - c : 0), ink::kText);
    } else {
        // The branch, and how far it is from the one it follows.
        std::string counts;
        if (g.status.ahead) counts = "\xE2\x86\x91" + std::to_string(g.status.ahead);
        if (g.status.behind) counts += (counts.empty() ? "\xE2\x86\x93" : " \xE2\x86\x93") + std::to_string(g.status.behind);
        const u32 tail = (counts.empty() ? 0 : width_of(counts) + 1) + (g.status.merging ? 8 : 0);
        u32 c = put(box, font, 1, 0, "\xE2\x8E\x87 ", ink::kMuted);  // a branch sign
        const std::string branch = g.status.branch.empty() ? "(no branch) " + g.status.head : g.status.branch;
        c = put(box, font, 1, c, fit_text(branch, cols > c + tail ? cols - c - tail : 1), ink::kText);
        if (!counts.empty()) c = put(box, font, 1, c + 1, counts, ink::kMuted);
        if (g.status.merging) put(box, font, 1, c + 1, "merging", ink::kWarning);
    }

    g.top = std::min<u32>(g.top, g.rows.size() > g.body_rows ? static_cast<u32>(g.rows.size()) - g.body_rows : 0);
    // The row under the mouse: there, and on the picked one, a file's row
    // shows the sign that stages or unstages it.
    int hover = -1;
    if (win_ && SDL_GetMouseFocus() == win_ && mouse_x_ >= 0) {
        const int px = static_cast<int>(mouse_x_ * density_), py = static_cast<int>(mouse_y_ * density_), pad_y = look().pad_y;
        if (px < panel_px() && py >= pad_y) hover = (py - pad_y) / font.cell_h();
    }
    u32 staged = 0, tracked = 0;  // (what a commit takes: the staged files, else every change to a file git knows)
    for (const GitEntry& e : g.status.entries) staged += e.staged(), tracked += e.unstaged() && e.index != '?';
    // A field: the text typed (its end, where the next letter goes, while it
    // is typed), or what it is for; on the right the key that does it; all
    // on a tone of its own.
    auto field = [&](u32 r, const std::string& text, const char* empty, bool typing, Hint key) {
        const u32 key_w = hints_width({&key, 1}, font);
        const u32 room = cols >= key_w + 11 ? cols - key_w - 3 : cols;
        u32 end = 0;
        if (text.empty()) put(box, font, r, 0, fit_text(empty, room), ink::kMuted);
        else end = put(box, font, r, 0, typing ? fit_text(text, room > 1 ? room - 1 : 0, true) : fit_text(text, room), ink::kText);
        if (room < cols) put_hints(box, font, r, cols - key_w, {&key, 1});
        u32* line = box.cells.data() + static_cast<size_t>(r) * cols;
        for (u32 c = 0; c < cols; ++c) line[c] |= kCellSelected | kFieldTone << kCellDecoShift;
        if (typing && end < cols) line[end] = (line[end] & ~kCellDecoMask) | kCellSelected;  // where the next letter goes
    };
    // An action's row: what it does (the commit says what it will take), and
    // on the right its key, if it has one.
    auto action = [&](u32 r, u32 a) {
        std::string label;
        const char* key = "";
        Ink in = ink::kText;
        switch (a) {
            case kActCommit:
                key = "Cmd+Enter";
                if (staged) label = "Commit " + std::to_string(staged) + " staged";
                else if (tracked == 1) label = "Commit 1 change";
                else if (tracked) label = "Commit all " + std::to_string(tracked) + " changes";
                else label = "Nothing to commit";
                in = staged || tracked ? ink::kAccentStrong : ink::kMuted;  // (the tab's one button)
                break;
            case kActPull: label = "Pull"; break;
            case kActPush: label = g.status.upstream.empty() ? "Push to origin" : "Push"; break;
            case kActFetch: label = "Fetch"; break;
            case kActStageAll: label = "Stage all"; break;
            case kActNewBranch: label = "New branch\xE2\x80\xA6"; break;
            case kActBlame: label = "Blame"; key = "Cmd+Option+B"; break;
            case kActAbortMerge: label = "Abort merge"; break;
            case kActCancel: label = "Cancel"; break;
            case kActInit: label = "Make it a repository"; key = "git init"; break;
            default: break;
        }
        const std::string keys = a == kActInit ? std::string(key) : key_symbols(key, font);
        const u32 key_w = width_of(keys);
        put(box, font, r, 0, fit_text(label, cols > key_w + 2 ? cols - key_w - 2 : cols), in);
        if (key_w && key_w + 2 < cols) put(box, font, r, cols - key_w, keys, ink::kMuted);
    };
    for (u32 r = kHead; r < g.foot_row && g.top + (r - kHead) < g.rows.size(); ++r) {
        const size_t i = g.top + (r - kHead);
        const GitUi::Row& row = g.rows[i];
        bool lit = static_cast<int>(i) == g.sel;
        switch (row.kind) {
            case kRowMessage:
                field(r, g.message, "Message", focused && g.input == 1, {"Cmd+Enter", "commit"});
                lit = false;  // (the field is lit by its tone, and its caret while typed into)
                break;
            case kRowAction:
                if (row.index == kActNewBranch && g.input == 2) {
                    field(r, g.branch_name, "Branch name", focused, {"Enter", "make it"});
                    lit = false;
                } else {
                    action(r, row.index);
                }
                break;
            case kRowNote:
                if (!g.busy.empty()) put(box, font, r, 0, fit_text(g.busy + "\xE2\x80\xA6", cols), ink::kMuted);
                else put(box, font, r, 0, fit_text(g.note, cols), g.note_bad ? ink::kError : ink::kMuted);
                break;
            case kRowHeader: {
                u32 n = 0;
                if (row.index == kSecBranches) n = static_cast<u32>(g.branches.size());
                else if (row.index == kSecHistory) n = static_cast<u32>(g.log.size());
                else
                    for (const GitEntry& e : g.status.entries) n += row.index == kSecConflicts ? e.conflict : row.index == kSecStaged ? e.staged() : e.unstaged();
                put(box, font, r, 0, g.fold[row.index] ? "\xE2\x96\xB8" : "\xE2\x96\xBE", ink::kMuted);  // a small triangle, right or down
                const u32 c = put(box, font, r, 2, kSections[row.index], ink::kHeading);
                put(box, font, r, c + 1, std::to_string(n), ink::kMuted);
                break;
            }
            case kRowEntry: {
                const GitEntry& e = g.status.entries[row.index & ~kStagedBit];
                const bool is_staged = row.index & kStagedBit;
                const char letter = e.conflict ? 'U' : is_staged ? e.index : e.index == '?' ? '?' : e.work;
                const Ink in = letter == 'D' || letter == 'U' ? ink::kRemoved : letter == 'A' ? ink::kAdded : letter == '?' ? ink::kMuted : ink::kChanged;
                put(box, font, r, 2, std::string_view(&letter, 1), Ink{in.ink, Font::kBold});
                // The name, then the folder it is in, quieter; and on the
                // picked row and the one under the mouse, at the end, the
                // sign that stages or unstages it.
                const bool sign = cols > 8 && (lit || static_cast<int>(r) == hover);
                const u32 room = cols > 4 + (sign ? 2 : 0) ? cols - 4 - (sign ? 2 : 0) : 0;
                const std::string_view name = name_of(e.path);
                const u32 c = put(box, font, r, 4, fit_text(name, room), ink::kText);
                if (name.size() < e.path.size() && c + 2 < 4 + room)
                    put(box, font, r, c + 1, fit_text(std::string_view(e.path).substr(0, e.path.size() - name.size()), 4 + room - c - 1, true), ink::kMuted);
                if (sign) {
                    put(box, font, r, cols - 1, is_staged ? "\xE2\x88\x92" : "+", ink::kText);
                    g.words.push_back({r, cols - 2, cols, is_staged ? static_cast<u32>(kActUnstage) : static_cast<u32>(kActStage)});
                }
                break;
            }
            case kRowBranch: {
                const GitBranch& b = g.branches[row.index];
                if (b.current) put(box, font, r, 2, "*", ink::kAccentStrong);  // the one checked out: where you are
                put(box, font, r, 4, fit_text(b.name, cols > 4 ? cols - 4 : 0), b.current ? ink::kStrong : b.remote ? ink::kMuted : ink::kText);
                break;
            }
            case kRowCommit: {
                const GitCommit& c = g.log[row.index];
                const u32 at = put(box, font, r, 2, c.hash, ink::kMuted);
                put(box, font, r, at + 1, fit_text(c.subject, cols > at + 1 ? cols - at - 1 : 0), ink::kText);
                break;
            }
            default: break;
        }
        if (lit) light_row(box, r, list);
    }
    if (!g.status.repo) {
        // What git follows, under the row that makes the folder a repository.
        std::string dir = g.dir;
        if (const char* home = SDL_getenv("HOME"); home && *home && dir.starts_with(home)) dir = "~" + dir.substr(std::strlen(home));
        u32 r = kHead + static_cast<u32>(g.rows.size()) + 1;
        for (const std::string& line : wrapped("Git follows the project folder: the folder the Files tab shows, here " + dir + ".", cols))
            if (r < g.foot_row) put(box, font, r++, 0, line, ink::kMuted);
    }

    // The keys for what is picked (or typed), each a word to click: as many
    // as fit, the likeliest first.
    if (g.foot_row >= rows) return;
    struct Key {
        const char* keys;
        const char* what;
        u32 action;
    };
    Key keys[4];
    u32 n = 0;
    auto add = [&](const char* k, const char* what, u32 a = kActNone) { keys[n++] = {k, what, a}; };
    const GitUi::Row* picked = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    if (!g.status.repo) {
        add("Enter", "make it one", kActInit);
        add("Cmd+R", "look again", kActLook);
    } else if (g.input == 2) {
        add("Enter", "make it");
        add("Esc", "cancel");
    } else if (g.input == 1) {
        add("Cmd+Enter", "commit", kActCommit);
        add("Tab", "to the list");
    } else if (picked && picked->kind == kRowEntry) {
        const GitEntry& e = g.status.entries[picked->index & ~kStagedBit];
        if (picked->index & kStagedBit) {
            add("Enter", "diff", kActOpenDiff);
            add("Left", "unstage", kActUnstage);
            add("o", "open", kActOpenFile);
        } else if (e.index == '?' || e.conflict) {
            add("Enter", "open", kActOpenDiff);
            add("Right", "stage", kActStage);
        } else {
            add("Enter", "diff", kActOpenDiff);
            add("Right", "stage", kActStage);
            add("Backspace", "discard", kActDiscard);
            add("o", "open", kActOpenFile);
        }
    } else if (picked && picked->kind == kRowBranch && !g.branches[picked->index].current) {
        add("Enter", "switch", kActSwitch);
        add("m", "merge", kActMerge);
        if (!g.branches[picked->index].remote) add("Backspace", "delete", kActDeleteBranch);
    } else if (picked && picked->kind == kRowCommit) {
        add("Enter", "show", kActShowCommit);
        add("Tab", "message");
    } else {
        add("Tab", "message");
        add("Cmd+R", "refresh", kActRefresh);
    }
    u32 c = 0;
    for (u32 k = 0; k < n; ++k) {
        const std::string sym = key_symbols(keys[k].keys, font);
        const u32 at = c + (k ? 3 : 0);
        if (at + width_of(sym) + 1 + width_of(keys[k].what) > cols) break;
        c = put(box, font, g.foot_row, at, sym, ink::kKey);
        c = put(box, font, g.foot_row, c + 1, keys[k].what, ink::kHint);
        if (keys[k].action != kActNone) g.words.push_back({g.foot_row, at, c, keys[k].action});
    }
}

bool App::git_panel_text(std::string_view text) {
    GitUi& g = *gitui_;
    if (!g.status.repo || !g.input) return true;  // (the list's letters are keys, not text)
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
    if (!cfg_.git || (k.key == SDLK_TAB && (k.mod & SDL_KMOD_CTRL))) return false;  // (Ctrl+Tab: the panel's next tab)
    if (cmd && k.key == SDLK_R) {  // git asked again (F5 is the debugger's)
        git_do(g.status.repo ? kActRefresh : kActLook);
        return changed();
    }
    // Up and down go from one row that does something to the next.
    auto step = [&](int by) {
        for (int i = g.sel + by; i >= 0 && static_cast<size_t>(i) < g.rows.size(); i += by)
            if (pickable(g.rows[static_cast<size_t>(i)].kind)) {
                g.pick(i);
                if (static_cast<u32>(i) < g.top) g.top = static_cast<u32>(i);
                if (g.body_rows && static_cast<u32>(i) >= g.top + g.body_rows) g.top = static_cast<u32>(i) - g.body_rows + 1;
                return;
            }
    };
    // From the message to the list (to its first row, when the message was what was picked).
    auto to_list = [&] {
        g.input = 0;
        if (g.sel < 0 || static_cast<size_t>(g.sel) >= g.rows.size() || !pickable(g.rows[static_cast<size_t>(g.sel)].kind)) {
            g.sel = -1;
            step(1);
        }
    };
    if (g.input && g.status.repo) {
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
            case SDLK_ESCAPE:
            case SDLK_TAB:
                if (g.input == 2) g.input = 0;  // (the branch's name given up; the New branch row stays picked)
                else to_list();
                return changed();
            case SDLK_DOWN:
                if (g.input == 1) to_list();
                return changed();
            case SDLK_UP: return true;
            case SDLK_BACKSPACE:
                while (!text.empty() && is_cont(static_cast<u8>(text.back()))) text.pop_back();
                if (!text.empty()) text.pop_back();
                return changed();
            default: return !cmd;
        }
    }
    const GitUi::Row* row = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    const bool plain = !(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT));
    switch (k.key) {
        case SDLK_UP: step(-1); return changed();
        case SDLK_DOWN: step(1); return changed();
        case SDLK_HOME:
            g.sel = -1;
            step(1);
            return changed();
        case SDLK_END:
            g.sel = static_cast<int>(g.rows.size());
            step(-1);
            return changed();
        case SDLK_TAB:  // to the message
            if (!g.status.repo) return true;
            g.input = 1;
            g.pick(g.first(kRowMessage, 0));
            g.top = 0;
            return changed();
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (cmd) git_do(kActCommit);
            else if (row && row->kind == kRowAction) git_do(row->index);
            else if (row && pickable(row->kind)) git_do(row->kind == kRowEntry ? kActOpenDiff : row->kind == kRowBranch ? kActSwitch : kActShowCommit);
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
        case SDLK_O:  // the picked file itself
            if (!plain) break;
            if (row && row->kind == kRowEntry) git_do(kActOpenFile);
            return changed();
        case SDLK_M:  // the picked branch merged into the one checked out
            if (!plain) break;
            if (row && row->kind == kRowBranch && !g.branches[row->index].current) git_do(kActMerge);
            return changed();
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
    const bool on_row = static_cast<u32>(row) < g.foot_row && i < g.rows.size();
    // A word that does something (a sign that stages, a key at the bottom)?
    // A sign is about its row's file: that is picked first.
    for (const GitUi::Word& w : g.words)
        if (w.row == static_cast<u32>(row) && col >= 0 && static_cast<u32>(col) >= w.c0 && static_cast<u32>(col) < w.c1) {
            if (on_row && pickable(g.rows[i].kind)) g.pick(static_cast<int>(i));
            if (on_row || g.input == 2) g.input = 0;  // (a row's sign: the list has the keyboard; a key below leaves the message its own)
            git_do(w.action);
            return;
        }
    if (static_cast<u32>(row) >= g.foot_row) return;
    if (!on_row) {
        if (g.status.repo) g.input = 0;
        return;
    }
    const GitUi::Row r = g.rows[i];
    switch (r.kind) {
        case kRowMessage:
            g.input = 1;
            g.pick(static_cast<int>(i));
            break;
        case kRowAction:  // a row that does something is a button
            if (g.input == 2 && r.index == kActNewBranch) break;  // (its name is being typed there)
            g.input = 0;
            g.pick(static_cast<int>(i));
            git_do(r.index);
            break;
        case kRowHeader: g.fold[r.index] = !g.fold[r.index]; break;
        case kRowNote: g.note.clear(); break;
        case kRowEntry:
            g.input = 0;
            g.pick(static_cast<int>(i));
            if (b.clicks >= 2) git_do(kActOpenDiff);
            break;
        case kRowBranch:
            g.input = 0;
            g.pick(static_cast<int>(i));
            if (b.clicks >= 2 && !g.branches[r.index].current) git_do(kActSwitch);
            break;
        case kRowCommit:
            g.input = 0;
            g.pick(static_cast<int>(i));
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
                 {"blame", kActBlame},     {"refresh", kActRefresh},   {"cancel", kActCancel},   {"init", kActInit},
                 {"look", kActLook}};
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
