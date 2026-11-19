// Git in the editor: the Git tab of the side panel (what has changed, a
// commit message, push and pull, stashes, branches, tags, history), the
// marks in the line numbers for lines changed since the last commit (and
// what a click on one offers), blame, and tabs that show a diff, in one
// column or two, whose hunks and lines can be staged. git itself runs on
// its own thread (git.cpp); this file asks it things and puts the answers
// on screen.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
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
constexpr size_t kMessageMost = 10000;      // bytes a commit message may have
constexpr u32 kFieldMost = 8;               // rows the message field grows to (then it scrolls)

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
    kActStash,
    kActPop,
    kActApply,
    kActDrop,
    kActShowStash,
    kActRebase,     // the branch checked out, onto the one picked
    kActContinue,   // a rebase or a cherry-pick that stopped for conflicts, once they are settled
    kActAbortRebase,
    kActPick,       // a commit cherry-picked onto the branch checked out
    kActAbortPick,
    kActNewTag,
    kActShowTag,
    kActPushTag,
    kActDeleteTag,
    kActHistory,    // History shows the picked branch's commits (or, again, the checked out one's)
    kActStageHere,  // the change at the cursor (or the lines chosen), staged; in a diff tab or a file
    kActUnstageHere,
    kActShowHere,   // the diff of the file, at that change
    kActOpenTree,     // a working tree, or a submodule, opened as the folder worked in
    kActNewTree,      // a working tree made for a branch, beside the repository
    kActRemoveTree,   // the picked working tree taken away
    kActSubmodules,   // the submodules checked out at the commits the repository wants
};

// The Git tab's rows: the message field, a row for each thing to do, how the
// last command went, and the sections of files, stashes, branches, tags and
// commits.
enum : u8 { kRowMessage, kRowAction, kRowNote, kRowHeader, kRowEntry, kRowBranch, kRowCommit, kRowBlank, kRowStash, kRowTag, kRowTree, kRowModule };
enum : u32 { kSecConflicts, kSecStaged, kSecChanges, kSecStashes, kSecBranches, kSecTags, kSecTrees, kSecModules, kSecHistory, kSecCount };
const char* const kSections[] = {"Conflicts", "Staged", "Changes", "Stashes", "Branches", "Tags", "Worktrees", "Submodules", "History"};
constexpr u32 kStagedBit = 0x80000000u;  // an entry row: the file's staged side
constexpr u32 kFieldTone = 1;            // the panel's tone for a field typed into (see panel_layout)

// What a diff tab shows: a file's changes not staged yet, what of it is
// staged (both can be staged or unstaged a hunk or a line at a time), or
// something else (a commit, a stash), only to read.
enum : u8 { kViewChanges, kViewStaged, kViewOther };

// Rows Up and Down go to (the message field is reached with Tab).
bool pickable(u8 kind) {
    return kind == kRowAction || kind == kRowEntry || kind == kRowBranch || kind == kRowCommit || kind == kRowStash || kind == kRowTag || kind == kRowTree ||
           kind == kRowModule;
}

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

// The first line of what git said; of a line it wrote over (its progress,
// ended by a carriage return), the last of what it wrote.
std::string first_line(const std::string& s) {
    std::string line = s.substr(0, s.find('\n'));
    while (line.ends_with('\r')) line.pop_back();
    const size_t cr = line.rfind('\r');
    return cr == std::string::npos ? line : line.substr(cr + 1);
}

std::string_view name_of(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

i64 stamp_of(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) ? static_cast<i64>(info.modify_time) : 0;
}

// A row of the commit message as the field shows it: its bytes of the
// message, the line of the message it is part of, and the column of that
// line it starts at.
struct MsgRow {
    u32 b0 = 0, b1 = 0;
    u32 line = 0, col = 0;
};

// The message's rows: each of its lines broken, between words where it
// can be, into rows of at most `room` columns. An empty line is a row.
void wrap_message(const std::string& m, u32 room, std::vector<MsgRow>* out) {
    out->clear();
    room = std::max<u32>(room, 1);
    const u8* p = reinterpret_cast<const u8*>(m.data());
    u32 line = 0;
    for (size_t at = 0;;) {
        size_t end = m.find('\n', at);
        if (end == std::string::npos) end = m.size();
        size_t b0 = at;
        u32 col = 0;
        do {
            size_t i = b0, gap = std::string::npos;
            u32 w = 0;
            while (i < end) {
                const Decoded d = decode_utf8(p + i, end - i);
                const u32 cw = cp_width(d.cp);
                if (w + cw > room && i > b0) break;
                w += cw;
                i += d.len;
                if (d.cp == ' ') gap = i;
            }
            if (i < end && gap != std::string::npos && gap > b0) i = gap;  // (after the last space that fits)
            out->push_back({static_cast<u32>(b0), static_cast<u32>(i), line, col});
            col += width_of(std::string_view(m).substr(b0, i - b0));
            b0 = i;
        } while (b0 < end);
        if (end >= m.size()) break;
        at = end + 1;
        ++line;
    }
}

// The row the caret is on: the last that starts at or before it (so at the
// place a line is broken, the row after).
u32 caret_row(const std::vector<MsgRow>& rows, size_t caret) {
    u32 r = 0;
    for (u32 i = 0; i < rows.size(); ++i)
        if (rows[i].b0 <= caret) r = i;
    return r;
}

// What a file's diff tab shows: what is staged of it, or what is not. (The
// prefixes are spelt out: a patch of it goes back to git apply, whatever
// the user's settings for diffs.)
std::vector<std::string> diff_args(const std::string& rel, bool staged) {
    std::vector<std::string> args = {"diff", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/"};
    if (staged) args.push_back("--cached");
    args.insert(args.end(), {"--", rel});
    return args;
}

// The file a path names, from the top of a repository; empty if it is not
// in it. (Git names the top by its real path: so is a file here.)
// The columns of text a split shows, for laying a diff out in two (or a merge in three). (Less
// one: the scroll bar's strip is over the last.)
u32 text_cols(const Pane& p, int cell_w) { return static_cast<u32>(std::max(1, (p.x1 - p.text_x) / std::max(1, cell_w) - 1)); }

// A character as UTF-8 (the graph's are all below U+10000).
std::string utf8_of(char32_t c) {
    std::string out;
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) out += static_cast<char>(0xC0 | c >> 6), out += static_cast<char>(0x80 | (c & 0x3F));
    else out += static_cast<char>(0xE0 | c >> 12), out += static_cast<char>(0x80 | (c >> 6 & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
    return out;
}

// What points at a commit, from git's decoration ("HEAD -> main,
// origin/main, origin/HEAD, tag: v1"): the branch checked out, others, the
// server's (its HEAD left out) and tags.
struct GitRef {
    std::string name;
    bool head = false, remote = false, tag = false;
};
std::vector<GitRef> refs_of(std::string_view s) {
    std::vector<GitRef> out;
    while (!s.empty()) {
        const size_t comma = s.find(", ");
        std::string_view r = s.substr(0, comma);
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 2);
        GitRef ref;
        if (r.starts_with("HEAD -> ")) r.remove_prefix(8), ref.head = true;
        else if (r.starts_with("tag: ")) r.remove_prefix(5), ref.tag = true;
        else if (r.ends_with("/HEAD")) continue;
        else ref.remote = r.find('/') != std::string_view::npos && r != "HEAD";  // (a branch may have a / in its name: then it shows as the server's, quietly)
        ref.name = r;
        out.push_back(std::move(ref));
    }
    return out;
}

std::string in_repo(const std::string& root, std::string full) {
    char real[PATH_MAX];
    if (!is_remote(full) && realpath(full.c_str(), real)) full = real;
    if (root.empty() || !full.starts_with(root + "/")) return {};
    return full.substr(root.size() + 1);
}

}  // namespace

// A diff tab's own state (see Doc::gitview).
// A merge tab: a conflicted file's three versions, merged (GitMerge), what
// is taken of each conflict, and how it was laid out.
struct MergeView {
    std::string full, rel;  // the file
    std::shared_ptr<GitMerge> m;
    std::vector<u32> chunk_of_row;
    u32 cols = 0;
    bool asked = false;
};

// An interactive rebase's tab: the commits since the base, oldest first,
// each with what is to be done with it (as git's to-do says: pick, reword,
// edit, squash, fixup, drop) and, reworded, its new subject.
struct RebaseView {
    std::string base;
    struct Item {
        std::string hash, full, subject, body, action = "pick", reword;
    };
    std::vector<Item> items;
    bool asked = false;
};

struct GitView {
    u8 kind = kViewOther;
    std::string path;               // the file, from the top of the repository (changes and staged)
    std::vector<std::string> args;  // the git command whose output it shows
    bool two = false;               // in two columns
    u32 cols = 0;                   // the room those were laid out for
    bool asked = false;             // its text has been asked for, and is not here yet
    u32 goto_line = 0;              // when it comes: the cursor to the hunk with this line of the file (from 1)
    int keep_hunk = -1;             // or to this hunk (after one was staged, the one after it is in its place)
    std::shared_ptr<const std::string> source;  // the diff as git printed it
    std::shared_ptr<const GitDiff> diff;
    std::vector<u32> left, right;   // two columns: the diff's lines each line of the tab shows
    // The keys for the hunk the cursor is in, after its heading.
    std::vector<u32> note_cells;
    GridNote note;
    u64 note_key = 0;

    // The line of the diff a line of the tab shows (in two columns the
    // left's, else the right's), or ~0u.
    u32 diff_line(u64 row) const {
        if (left.empty()) return row < (diff ? diff->lines() : 0) ? static_cast<u32>(row) : ~0u;
        if (row >= left.size()) return ~0u;
        return left[row] != ~0u ? left[row] : right[row];
    }
    // The first line of the tab that shows this line of the diff, or one
    // after it. (In two columns those lines go up as the tab's do.)
    u64 row_of(u32 line) const {
        if (left.empty()) return line;
        size_t lo = 0, hi = left.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (diff_line(mid) < line) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    int hunk_at_row(u64 row) const {
        const u32 l = diff_line(row);
        return diff && l != ~0u ? diff->hunk_at(l) : -1;
    }
};

struct App::GitUi {
    GitStatus status;
    std::vector<GitBranch> branches;
    std::vector<GitCommit> log;
    std::string log_of;  // the branch History shows (empty: the one checked out; "--all": every one)
    std::string log_find;  // what History is searched for (empty: none)
    std::vector<GitGraphRow> graph;  // History's lanes, a row a commit (none while searched: what is found is not one line of descent)
    std::vector<std::string> tags;
    std::vector<GitStash> stashes;
    std::vector<GitWorktree> worktrees;  // (this one first)
    std::vector<GitSubmodule> submodules;
    // Where git was last pointed: the working directory, or while there
    // is none, the project of this document. `known`: git has answered.
    std::string dir;
    u64 doc = 0;
    bool started = false, rooted = false, known = false;
    std::string busy;   // the command that is running
    std::string note;   // how the last one went
    bool note_bad = false;
    bool two_columns = false;  // diff tabs open in two columns (as the last one was turned to)

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
        if (r.kind == kRowStash) return i < stashes.size() ? stashes[i].name : std::string();
        if (r.kind == kRowTag) return i < tags.size() ? tags[i] : std::string();
        if (r.kind == kRowTree) return i < worktrees.size() ? worktrees[i].path : std::string();
        if (r.kind == kRowModule) return i < submodules.size() ? submodules[i].path : std::string();
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
    // Sections folded away. Tags to start with: a repository may have hundreds.
    bool fold[kSecCount] = {false, false, false, false, false, true, false, false, false};
    // What has the keyboard while the tab does: 0 the list, 1 the commit
    // message (to start with), 2 a new branch's name, 3 a new tag's.
    u8 input = 1;
    std::string message, branch_name, tag_name;
    // The message: where the next letter goes, its rows as the field shows
    // them (broken at `wrap` columns), and which of them are on show.
    size_t caret = 0;
    std::vector<MsgRow> segs;
    u32 wrap = 0, field_most = kFieldMost, field_top = 0, field_rows = 1;

    // Blame of the cursor's line, as cells.
    std::vector<u32> note_cells;
    GridNote blame_note;
    u64 note_key = 0;

    // What a click on a change's mark in the gutter offers: its actions in
    // a small box beside it.
    struct Offer {
        bool open = false;
        u64 doc = 0;
        GitHunk hunk;
        u32 items[3] = {};
        u32 count = 0, sel = 0;
        int x = 0, y = 0;  // where it goes, in pixels
        TextBox box;
        u64 drawn = ~0ull;
        SDL_Rect rect{};
    } offer;
};

void App::git_create() { gitui_ = std::make_shared<GitUi>(); }

// The top of the repository being worked in (empty: none).
std::string App::git_root() const { return gitui_ && gitui_->status.repo ? gitui_->status.root : std::string(); }

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
    if (!win_) return asked_ = what, true;  // scripted runs have nobody to ask (the tests read what was asked)
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
                g.log_of = std::move(r.log_of);
                g.log_find = std::move(r.log_find);
                g.graph = g.log_find.empty() ? graph_lanes(g.log) : std::vector<GitGraphRow>{};
                g.tags = std::move(r.tags);
                g.stashes = std::move(r.stashes);
                g.worktrees = std::move(r.worktrees);
                g.submodules = std::move(r.submodules);
                shows |= panel_px() > 0;
                if (moved)  // another commit to compare with: ask again
                    for (const std::unique_ptr<Pane>& p : panes_)
                        for (const std::shared_ptr<Doc>& d : p->docs) d->hunks_asked = d->hunks_rev = d->blame_asked = d->blame_rev = ~0ull;
                // What is staged and what is not may be other than it was:
                // the diff tabs of files are read again (and change only if
                // their text does).
                for (const std::unique_ptr<Pane>& p : panes_)
                    for (const std::shared_ptr<Doc>& d : p->docs)
                        if (d->gitview && d->gitview->kind != kViewOther && !d->gitview->asked) git_view_ask(*d, false);
                break;
            }
            case Git::kAction: {
                g.note = r.ok ? r.label + ": done" : r.label + ": " + first_line(r.text);
                if (r.ok && !r.text.empty() && r.text.size() < 120 && r.text.find('\n') == std::string::npos) g.note = r.label + ": " + r.text;
                g.note_bad = !r.ok;
                if (!r.ok) std::fprintf(stderr, "blit: git %s: %s\n", r.label.c_str(), r.text.c_str());
                if (r.ok && r.label == "Commit") {
                    g.message.clear();
                    g.caret = 0;
                    g.field_top = 0;
                    wrap_message(g.message, g.wrap ? g.wrap : 40, &g.segs);
                    g.field_rows = 1;
                }
                shows = true;
                reload_changed();  // a pull, a merge, a change of branch: files may be other files now
                for (const std::unique_ptr<Pane>& p : panes_)
                    for (const std::shared_ptr<Doc>& d : p->docs) d->hunks_asked = d->hunks_rev = d->blame_asked = d->blame_rev = ~0ull;
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
                if (d->rebase) {  // the commits to rebase: hash, whole hash, subject, body, each record ending in \x1e
                    RebaseView& rv = *d->rebase;
                    rv.asked = false;
                    rv.items.clear();
                    std::string_view text = r.text;
                    while (r.ok && !text.empty()) {
                        const size_t end = text.find('\x1e');
                        std::string_view rec = text.substr(0, end);
                        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
                        while (!rec.empty() && (rec.front() == '\n' || rec.front() == '\r')) rec.remove_prefix(1);
                        RebaseView::Item it;
                        std::string* const fields[] = {&it.hash, &it.full, &it.subject, &it.body};
                        for (std::string* f : fields) {
                            const size_t sep = rec.find('\x1f');
                            *f = rec.substr(0, sep);
                            rec = sep == std::string_view::npos ? std::string_view{} : rec.substr(sep + 1);
                        }
                        while (!it.body.empty() && (it.body.back() == '\n' || it.body.back() == ' ')) it.body.pop_back();
                        if (!it.full.empty()) rv.items.push_back(std::move(it));
                    }
                    if (!r.ok || rv.items.empty()) {
                        d->rebase.reset();
                        d->measurer = {};
                        d->ed.open_bytes({}, r.ok ? std::string("Nothing to rebase onto ") + rv.base + "\n" : "git: " + r.text + "\n");
                        d->ed.mark_saved();
                        where->cache_rev = ~0ull;
                    } else {
                        d->ed.move_to(0, false);
                        git_rebase_lay(*d);
                    }
                    shows = true;
                    break;
                }
                d->ed.insert(r.ok ? r.text : "git: " + r.text + "\n");
                d->ed.move_to(0, false);
                d->ed.mark_saved();
                where->cache_rev = ~0ull;
                shows = true;
                break;
            }
            case Git::kMerge: {
                Pane* where = nullptr;
                Doc* d = find_doc(r.doc, &where);
                if (!d || !d->merge) break;
                d->merge->asked = false;
                if (!r.ok || !r.merge) {
                    d->measurer = {};
                    d->ed.open_bytes({}, "git: " + r.text + "\n");
                    d->ed.mark_saved();
                    where->cache_rev = ~0ull;
                    shows = true;
                    break;
                }
                d->merge->m = std::move(r.merge);
                // (The cursor to the first conflict.)
                i64 first = -1;
                for (size_t k = 0; k < d->merge->m->chunks.size() && first < 0; ++k)
                    if (d->merge->m->chunks[k].kind == GitMerge::Chunk::kConflict) first = static_cast<i64>(k);
                git_merge_lay(*d, first);
                shows = true;
                break;
            }
            case Git::kView: {
                Pane* where = nullptr;
                Doc* d = find_doc(r.doc, &where);
                if (!d || !d->gitview) break;
                git_view_set(*where, *d, r);
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
    if (d.gitview) return git_view_want(d);
    if (d.merge) {  // (laid out again for another width)
        if (d.merge->m && text_cols(*pane_, font_->cell_w()) != d.merge->cols && pane_->doc == &d) git_merge_lay(d, -1);
        return;
    }
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
        for (const std::shared_ptr<Doc>& d : p->docs) {
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
    remote_forget(d.id);
    d.measurer = {};
    std::string err;
    bool read = true;
    if (bytes) d.ed.open_bytes(d.ed.path, *bytes);
    else read = d.ed.open(d.ed.path.c_str(), &err);
    if (read && d.nb) nb_load(d);  // (a notebook: the script of its cells again)
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

// A new tab holding what a git command prints (a commit, a stash). The
// text arrives in a moment, see sync_git().
void App::git_open_text(std::vector<std::string> args, const std::string& title) { git_open_view(std::move(args), title, kViewOther, {}); }

// ---------------------------------------------------------------- diff tabs

namespace {


}  // namespace

// A tab for a diff (or a commit, or a stash). One already open for the
// same is shown instead, and read again. `goto_line`: when it comes, the
// cursor goes to the hunk with that line of the file in it.
void App::git_open_view(std::vector<std::string> args, const std::string& title, u8 kind, const std::string& path, u32 goto_line) {
    GitUi& g = *gitui_;
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i) {
            Doc& d = *p->docs[i];
            if (!d.gitview || d.gitview->args != args) continue;
            focus_pane(p.get());
            tab_show(i);
            panel_focus(false);
            d.gitview->goto_line = goto_line;
            git_view_ask(d, false);
            return;
        }
    Doc* d = add_doc(pane_->tab + 1);
    d->title = title;
    d->read_only = true;
    d->gitview = std::make_shared<GitView>();
    GitView& v = *d->gitview;
    v.kind = kind;
    v.path = path;
    v.args = std::move(args);
    v.two = g.two_columns;
    v.goto_line = goto_line;
    tab_show(pane_->tab + 1);
    panel_focus(false);
    git_view_ask(*d, false);
}

// Asks for a diff tab's text: from git (again), or with `again` the diff it
// has laid out once more (in one column or two, or for another width).
void App::git_view_ask(Doc& d, bool again) {
    GitView& v = *d.gitview;
    Pane* where = nullptr;
    find_doc(d.id, &where);
    v.asked = true;
    const u32 cols = v.two ? text_cols(where ? *where : *pane_, font_->cell_w()) : 0;
    git_.view(d.id, v.args, again ? v.source : nullptr, cols, d.ed.tab_width);
}

// A diff tab's text has come: it takes the place of what the tab showed (if
// it is another), with the cursor on the hunk it was on, or the one asked for.
void App::git_view_set(Pane& p, Doc& d, Git::Result& r) {
    GitView& v = *d.gitview;
    v.asked = false;
    if (!r.ok || !r.view) {
        if (v.source || d.ed.buf.size()) return;  // (it showed something: that stays)
        d.ed.insert("git: " + r.text + "\n");
        d.ed.move_to(0, false);
        d.ed.mark_saved();
        p.cache_rev = ~0ull;
        return;
    }
    GitDiffView& view = *r.view;
    const bool same = v.source && *v.source == *r.source && v.cols == view.cols && v.two == (view.cols > 0);
    const bool history = !v.args.empty() && v.args[0] == "log";  // (a history opens at its top: the newest commit's heading)
    if (!v.source && v.keep_hunk < 0 && !v.goto_line && !history) v.keep_hunk = 0;  // (opened: at the first hunk)
    if (same && !v.goto_line && v.keep_hunk < 0) return;  // (nothing to show that is not shown)
    // Where the cursor was: in which hunk, or on which line.
    const bool front = p.doc == &d && pane_ == &p;
    u64 row = d.ed.buf.line_of(d.ed.cursor());
    int hunk = v.keep_hunk >= 0 ? v.keep_hunk : v.hunk_at_row(row);
    v.keep_hunk = -1;
    if (!same) {
        if (front && cfg_.modal) {  // (Visual mode's ends are in the text that goes)
            vim_attach();
            if (vim_.visual()) {
                vim_.keys(d.ed, "<Esc>");
                vim_after(Vim::kHandled);
            }
        }
        d.measurer = {};  // (it may be reading the text that goes)
        d.ed.open_bytes({}, view.text);
        d.ed.mark_saved();
        auto h = std::make_unique<Highlights>();
        h->base = 0;
        h->end = view.text.size();
        h->off = std::move(view.off);
        h->style = std::move(view.style);
        h->seal();
        d.marks = std::move(h);
        d.widest_rev = d.indent_rev = ~0ull;
        v.source = r.source;
        v.diff = r.diff;
        v.left = std::move(view.left);
        v.right = std::move(view.right);
        v.cols = view.cols;
        v.two = view.cols > 0;
        v.note_key = 0;
    }
    const std::vector<GitDiff::Hunk>& hunks = v.diff->hunks;
    if (v.goto_line) {  // the hunk with that line of the file, or the first after it
        hunk = -1;
        for (size_t k = 0; k < hunks.size() && hunk < 0; ++k)
            if (hunks[k].new_start + hunks[k].new_count >= v.goto_line) hunk = static_cast<int>(k);
        if (hunk < 0 && !hunks.empty()) hunk = static_cast<int>(hunks.size()) - 1;
        v.goto_line = 0;
    }
    if (hunk >= 0 && !hunks.empty()) row = v.row_of(hunks[static_cast<size_t>(std::min<size_t>(static_cast<size_t>(hunk), hunks.size() - 1))].first);
    row = std::min<u64>(row, d.ed.buf.line_count() - 1);
    if (!same || d.ed.buf.line_of(d.ed.cursor()) != row) d.ed.move_to(d.ed.buf.line_start(row), false);
    p.cache_rev = ~0ull;
    if (p.doc == &d) {
        Pane* const keep = pane_;
        pane_ = &p;
        scroll_to_cursor();
        pane_ = keep;
    }
    pick_theme();
}

// Once a frame for a diff tab on show: two columns are laid out again when
// the split has become narrower than they are, or a good deal wider.
void App::git_view_want(Doc& d) {
    GitView& v = *d.gitview;
    if (!v.two || !v.source || v.asked) return;
    const u32 now = text_cols(*pane_, font_->cell_w());
    if (now < v.cols || now > v.cols + 2) git_view_ask(d, true);
}

// A diff tab in two columns, or back in one; and the tabs opened from now on too.
void App::git_view_toggle() {
    Doc& d = *pane_->doc;
    if (!d.gitview) return;
    GitView& v = *d.gitview;
    v.two = !v.two;
    gitui_->two_columns = v.two;
    v.keep_hunk = v.hunk_at_row(d.ed.buf.line_of(d.ed.cursor()));
    if (v.source) git_view_ask(d, true);
    dirty_ = true;
}

// Stages (or unstages) what is chosen at the cursor. In a diff tab: the
// lines selected, or with none, the hunk the cursor is in, through a patch
// of them (git apply --cached). In a file: the run of changed lines the
// cursor is on, as the text is now.
void App::git_stage_here(bool stage) {
    GitUi& g = *gitui_;
    if (!cfg_.git || !g.status.repo) return;
    Doc& d = *pane_->doc;
    const char* label = stage ? "Stage" : "Unstage";
    if (!d.gitview) {
        const u64 line = d.ed.buf.line_of(d.ed.cursor());
        for (const GitHunk& h : d.hunks)
            if ((h.kind == GitHunk::kDeleted && h.line == line) || (h.kind != GitHunk::kDeleted && line >= h.line && line < u64(h.line) + h.count)) {
                std::vector<Piece> pieces;
                d.ed.buf.snapshot(&pieces);
                d.git = &git_;
                g.busy = label;
                g.note.clear();
                git_.stage(d.id, full_path(d.ed.path), std::move(pieces), h.line, h.line + h.count, stage, label);
                ++git_gen_;
                dirty_ = true;
                return;
            }
        notify(std::string("No change at the cursor to ") + (stage ? "stage" : "unstage"));
        return;
    }
    GitView& v = *d.gitview;
    if (!v.diff || v.kind == kViewOther || (v.kind == kViewChanges) != stage) return;
    // The lines chosen: those selected (with Vim, Visual mode's), else the
    // hunk the cursor is in.
    std::vector<u8> chosen(v.diff->lines(), 0);
    u64 lo = d.ed.sel_min(), hi = d.ed.sel_max();
    if (cfg_.modal) {
        vim_attach();
        if (vim_selection(&lo, &hi)) {
            vim_.keys(d.ed, "<Esc>");
            vim_after(Vim::kHandled);
        } else {
            lo = hi = d.ed.cursor();
        }
    }
    const u64 r0 = d.ed.buf.line_of(lo);
    int hunk = v.hunk_at_row(r0);
    if (hi > lo) {
        u64 r1 = d.ed.buf.line_of(hi);
        if (r1 > r0 && d.ed.buf.line_start(r1) == hi) --r1;  // (a selection that ends at a line's start is of the lines before)
        for (u64 r = r0; r <= r1; ++r) {
            if (v.left.empty()) {
                if (r < chosen.size()) chosen[r] = 1;
                continue;
            }
            for (const u32 l : {v.left[r], v.right[r]})
                if (l < chosen.size()) chosen[l] = 1;
            if (hunk < 0) hunk = v.hunk_at_row(r);
        }
    } else if (hunk >= 0) {
        const GitDiff::Hunk& h = v.diff->hunks[static_cast<size_t>(hunk)];
        std::fill(chosen.begin() + h.first, chosen.begin() + h.last, 1);
    }
    const std::string patch = diff_patch(*v.source, *v.diff, chosen, !stage);
    if (patch.empty()) {
        notify(std::string("No change there to ") + (stage ? "stage" : "unstage"));
        return;
    }
    v.keep_hunk = std::max(hunk, 0);
    std::vector<std::string> args = {"apply", "--cached", "--recount", "--whitespace=nowarn"};
    if (!stage) args.push_back("-R");
    args.push_back("-");
    g.busy = label;
    g.note.clear();
    git_.run(std::move(args), label, patch);
    ++git_gen_;
    dirty_ = true;
}

// The keys of a diff tab in front, and of the box a change's mark offers.
bool App::git_view_key(const SDL_KeyboardEvent& k) {
    GitUi& g = *gitui_;
    if (g.offer.open) {
        GitUi::Offer& o = g.offer;
        dirty_ = true;
        switch (k.key) {
            case SDLK_UP: o.sel = (o.sel + o.count - 1) % o.count; return true;
            case SDLK_DOWN: o.sel = (o.sel + 1) % o.count; return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: {
                o.open = false;
                const u32 a = o.items[o.sel];
                if (Doc* d = find_doc(o.doc); d && d == pane_->doc) {
                    const GitHunk h = o.hunk;
                    if (a == kActShowHere) {
                        const std::string rel = in_repo(g.status.root, full_path(d->ed.path));
                        if (!rel.empty())
                            git_open_view(diff_args(rel, h.staged), std::string(name_of(rel)) + (h.staged ? " (staged)" : " (changes)"), h.staged ? kViewStaged : kViewChanges,
                                          rel, h.line + 1);
                    } else {
                        std::vector<Piece> pieces;
                        d->ed.buf.snapshot(&pieces);
                        d->git = &git_;
                        const bool stage = a == kActStageHere;
                        g.busy = stage ? "Stage" : "Unstage";
                        g.note.clear();
                        git_.stage(d->id, full_path(d->ed.path), std::move(pieces), h.line, h.line + h.count, stage, g.busy);
                        ++git_gen_;
                    }
                }
                term_drop_text_ = true;
                return true;
            }
            case SDLK_ESCAPE: o.open = false; return true;
            default: o.open = false; break;  // (any other key: the box goes, and the key does what it does)
        }
    }
    if (cfg_.git && !find_open_ && !panel_has_keyboard() && pane_->doc->merge) return git_merge_key(k);
    if (cfg_.git && !find_open_ && !panel_has_keyboard() && pane_->doc->rebase) return git_rebase_key(k);
    if (!cfg_.git || find_open_ || panel_has_keyboard() || !pane_->doc->gitview) return false;
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT)) return false;
    if (cfg_.modal) {  // (not while a command or a search is typed)
        vim_attach();
        const Vim::Mode m = vim_.mode();
        if (m == Vim::kCommand || m == Vim::kInsert || m == Vim::kReplace || vim_.awaiting_char() || !vim_.showcmd().empty()) return false;
    }
    Doc& d = *pane_->doc;
    GitView& v = *d.gitview;
    const bool shift = k.mod & SDL_KMOD_SHIFT;
    switch (k.key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (v.kind == kViewOther) {  // a history's: the commit there, all of it
                if (v.args.empty() || v.args[0] != "log") return false;
                const std::string commit = git_commit_at(d);
                if (commit.empty()) return false;
                git_open_view({"show", "--stat", "--patch", "--no-ext-diff", commit}, commit.substr(0, 10), kViewOther, {});
                break;
            }
            git_stage_here(v.kind == kViewChanges);
            break;
        case SDLK_S:
            if (shift || v.kind != kViewChanges) return false;
            git_stage_here(true);
            break;
        case SDLK_U:
            if (shift || v.kind != kViewStaged) return false;
            git_stage_here(false);
            break;
        case SDLK_RIGHTBRACKET:
        case SDLK_LEFTBRACKET: {  // to the next hunk, or the one before
            if (!v.diff || v.diff->hunks.empty() || shift) return false;
            const u64 row = d.ed.buf.line_of(d.ed.cursor());
            const std::vector<GitDiff::Hunk>& hs = v.diff->hunks;
            u64 to = row;
            if (k.key == SDLK_RIGHTBRACKET) {
                for (const GitDiff::Hunk& h : hs)
                    if (v.row_of(h.first) > row) {
                        to = v.row_of(h.first);
                        break;
                    }
            } else {
                for (size_t i = hs.size(); i-- > 0;)
                    if (v.row_of(hs[i].first) < row) {
                        to = v.row_of(hs[i].first);
                        break;
                    }
            }
            if (cfg_.modal && vim_.visual()) {
                vim_.keys(d.ed, "<Esc>");
                vim_after(Vim::kHandled);
            }
            d.ed.move_to(d.ed.buf.line_start(to), false);
            scroll_to_cursor();
            break;
        }
        case SDLK_PIPE:
        case SDLK_BACKSLASH:
            if (k.key == SDLK_BACKSLASH && !shift) return false;
            git_view_toggle();
            break;
        default: return false;
    }
    term_drop_text_ = true;  // (the letter is not typed after it)
    dirty_ = true;
    return true;
}

// A click on a change's mark beside the line numbers: a small box beside it
// offers to stage the change (or unstage it, if it is staged) and to show
// the file's diff there. A click anywhere else puts the box away.
bool App::git_gutter_click(const SDL_MouseButtonEvent& b) {
    GitUi& g = *gitui_;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (g.offer.open) {
        GitUi::Offer& o = g.offer;
        o.open = false;
        dirty_ = true;
        const SDL_Rect& r = o.rect;
        if (b.button == SDL_BUTTON_LEFT && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) {
            const int row = (py - r.y - look().pad_y) / font_->cell_h();
            if (row >= 0 && static_cast<u32>(row) < o.count) {
                o.sel = static_cast<u32>(row);
                o.open = true;
                SDL_KeyboardEvent enter{};
                enter.key = SDLK_RETURN;
                git_view_key(enter);
            }
            return true;
        }
    }
    if (b.button != SDL_BUTTON_LEFT || !cfg_.git || !cfg_.line_numbers || !g.status.repo) return false;
    Doc& d = *pane_->doc;
    if (d.hunks.empty() || d.read_only || d.ed.path.empty()) return false;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    if (px < pane_->text_x - cw || px >= pane_->text_x || py < pane_->text_y || py >= pane_->y1) return false;
    const i64 y = py - pane_->text_y + std::llround(pane_->scroll_y);
    if (y < 0) return false;
    const u64 lines = d.ed.buf.line_count(), line = static_cast<u64>(y / ch) < view_line_count() ? line_of_row(static_cast<u64>(y / ch)) : lines;  // (rows, with lines folded)
    if (line >= lines) return false;
    // The run of lines the mark there is for (as the gutter draws them).
    const GitHunk* hit = nullptr;
    for (const GitHunk& h : d.hunks) {
        if (h.kind == GitHunk::kDeleted ? (h.line == line || (h.line >= lines && line + 1 == lines)) : line >= h.line && line < u64(h.line) + h.count) hit = &h;
        if (hit) break;
    }
    if (!hit) return false;
    GitUi::Offer& o = g.offer;
    o.open = true;
    o.doc = d.id;
    o.hunk = *hit;
    o.count = 0;
    o.items[o.count++] = hit->staged ? kActUnstageHere : kActStageHere;
    o.items[o.count++] = kActShowHere;
    o.sel = 0;
    o.drawn = ~0ull;
    o.x = pane_->text_x;
    o.y = pane_->text_y + static_cast<int>(static_cast<i64>(line + 1) * ch - std::llround(pane_->scroll_y));
    lsp_close(kPopAll);
    dirty_ = true;
    return true;
}

// The box a change's mark offers, under the line it was clicked on.
void App::git_offer_layout(FrameDesc& fd) {
    GitUi::Offer& o = gitui_->offer;
    if (!o.open) return;
    if (pane_->doc->id != o.doc || fd.popup_count >= std::size(fd.popups)) {
        o.open = false;
        return;
    }
    Font& font = *font_;
    const Look l = look();
    const int cw = font.cell_w(), ch = font.cell_h();
    const u64 key = (static_cast<u64>(font.generation()) << 32) | (o.sel << 4) | o.items[0];
    if (key != o.drawn) {
        o.drawn = key;
        auto label = [](u32 a) {
            return a == kActStageHere ? "Stage this change" : a == kActUnstageHere ? "Unstage this change" : "Show the diff";
        };
        const std::string enter = key_symbols("Enter", font);
        u32 widest = 0;
        for (u32 i = 0; i < o.count; ++i) widest = std::max(widest, width_of(label(o.items[i])));
        const u32 cols = widest + 3 + width_of(enter);
        o.box.resize(cols, o.count);
        for (u32 i = 0; i < o.count; ++i) {
            put(o.box, font, i, 0, label(o.items[i]), i == 0 ? ink::kText : ink::kText);
            if (i == o.sel) {
                put(o.box, font, i, cols - width_of(enter), enter, ink::kMuted);
                o.box.select_row(i);
            }
        }
    }
    Popup& p = fd.popups[fd.popup_count++];
    p.cols = o.box.cols;
    p.rows = o.box.rows;
    p.cells = o.box.cells.data();
    p.pad_x = l.edge;
    p.pad_y = l.pad_y;
    p.w = static_cast<int>(p.cols) * cw + 2 * p.pad_x;
    p.h = static_cast<int>(p.rows) * ch + 2 * p.pad_y;
    p.x = std::clamp(o.x, 0, std::max(0, px_w_ - p.w));
    p.y = std::clamp(o.y + l.gap, 0, std::max(0, px_h_ - p.h));
    p.border_px = static_cast<float>(l.hair);
    o.rect = {p.x, p.y, p.w, p.h};
    fx_float(p, kFxMenu);
}

// In a diff tab whose changes can be staged: after the heading of the hunk
// the cursor is in, the key that stages it (or unstages it).
void App::git_view_note(FrameDesc& fd) {
    Doc& d = *pane_->doc;
    if (!d.gitview || focus_view_ >= fd.view_count || panel_has_keyboard()) return;
    GitView& v = *d.gitview;
    if (!v.diff || v.kind == kViewOther) return;
    GridSpec& grid = fd.views[focus_view_].grid;
    if (!grid.notes.empty()) return;
    const int hunk = v.hunk_at_row(pane_->cur_line);
    if (hunk < 0) return;
    const bool lines = pane_->has_sel;
    const u64 head = v.row_of(v.diff->hunks[static_cast<size_t>(hunk)].first);
    if (head >= d.ed.buf.line_count()) return;
    const u64 key = ((static_cast<u64>(hunk) * 4 + (lines ? 2 : 0) + v.kind) << 16) ^ (static_cast<u64>(font_->generation()) << 48) ^ d.id;
    if (key != v.note_key) {
        v.note_key = key;
        const char* what = v.kind == kViewChanges ? (lines ? "stage the lines" : "stage") : (lines ? "unstage the lines" : "unstage");
        const Hint hint{"Enter", what};
        TextBox box;
        box.resize(hints_width({&hint, 1}, *font_), 1);
        put_hints(box, *font_, 0, 0, {&hint, 1});
        v.note_cells = std::move(box.cells);
    }
    const u64 start = d.ed.buf.line_start(head);
    v.note = {head, d.ed.col_between(start, d.ed.line_end(head)) + 4, v.note_cells.data(), static_cast<u32>(v.note_cells.size())};
    grid.notes = {&v.note, 1};
}

// ---------------------------------------------------------------- interactive rebase

// The commits since `base` (the merges left out, as git rebase does), oldest
// first, in a tab: p picks the commit at the cursor, r rewords it (its new
// subject asked for), e stops at it to be changed, s squashes it into the
// one before, f fixes it up into that one (its message dropped), d drops it;
// Alt+Up and Alt+Down move it. Cmd+S starts the rebase: blit is git's
// sequence editor, and writes the to-do. A stop (edit, a conflict) is
// handled as a rebase's stops are: continued or abandoned from the Git tab.
namespace {

std::string_view trimmed_view(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

void App::git_rebase_open(const std::string& base) {
    GitUi& g = *gitui_;
    if (!cfg_.git || !g.status.repo) return notify("Not in a git repository");
    if (g.status.rebasing || g.status.merging || g.status.picking) return notify("A rebase, merge or cherry-pick is under way");
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->rebase) {  // (one at a time: that one, onto this base)
                focus_pane(p.get());
                tab_show(i);
                panel_focus(false);
                Doc& d = *p->docs[i];
                d.rebase->base = base;
                d.rebase->asked = true;
                d.title = "Rebase onto " + base;
                git_.text(d.id, {"log", "--reverse", "--no-merges", "--format=%h%x1f%H%x1f%s%x1f%b%x1e", base + "..HEAD", "--"}, "Rebase");
                return;
            }
    Doc* d = add_doc(pane_->tab + 1);
    d->title = "Rebase onto " + base;
    d->read_only = true;
    d->rebase = std::make_shared<RebaseView>();
    d->rebase->base = base;
    d->rebase->asked = true;
    git_.text(d->id, {"log", "--reverse", "--no-merges", "--format=%h%x1f%H%x1f%s%x1f%b%x1e", base + "..HEAD", "--"}, "Rebase");
    tab_show(pane_->tab + 1);
    panel_focus(false);
}

// The tab's text: a line a commit, what is done with it first (in the
// accent, unless picked; a dropped one muted, and those folded into the one
// before quieter too).
void App::git_rebase_lay(Doc& d) {
    const RebaseView& rv = *d.rebase;
    const u64 row = d.ed.buf.line_of(d.ed.cursor());
    std::string text;
    std::vector<u32> off;
    std::vector<u8> style;
    auto mark = [&](u8 st) {
        const u32 at = static_cast<u32>(text.size());
        if (!style.empty() && style.back() == st) return;
        if (!off.empty() && off.back() == at) style.back() = st;
        else off.push_back(at), style.push_back(st);
    };
    for (const RebaseView::Item& it : rv.items) {
        const bool drop = it.action == "drop", folded = it.action == "squash" || it.action == "fixup";
        mark(drop ? 4 : it.action == "pick" ? 0 : 3);
        text += it.action;
        text.append(7 - std::min<size_t>(7, it.action.size()), ' ');
        mark(4);
        text += it.hash + "  ";
        mark(drop || folded ? 4 : 0);
        text += it.reword.empty() ? it.subject : it.reword;
        if (!it.reword.empty()) mark(4), text += "  (was: " + it.subject + ")";
        text += '\n';
    }
    if (!text.empty()) text.pop_back();
    d.measurer = {};
    d.ed.open_bytes({}, text);
    d.ed.mark_saved();
    auto h = std::make_unique<Highlights>();
    h->base = 0;
    h->end = text.size();
    h->off = std::move(off);
    h->style = std::move(style);
    h->seal();
    d.marks = std::move(h);
    d.style_bits[1] = kInkAdded << kCellInkShift;
    d.style_bits[2] = kInkDiagnostic << kCellInkShift;
    d.style_bits[3] = kInkAccent << kCellInkShift;
    d.style_bits[4] = kInkMuted << kCellInkShift;
    d.widest_rev = d.indent_rev = ~0ull;
    d.ed.move_to(d.ed.buf.line_start(std::min<u64>(row, d.ed.buf.line_count() - 1)), false);
    Pane* where = nullptr;
    find_doc(d.id, &where);
    if (where) {
        where->cache_rev = ~0ull;
        if (where->doc == &d) {
            Pane* const keep = pane_;
            pane_ = where;
            scroll_to_cursor();
            pane_ = keep;
        }
    }
    dirty_ = true;
}

bool App::git_rebase_key(const SDL_KeyboardEvent& k) {
    Doc& d = *pane_->doc;
    RebaseView& rv = *d.rebase;
    if ((k.mod & kPrimaryMod) && k.key == SDLK_S) {
        git_rebase_start();
        term_drop_text_ = true;
        return true;
    }
    if (rv.items.empty()) return false;
    const u64 row = std::min<u64>(d.ed.buf.line_of(d.ed.cursor()), rv.items.size() - 1);
    if ((k.mod & SDL_KMOD_ALT) && (k.key == SDLK_UP || k.key == SDLK_DOWN)) {  // the commit moved
        const u64 to = k.key == SDLK_UP ? (row ? row - 1 : row) : std::min<u64>(row + 1, rv.items.size() - 1);
        if (to != row) {
            std::swap(rv.items[row], rv.items[to]);
            git_rebase_lay(d);
            d.ed.move_to(d.ed.buf.line_start(to), false);
            scroll_to_cursor();
        }
        term_drop_text_ = true;
        return true;
    }
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT | SDL_KMOD_SHIFT)) return false;
    if (cfg_.modal) {  // (not while a command or a search is typed)
        vim_attach();
        const Vim::Mode m = vim_.mode();
        if (m == Vim::kCommand || m == Vim::kInsert || m == Vim::kReplace || vim_.awaiting_char() || !vim_.showcmd().empty()) return false;
    }
    const char* action = nullptr;
    switch (k.key) {
        case SDLK_P: action = "pick"; break;
        case SDLK_E: action = "edit"; break;
        case SDLK_S: action = "squash"; break;
        case SDLK_F: action = "fixup"; break;
        case SDLK_D: action = "drop"; break;
        case SDLK_R: {  // a new subject for it
            const u64 at = row;
            const std::string was = rv.items[at].reword.empty() ? rv.items[at].subject : rv.items[at].reword;
            find_choose(kFindInput, "Subject", was, [this, id = d.id, at](const std::string& typed) {
                Doc* doc = find_doc(id);
                if (!doc || !doc->rebase || at >= doc->rebase->items.size() || trimmed_view(typed).empty()) return;
                RebaseView::Item& it = doc->rebase->items[at];
                it.action = "reword";
                it.reword = std::string(trimmed_view(typed));
                if (it.reword == it.subject) it.reword.clear(), it.action = "pick";
                git_rebase_lay(*doc);
            });
            term_drop_text_ = true;
            return true;
        }
        default: return false;
    }
    if ((std::string_view(action) == "squash" || std::string_view(action) == "fixup") && row == 0) {
        notify("Nothing before it to fold it into");
    } else {
        rv.items[row].action = action;
        rv.items[row].reword.clear();
        git_rebase_lay(d);
    }
    term_drop_text_ = true;
    dirty_ = true;
    return true;
}

namespace {

// Text quoted for sh, whatever is in it.
std::string sh_quote(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

}  // namespace

// The rebase started, with this to-do: git's sequence editor is `cat >`,
// which writes it into git's file from what blit sends it (so that it
// works where the repository is on another machine too). A reworded commit
// is picked, then its message amended by an exec, the new subject over the
// old body.
void App::git_rebase_start() {
    Doc& d = *pane_->doc;
    if (!d.rebase || d.rebase->items.empty() || d.rebase->asked) return;
    const RebaseView rv = *d.rebase;
    std::string todo;
    for (const RebaseView::Item& it : rv.items) {
        todo += (it.action == "reword" ? std::string("pick") : it.action) + " " + it.full + " " + it.subject + "\n";
        if (it.action == "reword") {
            std::string printf_args = sh_quote(it.reword);
            if (!it.body.empty()) {
                printf_args += " ''";
                size_t at = 0;
                while (at <= it.body.size()) {
                    size_t nl = it.body.find('\n', at);
                    if (nl == std::string::npos) nl = it.body.size();
                    printf_args += " " + sh_quote(std::string_view(it.body).substr(at, nl - at));
                    at = nl + 1;
                }
            }
            todo += "exec printf '%s\\n' " + printf_args + " | git commit --amend --only --no-verify --cleanup=verbatim -F -\n";
        }
    }
    GitUi& g = *gitui_;
    g.busy = "Rebase";
    g.note.clear();
    git_.run({"-c", "sequence.editor=cat >", "rebase", "-i", rv.base}, "Rebase", todo);
    ++git_gen_;
    Pane* where = nullptr;
    size_t at = 0;
    if (find_doc(d.id, &where, &at) && where == pane_) tab_close(at, true);
}

// The rebase tab in front, for the tests: each commit's action and subject.
std::string App::git_rebase_dump() {
    const Doc& d = *pane_->doc;
    if (!d.rebase) return "no rebase";
    std::string out;
    for (const RebaseView::Item& it : d.rebase->items) out += (out.empty() ? "" : "; ") + it.action + " " + (it.reword.empty() ? it.subject : it.reword);
    return out;
}

// ---------------------------------------------------------------- the merge tool

// A conflicted file in a merge tab: theirs, the result and yours side by
// side. In it, t takes theirs for the conflict at the cursor, y yours, b
// both (yours first; B theirs first), e each side's changes where they only
// touch, x opens it again, a takes each side's in every such simple one;
// ] and [ go to the next conflict and the one before. Cmd+S writes the
// result to the file, and stages it once no conflict is open.
void App::git_merge_open(const std::string& full) {
    GitUi& g = *gitui_;
    if (!cfg_.git || !g.status.repo) return notify("Not in a git repository");
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->merge && p->docs[i]->merge->full == full) {
                focus_pane(p.get());
                tab_show(i);
                panel_focus(false);
                return;
            }
    const std::string rel = in_repo(g.status.root, full);
    if (rel.empty()) return notify("Not in the repository");
    Doc* d = add_doc(pane_->tab + 1);
    d->title = std::string(name_of(rel)) + " (merge)";
    d->read_only = true;
    d->merge = std::make_shared<MergeView>();
    d->merge->full = full;
    d->merge->rel = rel;
    d->merge->asked = true;
    git_.merge(d->id, full);
    tab_show(pane_->tab + 1);
    panel_focus(false);
}

// The merge laid out (again) as wide as the split, the cursor where it was,
// or at the first row of chunk `chunk`.
void App::git_merge_lay(Doc& d, i64 chunk) {
    MergeView& mv = *d.merge;
    if (!mv.m) return;
    Pane* where = nullptr;
    find_doc(d.id, &where);
    const u32 cols = text_cols(where ? *where : *pane_, font_->cell_w());
    GitMergeView view;
    merge_view(*mv.m, cols, d.ed.tab_width, &view);
    const u64 row = d.ed.buf.line_of(d.ed.cursor());
    mv.cols = cols;
    mv.chunk_of_row = std::move(view.chunk_of_row);
    d.measurer = {};  // (it may be reading the text that goes)
    if (cfg_.modal && where == pane_ && pane_->doc == &d) {  // (Visual mode's ends are in the text that goes)
        vim_attach();
        if (vim_.visual()) {
            vim_.keys(d.ed, "<Esc>");
            vim_after(Vim::kHandled);
        }
    }
    d.ed.open_bytes({}, view.text);
    d.ed.mark_saved();
    auto h = std::make_unique<Highlights>();
    h->base = 0;
    h->end = view.text.size();
    h->off = std::move(view.off);
    h->style = std::move(view.style);
    h->seal();
    d.marks = std::move(h);
    d.style_bits[1] = kInkAdded << kCellInkShift;
    d.style_bits[2] = kInkDiagnostic << kCellInkShift;
    d.style_bits[3] = kInkAccent << kCellInkShift;
    d.style_bits[4] = kInkMuted << kCellInkShift;
    d.widest_rev = d.indent_rev = ~0ull;
    u64 to = std::min<u64>(row, d.ed.buf.line_count() - 1);
    if (chunk >= 0)
        for (size_t r = 0; r < mv.chunk_of_row.size(); ++r)
            if (mv.chunk_of_row[r] == static_cast<u32>(chunk)) {
                to = r;
                break;
            }
    d.ed.move_to(d.ed.buf.line_start(to), false);
    if (where) {
        where->cache_rev = ~0ull;
        if (where->doc == &d) {
            Pane* const keep = pane_;
            pane_ = where;
            scroll_to_cursor();
            pane_ = keep;
        }
    }
    dirty_ = true;
}

bool App::git_merge_key(const SDL_KeyboardEvent& k) {
    Doc& d = *pane_->doc;
    MergeView& mv = *d.merge;
    if ((k.mod & kPrimaryMod) && k.key == SDLK_S) {
        git_merge_save();
        term_drop_text_ = true;
        return true;
    }
    if (!mv.m || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT))) return false;
    if (cfg_.modal) {  // (not while a command or a search is typed)
        vim_attach();
        const Vim::Mode m = vim_.mode();
        if (m == Vim::kCommand || m == Vim::kInsert || m == Vim::kReplace || vim_.awaiting_char() || !vim_.showcmd().empty()) return false;
    }
    const bool shift = k.mod & SDL_KMOD_SHIFT;
    using C = GitMerge::Chunk;
    switch (k.key) {
        case SDLK_T: git_merge_take(C::kTakeTheirs, false); break;
        case SDLK_Y: git_merge_take(C::kTakeYours, false); break;
        case SDLK_B: git_merge_take(shift ? C::kTakeTheirsYours : C::kTakeYoursTheirs, false); break;
        case SDLK_E: git_merge_take(C::kTakeEach, false); break;
        case SDLK_X: git_merge_take(C::kOpen, false); break;
        case SDLK_A: git_merge_take(C::kTakeEach, true); break;
        case SDLK_RIGHTBRACKET:
        case SDLK_LEFTBRACKET: {  // to the next conflict, or the one before
            if (shift) return false;
            const u64 row = d.ed.buf.line_of(d.ed.cursor());
            const u32 here = row < mv.chunk_of_row.size() ? mv.chunk_of_row[row] : ~0u;
            const std::vector<C>& cs = mv.m->chunks;
            i64 to = -1;
            if (k.key == SDLK_RIGHTBRACKET) {
                for (size_t c = here == ~0u ? 0 : here + 1; c < cs.size() && to < 0; ++c)
                    if (cs[c].kind == C::kConflict) to = static_cast<i64>(c);
            } else {
                for (size_t c = here == ~0u ? 0 : here; c-- > 0 && to < 0;)
                    if (cs[c].kind == C::kConflict) to = static_cast<i64>(c);
            }
            if (to < 0) break;
            for (size_t r = 0; r < mv.chunk_of_row.size(); ++r)
                if (mv.chunk_of_row[r] == static_cast<u32>(to)) {
                    if (cfg_.modal && vim_.visual()) {
                        vim_.keys(d.ed, "<Esc>");
                        vim_after(Vim::kHandled);
                    }
                    d.ed.move_to(d.ed.buf.line_start(r), false);
                    scroll_to_cursor();
                    break;
                }
            break;
        }
        default: return false;
    }
    term_drop_text_ = true;  // (the letter is not typed after it)
    dirty_ = true;
    return true;
}

// The conflict at the cursor taken as `take` (or, all_simple, each side's
// changes in every simple conflict still open).
void App::git_merge_take(u8 take, bool all_simple) {
    Doc& d = *pane_->doc;
    if (!d.merge || !d.merge->m) return;
    MergeView& mv = *d.merge;
    using C = GitMerge::Chunk;
    if (all_simple) {
        u32 n = 0;
        for (C& c : mv.m->chunks)
            if (c.kind == C::kConflict && c.simple && c.take == C::kOpen) c.take = C::kTakeEach, ++n;
        if (n == 0) return notify("No simple conflicts to resolve");
        git_merge_lay(d, -1);
        return;
    }
    const u64 row = d.ed.buf.line_of(d.ed.cursor());
    const u32 at = row < mv.chunk_of_row.size() ? mv.chunk_of_row[row] : ~0u;
    if (at == ~0u || mv.m->chunks[at].kind != C::kConflict) return notify("No conflict at the cursor");
    C& c = mv.m->chunks[at];
    if (take == C::kTakeEach && !c.simple) return notify("Both sides changed the same lines here");
    c.take = static_cast<C::Take>(take);
    git_merge_lay(d, at);
}

// The result written to the file; staged once no conflict is open.
void App::git_merge_save() {
    Doc& d = *pane_->doc;
    if (!d.merge || !d.merge->m) return;
    const MergeView& mv = *d.merge;
    std::string err;
    if (!fs_write(mv.full, mv.m->result(), &err)) return notify("Cannot write " + mv.rel + ": " + err);
    reload_changed();  // (the file, where it is open)
    const u32 open = mv.m->open();
    if (open) return notify("Saved, with " + std::to_string(open) + (open == 1 ? " conflict" : " conflicts") + " open: not staged");
    GitUi& g = *gitui_;
    g.busy = "Stage";
    g.note.clear();
    git_.run({"add", "--", mv.rel}, "Stage");
    ++git_gen_;
    notify("Saved and staged " + mv.rel);
}

// The merge tab in front, for the tests: each chunk that is a change (its
// kind and what is taken), and how many conflicts are open.
std::string App::git_merge_dump() {
    const Doc& d = *pane_->doc;
    if (!d.merge || !d.merge->m) return "no merge";
    static const char* const kKind[] = {"same", "yours", "theirs", "both", "conflict"};
    static const char* const kTake[] = {"open", "theirs", "yours", "yours+theirs", "theirs+yours", "each"};
    std::string out;
    for (const GitMerge::Chunk& c : d.merge->m->chunks) {
        if (c.kind == GitMerge::Chunk::kSame) continue;
        out += (out.empty() ? "" : " ") + std::string(kKind[c.kind]);
        if (c.kind == GitMerge::Chunk::kConflict) out += std::string(c.simple ? "(simple)" : "") + ":" + kTake[c.take];
    }
    return out + " | open " + std::to_string(d.merge->m->open());
}

// ---------------------------------------------------------------- comparing with a revision

// "Git: Compare the file with…": the file in front against a branch, a tag
// or a commit chosen from a list (the branches, the tags, then the
// history's commits, newest first), in a diff tab: what the file has now
// that it had there. (What is typed and is none of them, a revision git
// knows, is compared with as it is.)
void App::git_compare_pick() {
    GitUi& g = *gitui_;
    if (!cfg_.git || !g.status.repo) return notify("Not in a git repository");
    Doc& d = *pane_->doc;
    if (d.ed.path.empty() || d.gitview || d.term || d.prof || d.loading) return;
    const std::string rel = in_repo(g.status.root, full_path(d.ed.path));
    if (rel.empty()) return notify("Not in the repository");
    std::vector<std::string> revs, rows;
    for (const GitBranch& b : g.branches) {
        if (b.current) continue;
        revs.push_back(b.name);
        rows.push_back(b.name + "  \xC2\xB7 " + (b.remote ? "remote branch" : "branch"));
    }
    for (const std::string& t : g.tags) {
        revs.push_back(t);
        rows.push_back(t + "  \xC2\xB7 tag");
    }
    for (const GitCommit& c : g.log) {
        revs.push_back(c.full.empty() ? c.hash : c.full);
        rows.push_back(c.hash + "  " + c.subject + "  \xC2\xB7 " + c.author + ", " + c.when);
    }
    revs.push_back("HEAD");
    rows.push_back("HEAD  \xC2\xB7 the last commit");
    const std::string name(name_of(rel));
    find_choose_item("Compare " + name + " with", std::move(rows), [this, revs, rel, name](int i) {
        if (i < 0 || static_cast<size_t>(i) >= revs.size()) return;
        git_compare(revs[static_cast<size_t>(i)], rel);
    });
}

void App::git_compare(const std::string& rev, const std::string& rel) {
    const std::string shown = rev.size() == 40 ? rev.substr(0, 10) : rev;
    git_open_view({"diff", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", rev, "--", rel}, std::string(name_of(rel)) + " (against " + shown + ")", kViewOther, rel);
}

// ---------------------------------------------------------------- a file's history

// The commits that changed the file in front (followed through renames),
// or the lines chosen in it (the cursor's, if none: as git log -L finds
// them in the commits before), each with its change, in a diff tab. Enter
// on a commit there shows all of it.
void App::git_history(bool lines) {
    GitUi& g = *gitui_;
    if (!cfg_.git || !g.status.repo) return notify("Not in a git repository");
    Doc& d = *pane_->doc;
    if (d.ed.path.empty() || d.gitview || d.term || d.prof || d.loading) return;
    const std::string rel = in_repo(g.status.root, full_path(d.ed.path));
    if (rel.empty()) return notify("Not in the repository");
    std::vector<std::string> args{"log", "--pretty=medium", "--no-ext-diff"};
    std::string title = std::string(name_of(rel));
    if (lines) {
        const u64 a = std::min(d.ed.cursor(), d.ed.anchor()), b = std::max(d.ed.cursor(), d.ed.anchor());
        const u64 first = d.ed.buf.line_of(a);
        u64 last = d.ed.buf.line_of(b);
        if (last > first && b == d.ed.buf.line_start(last)) --last;  // (a selection to the start of a line ends on the line before)
        const std::string span = std::to_string(first + 1) + "," + std::to_string(last + 1);
        args.push_back("-L" + span + ":" + rel);
        title += ":" + std::to_string(first + 1) + (last > first ? "-" + std::to_string(last + 1) : std::string()) + " (history)";
    } else {
        args.insert(args.end(), {"--follow", "--patch", "--", rel});
        title += " (history)";
    }
    git_open_view(std::move(args), title, kViewOther, {});
}

// The commit a line of a history tab (or any log with patches) is in: the
// last "commit <hash>" at or above it.
std::string App::git_commit_at(const Doc& d) {
    if (!d.gitview || !d.gitview->source || !d.gitview->diff) return {};
    const GitView& v = *d.gitview;
    const std::string& text = *v.source;
    const std::vector<u64>& off = v.diff->line_off;
    u32 line = ~0u;  // (the cursor's row, or the nearest above that shows a line of the diff)
    for (u64 r = d.ed.buf.line_of(d.ed.cursor()) + 1; r-- > 0 && line == ~0u;) line = v.diff_line(r);
    if (line == ~0u) return {};
    for (u32 l = line + 1; l-- > 0;) {
        if (l + 1 >= off.size()) continue;
        const std::string_view s(text.data() + off[l], off[l + 1] - off[l]);
        if (!s.starts_with("commit ")) continue;
        size_t n = 7;
        while (n < s.size() && std::isxdigit(static_cast<unsigned char>(s[n]))) ++n;
        if (n >= 7 + 7) return std::string(s.substr(7, n - 7));
    }
    return {};
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
    // What the action is about: its argument, or the row picked in the Git
    // tab (when that is a row of the kind the action is for).
    const GitUi::Row* row = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    const u8 kind = row ? row->kind : static_cast<u8>(kRowBlank);
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
    } else if (row && (row->kind == kRowBranch || row->kind == kRowCommit || row->kind == kRowStash || row->kind == kRowTag || row->kind == kRowTree ||
                       row->kind == kRowModule)) {
        name = g.name_of_row(*row);
    }
    auto run = [&](std::vector<std::string> args, const char* label, std::string input = {}) {
        g.busy = label;
        g.note.clear();
        git_.run(std::move(args), label, std::move(input));
    };
    // The stash an action is about: the one named, the one picked, or the latest.
    auto stash = [&] { return !arg.empty() ? arg : kind == kRowStash ? name : g.stashes.empty() ? std::string() : g.stashes[0].name; };
    auto of_kind = [&](u8 k) { return !arg.empty() || kind == k ? name : std::string(); };
    // Where tags go: the server the branch follows, or else origin.
    const std::string server = g.status.upstream.find('/') != std::string::npos ? g.status.upstream.substr(0, g.status.upstream.find('/')) : "origin";
    const std::string full = g.status.root + "/" + name;
    switch (action) {
        case kActCommit: {
            std::string text = arg.empty() ? g.message : arg;
            while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\t' || text.back() == '\r')) text.pop_back();
            if (text.empty()) {  // it needs saying what the commit is: the message field, in view
                panel_show_git();
                g.input = 1;
                g.pick(g.first(kRowMessage, 0));
                g.top = 0;
                g.note = "Type a message for the commit, then " + key_words("Cmd+Enter");
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
            if (entry->conflict) {  // its three versions, to merge
                git_merge_open(full);
            } else if (entry->index == '?') {  // nothing to compare with: the file itself
                open_path(full.c_str());
            } else {
                const bool cached = arg.empty() ? staged_side : !entry->unstaged();
                git_open_view(diff_args(name, cached), std::string(name_of(name)) + (cached ? " (staged)" : " (changes)"), cached ? kViewStaged : kViewChanges, name);
            }
            break;
        case kActSwitch:
            if (name.empty()) break;
            // A branch on a server gets a local one of the same name that follows it.
            if (row && arg.empty() && kind == kRowBranch && g.branches[row->index].remote) run({"switch", "-q", "--track", name}, "Switch");
            else run({"switch", "-q", name}, "Switch");
            break;
        case kActMerge:
            if (!name.empty()) run({"merge", "--no-edit", name}, "Merge");
            break;
        case kActDeleteBranch:
            if (!name.empty() && confirm("Delete the branch " + name + "?", "Delete")) run({"branch", "-d", name}, "Delete branch");
            break;
        case kActNewBranch:
        case kActNewTag: {
            const bool tag = action == kActNewTag;
            if (arg.empty()) {  // its name, typed in its row
                panel_show_git();
                g.input = tag ? 3 : 2;
                (tag ? g.tag_name : g.branch_name).clear();
                g.pick(g.first(kRowAction, action));
                g.top = 0;
            } else {
                if (tag && arg[0] != '-') run({"tag", arg}, "New tag");
                else run({"switch", "-q", "-c", arg}, "New branch");
                g.input = 0;
            }
            break;
        }
        case kActShowCommit:
            if (!name.empty()) git_open_view({"show", "--stat", "--patch", "--no-ext-diff", name}, name, kViewOther, {});
            break;
        case kActBlame:
            pane_->doc->blame_on = !pane_->doc->blame_on;
            pane_->cache_rev = ~0ull;
            break;
        case kActRefresh: git_.refresh(); break;
        // Stashes: what has changed put away (files git does not know stay),
        // and brought back. One that does not go back cleanly leaves its
        // conflicts, as a merge does, and is kept.
        case kActStash: run({"stash", "push", "-q"}, "Stash"); break;
        case kActPop:
        case kActApply: {
            const std::string which = stash();
            if (which.empty()) break;
            if (action == kActPop) run({"stash", "pop", "-q", which}, "Pop stash");
            else run({"stash", "apply", "-q", which}, "Apply stash");
            break;
        }
        case kActDrop: {
            const std::string which = stash();
            if (!which.empty() && confirm("Drop " + which + "? Its changes are gone with it.", "Drop")) run({"stash", "drop", "-q", which}, "Drop stash");
            break;
        }
        case kActShowStash: {
            const std::string which = stash();
            if (!which.empty()) git_open_view({"stash", "show", "-p", "--no-ext-diff", which}, which, kViewOther, {});
            break;
        }
        // The branch checked out rebased onto another (it asks: the commits
        // are made anew), a commit cherry-picked from another branch's
        // history. Either stops at conflicts for them to be settled, staged,
        // and then continued (or given up).
        case kActRebase: {
            const std::string onto = of_kind(kRowBranch);
            if (onto.empty() || onto == g.status.branch) break;
            const std::string what = g.status.branch.empty() ? std::string("what is checked out") : g.status.branch;
            if (confirm("Rebase " + what + " onto " + onto + "? Its commits are made again on top of " + onto + ".", "Rebase")) run({"rebase", onto}, "Rebase");
            break;
        }
        case kActContinue:
            if (g.status.rebasing) run({"rebase", "--continue"}, "Continue rebase");
            else if (g.status.picking) run({"cherry-pick", "--continue"}, "Continue cherry-pick");
            break;
        case kActAbortRebase: run({"rebase", "--abort"}, "Abort rebase"); break;
        case kActPick: {
            const std::string commit = of_kind(kRowCommit);
            if (!commit.empty()) run({"cherry-pick", commit}, "Cherry-pick");
            break;
        }
        case kActAbortPick: run({"cherry-pick", "--abort"}, "Abort cherry-pick"); break;
        case kActShowTag: {
            const std::string tag = of_kind(kRowTag);
            if (!tag.empty()) git_open_view({"show", "--stat", "--patch", "--no-ext-diff", tag}, tag, kViewOther, {});
            break;
        }
        case kActPushTag: {
            const std::string tag = of_kind(kRowTag);
            if (!tag.empty()) run({"push", server, "refs/tags/" + tag}, "Push tag");
            break;
        }
        case kActDeleteTag: {
            const std::string tag = of_kind(kRowTag);
            if (!tag.empty() && confirm("Delete the tag " + tag + " here? (One pushed stays on the server.)", "Delete")) run({"tag", "-d", tag}, "Delete tag");
            break;
        }
        case kActHistory: {
            // The picked branch's commits in History; that branch again (or
            // the one checked out), the checked out one's.
            std::string branch = of_kind(kRowBranch);
            if (branch == g.log_of || (row && arg.empty() && kind == kRowBranch && g.branches[row->index].current)) branch.clear();
            g.log_of = branch;
            git_.log_of(branch);
            break;
        }
        case kActStageHere: git_stage_here(true); break;
        case kActUnstageHere: git_stage_here(false); break;
        case kActOpenTree: {  // a working tree (not this one), or a submodule (one checked out), as the folder worked in
            std::string dir;
            if (kind == kRowTree && row->index > 0) dir = name;
            else if (kind == kRowModule) {
                const GitSubmodule& m = g.submodules[row->index];
                if (m.state == '-') {
                    g.note = m.path + " is not checked out: u updates the submodules";
                    g.note_bad = true;
                    break;
                }
                dir = g.status.root + "/" + m.path;
            } else if (!arg.empty()) {
                dir = arg.starts_with('/') ? arg : g.status.root + "/" + arg;
            }
            if (!dir.empty()) panel_set_workdir(dir);
            break;
        }
        case kActNewTree: {  // beside the repository, named after it and the branch; the branch made if there is none
            if (arg.empty()) break;
            std::string leaf = std::string(name_of(g.status.root)) + "-" + arg;
            std::ranges::replace(leaf, '/', '-');
            const std::string where = parent_of(g.status.root) + "/" + leaf;
            const bool have = std::ranges::any_of(g.branches, [&](const GitBranch& b) { return !b.remote && b.name == arg; });
            if (have) run({"worktree", "add", where, arg}, "New worktree");
            else run({"worktree", "add", "-b", arg, where}, "New worktree");
            break;
        }
        case kActRemoveTree: {
            if (kind != kRowTree || row->index == 0) break;
            if (confirm("Remove the worktree at " + name + "? (Its branch stays.)", "Remove")) run({"worktree", "remove", name}, "Remove worktree");
            break;
        }
        case kActSubmodules: run({"submodule", "update", "--init", "--recursive"}, "Update submodules"); break;
        default: break;
    }
    ++git_gen_;
    dirty_ = true;
}

// ---------------------------------------------------------------- in the text view

// Who last changed the cursor's line, after the end of that line. (And in
// a diff tab, the key for the hunk the cursor is in; and the box a change's
// mark offers.)
void App::git_note(FrameDesc& fd) {
    GitUi& g = *gitui_;
    git_offer_layout(fd);
    git_view_note(fd);
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

// The message field's rows, broken again (the message or the room for it
// has changed), and the caret's kept in view.
void App::git_message_changed() {
    GitUi& g = *gitui_;
    g.caret = std::min(g.caret, g.message.size());
    wrap_message(g.message, g.wrap ? g.wrap : 40, &g.segs);
    g.field_rows = std::min<u32>(static_cast<u32>(g.segs.size()), std::max<u32>(1, g.field_most));
    const u32 at = caret_row(g.segs, g.caret);
    if (at < g.field_top) g.field_top = at;
    if (at >= g.field_top + g.field_rows) g.field_top = at - g.field_rows + 1;
    g.field_top = std::min<u32>(g.field_top, static_cast<u32>(g.segs.size()) - g.field_rows);
    git_rows();
}

// The rows of the Git tab, from what git last said. Made again whenever
// that changes, so that a row never points at something that has gone.
void App::git_rows() {
    GitUi& g = *gitui_;
    g.rows_gen = git_gen_;
    g.rows.clear();
    if (!g.status.repo) {
        if (g.known && cfg_.git) g.rows.push_back({kRowAction, kActInit});  // (the one thing to do: make it one)
    } else {
        // The message (a row for each of its rows on show), then a row for
        // each thing to do (reached with Up and Down like the files), then
        // how the last of them went.
        for (u32 k = 0; k < std::max<u32>(1, g.field_rows); ++k) g.rows.push_back({kRowMessage, k});
        for (const u32 a : {kActCommit, kActPull, kActPush, kActFetch, kActStageAll, kActStash, kActNewBranch, kActNewTag, kActBlame})
            g.rows.push_back({kRowAction, a});
        if (g.status.merging) g.rows.push_back({kRowAction, kActAbortMerge});
        if (g.status.rebasing || g.status.picking) g.rows.push_back({kRowAction, kActContinue});
        if (g.status.rebasing) g.rows.push_back({kRowAction, kActAbortRebase});
        if (g.status.picking) g.rows.push_back({kRowAction, kActAbortPick});
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
        // A list of its own: its heading, and unless it is folded, its rows.
        auto list = [&](u32 sec, u8 kind, size_t n, bool always) {
            if (!n && !always) return;
            g.rows.push_back({kRowHeader, sec});
            if (!g.fold[sec])
                for (u32 i = 0; i < n; ++i) g.rows.push_back({kind, i});
        };
        section(kSecConflicts, [](const GitEntry& e) { return e.conflict; }, false);
        section(kSecStaged, [](const GitEntry& e) { return e.staged(); }, true);
        section(kSecChanges, [](const GitEntry& e) { return e.unstaged(); }, false);
        list(kSecStashes, kRowStash, g.stashes.size(), false);
        list(kSecBranches, kRowBranch, g.branches.size(), true);
        list(kSecTags, kRowTag, g.tags.size(), false);
        list(kSecTrees, kRowTree, g.worktrees.size() > 1 ? g.worktrees.size() : 0, false);  // (only when there is another than this one)
        list(kSecModules, kRowModule, g.submodules.size(), false);
        list(kSecHistory, kRowCommit, g.log.size(), false);
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
    // The message is broken into rows as wide as the field (its key shows
    // at the right of the first while there is room for it there), and the
    // field grows to a third of the tab before it scrolls.
    const Hint commit_key{"Cmd+Enter", "commit"};
    const u32 commit_w = hints_width({&commit_key, 1}, font);
    const u32 room = cols >= commit_w + 11 ? cols - commit_w - 3 : cols;
    const u32 wrap = cols > 1 ? cols - 1 : 1;
    const u32 most = std::clamp<u32>(g.body_rows / 3, 1, kFieldMost);
    if (wrap != g.wrap || most != g.field_most) {
        g.wrap = wrap;
        g.field_most = most;
        git_message_changed();
    } else if (g.rows_gen != git_gen_) {
        git_rows();
    }
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
        // The branch, and how far it is from the one it follows; and what is
        // under way, if something stopped for conflicts.
        std::string counts;
        if (g.status.ahead) counts = "\xE2\x86\x91" + std::to_string(g.status.ahead);
        if (g.status.behind) counts += (counts.empty() ? "\xE2\x86\x93" : " \xE2\x86\x93") + std::to_string(g.status.behind);
        const char* state = g.status.rebasing ? "rebasing" : g.status.picking ? "cherry-picking" : g.status.merging ? "merging" : nullptr;
        const u32 tail = (counts.empty() ? 0 : width_of(counts) + 1) + (state ? width_of(state) + 1 : 0);
        u32 c = put(box, font, 1, 0, "\xE2\x8E\x87 ", ink::kMuted);  // a branch sign
        const std::string branch = g.status.branch.empty() ? "(no branch) " + g.status.head : g.status.branch;
        c = put(box, font, 1, c, fit_text(branch, cols > c + tail ? cols - c - tail : 1), ink::kText);
        if (!counts.empty()) c = put(box, font, 1, c + 1, counts, ink::kMuted);
        if (state) put(box, font, 1, c + 1, state, ink::kWarning);
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
    // A field's tone, along a whole row.
    auto tone = [&](u32 r) {
        u32* line = box.cells.data() + static_cast<size_t>(r) * cols;
        for (u32 c = 0; c < cols; ++c) line[c] |= kCellSelected | kFieldTone << kCellDecoShift;
        return line;
    };
    // A field of one line: the text typed (its end, where the next letter
    // goes, while it is typed), or what it is for; on the right the key
    // that does it; all on a tone of its own.
    auto field = [&](u32 r, const std::string& text, const char* empty, bool typing, Hint key) {
        const u32 key_w = hints_width({&key, 1}, font);
        const u32 fits = cols >= key_w + 11 ? cols - key_w - 3 : cols;
        u32 end = 0;
        if (text.empty()) put(box, font, r, 0, fit_text(empty, fits), ink::kMuted);
        else end = put(box, font, r, 0, typing ? fit_text(text, fits > 1 ? fits - 1 : 0, true) : fit_text(text, fits), ink::kText);
        if (fits < cols) put_hints(box, font, r, cols - key_w, {&key, 1});
        u32* line = tone(r);
        if (typing && end < cols) line[end] = (line[end] & ~kCellDecoMask) | kCellSelected;  // where the next letter goes
    };
    // A row of the commit message. Past the 50th column of its first line,
    // and the 72nd of the others, the text is muted: git's guides for a
    // summary and a body, shown without a word about them.
    auto message_row = [&](u32 r, u32 k) {
        const bool typing = focused && g.input == 1;
        const u32 at = g.field_top + k;
        u32 caret_col = ~0u;
        if (g.message.empty()) {
            if (k == 0) put(box, font, r, 0, fit_text("Message", room), ink::kMuted);
            caret_col = 0;
        } else if (at < g.segs.size()) {
            const MsgRow& m = g.segs[at];
            const std::string_view text(g.message.data() + m.b0, m.b1 - m.b0);
            const u32 limit = m.line == 0 ? 50 : 72;
            size_t split = text.size();
            if (m.col >= limit) {
                split = 0;
            } else {
                u32 w = m.col;
                const u8* p = reinterpret_cast<const u8*>(text.data());
                for (size_t i = 0; i < text.size();) {
                    const Decoded d = decode_utf8(p + i, text.size() - i);
                    if (w + cp_width(d.cp) > limit) {
                        split = i;
                        break;
                    }
                    w += cp_width(d.cp);
                    i += d.len;
                }
            }
            const u32 c = put(box, font, r, 0, text.substr(0, split), ink::kText);
            put(box, font, r, c, text.substr(split), ink::kMuted);
            if (caret_row(g.segs, g.caret) == at) caret_col = width_of(std::string_view(g.message).substr(m.b0, g.caret - m.b0));
        }
        if (k == 0 && room < cols && g.field_top == 0 && (g.segs.empty() || width_of(std::string_view(g.message).substr(g.segs[0].b0, g.segs[0].b1 - g.segs[0].b0)) < room))
            put_hints(box, font, r, cols - commit_w, {&commit_key, 1});
        u32* line = tone(r);
        if (typing && caret_col < cols) line[caret_col] = (line[caret_col] & ~kCellDecoMask) | kCellSelected;  // where the next letter goes
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
            case kActStash: label = "Stash"; break;
            case kActNewBranch: label = "New branch\xE2\x80\xA6"; break;
            case kActNewTag: label = "New tag\xE2\x80\xA6"; break;
            case kActBlame: label = "Blame"; key = "Cmd+Option+B"; break;
            case kActAbortMerge: label = "Abort merge"; break;
            case kActContinue: label = g.status.rebasing ? "Continue rebase" : "Continue cherry-pick"; break;
            case kActAbortRebase: label = "Abort rebase"; break;
            case kActAbortPick: label = "Abort cherry-pick"; break;
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
                message_row(r, row.index);
                lit = false;  // (the field is lit by its tone, and its caret while typed into)
                break;
            case kRowAction:
                if (row.index == kActNewBranch && g.input == 2) {
                    field(r, g.branch_name, "Branch name", focused, {"Enter", "make it"});
                    lit = false;
                } else if (row.index == kActNewTag && g.input == 3) {
                    field(r, g.tag_name, "Tag name", focused, {"Enter", "make it"});
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
                else if (row.index == kSecStashes) n = static_cast<u32>(g.stashes.size());
                else if (row.index == kSecTags) n = static_cast<u32>(g.tags.size());
                else if (row.index == kSecTrees) n = static_cast<u32>(g.worktrees.size());
                else if (row.index == kSecModules) n = static_cast<u32>(g.submodules.size());
                else
                    for (const GitEntry& e : g.status.entries) n += row.index == kSecConflicts ? e.conflict : row.index == kSecStaged ? e.staged() : e.unstaged();
                put(box, font, r, 0, g.fold[row.index] ? "\xE2\x96\xB8" : "\xE2\x96\xBE", ink::kMuted);  // a small triangle, right or down
                u32 c = put(box, font, r, 2, kSections[row.index], ink::kHeading);
                if (row.index == kSecHistory && !g.log_of.empty())
                    c = put(box, font, r, c + 1, fit_text(g.log_of == "--all" ? "all branches" : g.log_of, cols > c + 6 ? cols - c - 6 : 1), ink::kText);
                if (row.index == kSecHistory && !g.log_find.empty())
                    c = put(box, font, r, c + 1, fit_text("\xE2\x80\x9C" + g.log_find + "\xE2\x80\x9D", cols > c + 6 ? cols - c - 6 : 1), ink::kAccent);
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
                const u32 fits = cols > 4 + (sign ? 2 : 0) ? cols - 4 - (sign ? 2 : 0) : 0;
                const std::string_view name = name_of(e.path);
                const u32 c = put(box, font, r, 4, fit_text(name, fits), ink::kText);
                if (name.size() < e.path.size() && c + 2 < 4 + fits)
                    put(box, font, r, c + 1, fit_text(std::string_view(e.path).substr(0, e.path.size() - name.size()), 4 + fits - c - 1, true), ink::kMuted);
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
            case kRowCommit: {  // its lanes, its hash, what points at it, its subject
                const GitCommit& c = g.log[row.index];
                u32 at = 2;
                if (row.index < g.graph.size()) {
                    const GitGraphRow& gr = g.graph[row.index];
                    for (size_t k = 0; k < gr.cells.size() && at + 8 < cols; ++k, ++at) {
                        if (gr.cells[k] == U' ') continue;
                        put(box, font, r, at, utf8_of(gr.cells[k]), guide_inks_ ? Ink{kInkGuide + gr.lanes[k] % guide_inks_, 0} : ink::kMuted);
                    }
                    ++at;
                }
                at = put(box, font, r, at, c.hash, ink::kMuted) + 1;
                for (const GitRef& ref : refs_of(c.refs)) {
                    if (at + 4 >= cols) break;
                    at = put(box, font, r, at, fit_text(ref.name, cols - at - 2), ref.tag ? ink::kWarning : ref.remote ? ink::kMuted : ref.head ? ink::kAccentStrong : ink::kAccent) + 1;
                }
                put(box, font, r, at, fit_text(c.subject, cols > at ? cols - at : 0), ink::kText);
                break;
            }
            case kRowStash: {  // its number, and what git called it
                const GitStash& st = g.stashes[row.index];
                const std::string_view n = std::string_view(st.name).substr(std::min<size_t>(st.name.size(), 6));  // ("stash@{0}": "{0}")
                const u32 at = put(box, font, r, 2, n, ink::kMuted);
                put(box, font, r, at + 1, fit_text(st.subject, cols > at + 1 ? cols - at - 1 : 0), ink::kText);
                break;
            }
            case kRowTag: put(box, font, r, 2, fit_text(g.tags[row.index], cols > 2 ? cols - 2 : 0), ink::kText); break;
            case kRowTree: {  // its branch (this one's marked), and where it is
                const GitWorktree& w = g.worktrees[row.index];
                const bool here = row.index == 0;
                u32 at = put(box, font, r, 2, here ? "\xE2\x97\x8F" : " ", ink::kAccent) + 1;  // ●
                at = put(box, font, r, at, fit_text(w.branch.empty() ? w.head.substr(0, 7) : w.branch, cols > at ? cols - at : 0), here ? ink::kAccentStrong : ink::kText) + 1;
                std::string where = w.path;
                if (const char* home = SDL_getenv("HOME"); home && *home && where.starts_with(home)) where = "~" + where.substr(std::strlen(home));
                put(box, font, r, at, fit_text(where + (w.locked ? "  (locked)" : w.prunable ? "  (gone)" : ""), cols > at ? cols - at : 0), ink::kMuted);
                break;
            }
            case kRowModule: {  // how it stands, its path, and the commit
                const GitSubmodule& m = g.submodules[row.index];
                const char* mark = m.state == '-' ? "\xE2\x97\x8B" : m.state == '+' ? "+" : m.state == 'U' ? "U" : "\xE2\x97\x8F";  // ○ not checked out, ● as wanted
                u32 at = put(box, font, r, 2, mark, m.state == ' ' ? ink::kAdded : m.state == '-' ? ink::kMuted : ink::kWarning) + 1;
                at = put(box, font, r, at, fit_text(m.path, cols > at ? cols - at : 0), ink::kText) + 1;
                put(box, font, r, at, fit_text(m.describe.empty() ? m.commit.substr(0, 7) : m.describe, cols > at ? cols - at : 0), ink::kMuted);
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
    Key keys[6];
    u32 n = 0;
    auto add = [&](const char* k, const char* what, u32 a = kActNone) { keys[n++] = {k, what, a}; };
    const GitUi::Row* picked = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    if (!g.status.repo) {
        add("Enter", "make it one", kActInit);
        add("Cmd+R", "look again", kActLook);
    } else if (g.input == 2 || g.input == 3) {
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
    } else if (picked && picked->kind == kRowBranch) {
        const GitBranch& b = g.branches[picked->index];
        if (!b.current) {
            add("Enter", "switch", kActSwitch);
            add("m", "merge", kActMerge);
            add("r", "rebase onto", kActRebase);
        }
        add("h", b.name == g.log_of || (b.current && !g.log_of.empty()) ? "back" : "history", kActHistory);
        if (!b.current && !b.remote) add("Backspace", "delete", kActDeleteBranch);
    } else if (picked && picked->kind == kRowCommit) {
        add("Enter", "show", kActShowCommit);
        if (!g.log_of.empty() && g.log_of != g.status.branch) add("c", "cherry-pick", kActPick);
        if (g.log_of.empty() && g.log_find.empty()) add("i", "rebase from here");
        add("Tab", "message");
    } else if (picked && picked->kind == kRowStash) {
        add("Enter", "show", kActShowStash);
        add("p", "pop", kActPop);
        add("a", "apply", kActApply);
        add("Backspace", "drop", kActDrop);
    } else if (picked && picked->kind == kRowTag) {
        add("Enter", "show", kActShowTag);
        add("p", "push", kActPushTag);
        add("Backspace", "delete", kActDeleteTag);
    } else if (picked && picked->kind == kRowTree) {
        if (picked->index > 0) {
            add("Enter", "open", kActOpenTree);
            add("Backspace", "remove", kActRemoveTree);
        }
    } else if (picked && picked->kind == kRowModule) {
        add("Enter", "open", kActOpenTree);
        add("u", "update all", kActSubmodules);
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
    if (g.input == 1) {  // the message: where the caret is
        g.caret = std::min(g.caret, g.message.size());
        if (g.message.size() + text.size() <= kMessageMost) {
            g.message.insert(g.caret, text);
            g.caret += text.size();
        }
        git_message_changed();
    } else {
        std::string& into = g.input == 2 ? g.branch_name : g.tag_name;
        if (into.size() < 500) into += text;
    }
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
    if (g.input == 1 && g.status.repo) {
        // The message: Enter makes a new line, Cmd+Enter commits; the caret
        // goes by characters and by rows as the field shows them.
        std::string& m = g.message;
        size_t& c = g.caret;
        c = std::min(c, m.size());
        auto before = [&](size_t i) {
            if (i == 0) return i;
            --i;
            while (i > 0 && is_cont(static_cast<u8>(m[i]))) --i;
            return i;
        };
        auto after = [&](size_t i) {
            if (i >= m.size()) return i;
            ++i;
            while (i < m.size() && is_cont(static_cast<u8>(m[i]))) ++i;
            return i;
        };
        // To row r of the field, as near the column the caret is at as it has.
        auto to_row = [&](u32 r) {
            const MsgRow& from = g.segs[caret_row(g.segs, c)];
            const u32 col = width_of(std::string_view(m).substr(from.b0, c - from.b0));
            const MsgRow& to = g.segs[r];
            const bool broken = r + 1 < g.segs.size() && g.segs[r + 1].line == to.line && to.b1 > to.b0;  // (its end is the next row's start)
            const size_t end = broken ? before(to.b1) : to.b1;
            size_t i = to.b0;
            for (u32 w = 0; i < end;) {
                const size_t j = after(i);
                w += width_of(std::string_view(m).substr(i, j - i));
                if (w > col) break;
                i = j;
            }
            c = i;
        };
        auto edited = [&] {
            git_message_changed();
            return changed();
        };
        switch (k.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (cmd) {
                    git_do(kActCommit);
                    return changed();
                }
                if (m.size() < kMessageMost) m.insert(c++, 1, '\n');
                return edited();
            case SDLK_ESCAPE:
            case SDLK_TAB: to_list(); return changed();
            case SDLK_UP: {
                const u32 r = caret_row(g.segs, c);
                if (r > 0) to_row(r - 1);
                return edited();
            }
            case SDLK_DOWN: {
                const u32 r = caret_row(g.segs, c);
                if (r + 1 >= g.segs.size()) to_list();
                else to_row(r + 1);
                return edited();
            }
            case SDLK_LEFT: c = before(c); return edited();
            case SDLK_RIGHT: c = after(c); return edited();
            case SDLK_HOME: {
                const size_t nl = c ? m.rfind('\n', c - 1) : std::string::npos;
                c = nl == std::string::npos ? 0 : nl + 1;
                return edited();
            }
            case SDLK_END: c = std::min(m.size(), m.find('\n', c)); return edited();
            case SDLK_BACKSPACE: {
                const size_t b = before(c);
                m.erase(b, c - b);
                c = b;
                return edited();
            }
            case SDLK_DELETE: m.erase(c, after(c) - c); return edited();
            default: return !cmd;
        }
    }
    if ((g.input == 2 || g.input == 3) && g.status.repo) {  // a new branch's name, or a tag's
        std::string& text = g.input == 2 ? g.branch_name : g.tag_name;
        switch (k.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (!text.empty()) git_do(g.input == 2 ? kActNewBranch : kActNewTag, text);
                g.input = 0;
                return changed();
            case SDLK_ESCAPE:
            case SDLK_TAB: g.input = 0; return changed();  // (the name given up; its row stays picked)
            case SDLK_UP:
            case SDLK_DOWN: return true;
            case SDLK_BACKSPACE:
                while (!text.empty() && is_cont(static_cast<u8>(text.back()))) text.pop_back();
                if (!text.empty()) text.pop_back();
                return changed();
            default: return !cmd;
        }
    }
    const GitUi::Row* row = g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() ? &g.rows[static_cast<size_t>(g.sel)] : nullptr;
    const u8 kind = row ? row->kind : static_cast<u8>(kRowBlank);
    const bool plain = !(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT));
    const bool other = kind == kRowBranch && !g.branches[row->index].current;  // a branch not checked out
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
            else if (kind == kRowAction) git_do(row->index);
            else if (kind == kRowEntry) git_do(kActOpenDiff);
            else if (kind == kRowBranch) git_do(kActSwitch);
            else if (kind == kRowCommit) git_do(kActShowCommit);
            else if (kind == kRowStash) git_do(kActShowStash);
            else if (kind == kRowTag) git_do(kActShowTag);
            else if (kind == kRowTree || kind == kRowModule) git_do(kActOpenTree);
            return changed();
        case SDLK_RIGHT:  // towards "staged"
            if (kind == kRowEntry && !(row->index & kStagedBit)) git_do(kActStage);
            return changed();
        case SDLK_LEFT:
            if (kind == kRowEntry && (row->index & kStagedBit)) git_do(kActUnstage);
            return changed();
        case SDLK_DELETE:
        case SDLK_BACKSPACE:
            if (kind == kRowEntry && !(row->index & kStagedBit)) git_do(kActDiscard);
            else if (other && !g.branches[row->index].remote) git_do(kActDeleteBranch);
            else if (kind == kRowStash) git_do(kActDrop);
            else if (kind == kRowTag) git_do(kActDeleteTag);
            else if (kind == kRowTree && row->index > 0) git_do(kActRemoveTree);
            return changed();
        default: break;
    }
    if (!plain) return false;
    // Letters for what is picked: a file opened; a branch merged, rebased
    // onto, or its history; a commit cherry-picked; a stash popped or
    // applied; a tag pushed.
    switch (k.key) {
        case SDLK_O:
            if (kind == kRowEntry) git_do(kActOpenFile);
            return changed();
        case SDLK_U:
            if (kind == kRowModule) git_do(kActSubmodules);
            return changed();
        case SDLK_M:
            if (other) git_do(kActMerge);
            return changed();
        case SDLK_R:
            if (other) git_do(kActRebase);
            return changed();
        case SDLK_H:
            if (kind == kRowBranch) git_do(kActHistory);
            return changed();
        case SDLK_C:
            if (kind == kRowCommit && !g.log_of.empty() && g.log_of != g.status.branch) git_do(kActPick);
            return changed();
        case SDLK_I:  // an interactive rebase of the commits from this one on
            if (kind == kRowCommit && g.log_of.empty() && g.log_find.empty() && g.sel >= 0) {
                const std::string hash = g.name_of_row(g.rows[static_cast<size_t>(g.sel)]);
                if (!hash.empty()) git_rebase_open(hash + "~1");
            }
            return changed();
        case SDLK_P:
            if (kind == kRowStash) git_do(kActPop);
            else if (kind == kRowTag) git_do(kActPushTag);
            return changed();
        case SDLK_A:
            if (kind == kRowStash) git_do(kActApply);
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
            if (on_row || g.input >= 2) g.input = 0;  // (a row's sign: the list has the keyboard; a key below leaves the message its own)
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
        case kRowMessage: {  // the caret to where it was clicked
            g.input = 1;
            g.pick(static_cast<int>(i));
            const u32 at = g.field_top + r.index;
            if (at < g.segs.size() && col >= 0) {
                const MsgRow& m = g.segs[at];
                size_t c = m.b0;
                u32 w = 0;
                while (c < m.b1) {
                    size_t j = c + 1;
                    while (j < m.b1 && is_cont(static_cast<u8>(g.message[j]))) ++j;
                    w += width_of(std::string_view(g.message).substr(c, j - c));
                    if (w > static_cast<u32>(col)) break;
                    c = j;
                }
                g.caret = c;
                git_message_changed();
            }
            break;
        }
        case kRowAction:  // a row that does something is a button
            if ((g.input == 2 && r.index == kActNewBranch) || (g.input == 3 && r.index == kActNewTag)) break;  // (its name is being typed there)
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
        case kRowStash:
        case kRowTag:
            g.input = 0;
            g.pick(static_cast<int>(i));
            if (b.clicks >= 2) git_do(r.kind == kRowCommit ? kActShowCommit : r.kind == kRowStash ? kActShowStash : kActShowTag);
            break;
        case kRowTree:
        case kRowModule:
            g.input = 0;
            g.pick(static_cast<int>(i));
            if (b.clicks >= 2) git_do(kActOpenTree);
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
    } known[] = {{"commit", kActCommit},       {"pull", kActPull},           {"push", kActPush},           {"fetch", kActFetch},
                 {"stage", kActStage},         {"unstage", kActUnstage},     {"discard", kActDiscard},     {"diff", kActOpenDiff},
                 {"open", kActOpenFile},       {"switch", kActSwitch},       {"merge", kActMerge},         {"delete", kActDeleteBranch},
                 {"branch", kActNewBranch},    {"stageall", kActStageAll},   {"abort", kActAbortMerge},    {"show", kActShowCommit},
                 {"blame", kActBlame},         {"refresh", kActRefresh},     {"cancel", kActCancel},       {"init", kActInit},
                 {"look", kActLook},           {"stash", kActStash},         {"pop", kActPop},             {"apply", kActApply},
                 {"drop", kActDrop},           {"showstash", kActShowStash}, {"rebase", kActRebase},       {"continue", kActContinue},
                 {"abortrebase", kActAbortRebase}, {"pick", kActPick},       {"abortpick", kActAbortPick}, {"tag", kActNewTag},
                 {"showtag", kActShowTag},     {"pushtag", kActPushTag},     {"deletetag", kActDeleteTag}, {"history", kActHistory},
                 {"stagehere", kActStageHere}, {"unstagehere", kActUnstageHere}, {"worktree", kActNewTree}, {"removeworktree", kActRemoveTree},
                 {"submodules", kActSubmodules}, {"opentree", kActOpenTree}};
    if (name == "columns") return git_view_toggle();
    if (name == "find") {  // History searched (nothing: all of it again)
        gitui_->log_find = arg;
        git_.log_find(arg);
        ++git_gen_;
        return;
    }
    if (name == "allbranches") {  // History of every branch, or (again) the one checked out
        GitUi& g = *gitui_;
        g.log_of = g.log_of == "--all" ? std::string() : std::string("--all");
        git_.log_of(g.log_of);
        ++git_gen_;
        return;
    }
    if (name == "staged") {  // (what is staged of a file, in a diff tab)
        if (gitui_->status.repo) git_open_view(diff_args(arg, true), std::string(name_of(arg)) + " (staged)", kViewStaged, arg);
        return;
    }
    for (const auto& k : known)
        if (name == k.name) return git_do(k.action, arg);
    std::fprintf(stderr, "blit: no git action called %.*s\n", static_cast<int>(name.size()), name.data());
}

// History as the Git tab draws it: each commit's lanes, what points at it
// and its subject, a line each (for the tests).
std::string App::git_searched() const { return gitui_->log_find; }
std::string App::git_upstream() const { return gitui_->status.upstream; }

std::string App::git_graph_dump() {
    const GitUi& g = *gitui_;
    std::string out;
    for (size_t i = 0; i < g.log.size(); ++i) {
        std::string lanes;
        if (i < g.graph.size())
            for (const char32_t ch : g.graph[i].cells) lanes += utf8_of(ch);
        out += (i ? "\n" : "") + lanes + (lanes.empty() ? "" : "  ") + g.log[i].subject;
        for (const GitRef& ref : refs_of(g.log[i].refs)) out += std::string(" [") + (ref.tag ? "tag " : "") + ref.name + "]";
    }
    return out;
}

// What git has told the editor, on one line (for the tests).
std::string App::git_dump() {
    const GitUi& g = *gitui_;
    if (!g.status.repo) return "no repository";
    std::string out = g.status.branch.empty() ? "(no branch)" : g.status.branch;
    if (g.status.ahead) out += " +" + std::to_string(g.status.ahead);
    if (g.status.behind) out += " -" + std::to_string(g.status.behind);
    if (g.status.merging) out += " merging";
    if (g.status.rebasing) out += " rebasing";
    if (g.status.picking) out += " cherry-picking";
    auto list = [&](const char* label, auto&& wanted, auto&& letter) {
        std::string s;
        for (const GitEntry& e : g.status.entries)
            if (wanted(e)) s += std::string(" ") + letter(e) + ":" + e.path;
        if (!s.empty()) out += std::string(" | ") + label + s;
    };
    list("conflicts", [](const GitEntry& e) { return e.conflict; }, [](const GitEntry&) { return 'U'; });
    list("staged", [](const GitEntry& e) { return e.staged(); }, [](const GitEntry& e) { return e.index; });
    list("changes", [](const GitEntry& e) { return e.unstaged(); }, [](const GitEntry& e) { return e.index == '?' ? '?' : e.work; });
    if (!g.stashes.empty()) {
        out += " | stashes";
        for (const GitStash& st : g.stashes) out += " \"" + st.subject + "\"";
    }
    out += " | branches";
    for (const GitBranch& b : g.branches) out += std::string(" ") + (b.current ? "*" : "") + b.name;
    if (!g.tags.empty()) {
        out += " | tags";
        for (const std::string& t : g.tags) out += " " + t;
    }
    if (g.worktrees.size() > 1) {
        out += " | worktrees";
        for (const GitWorktree& w : g.worktrees) out += " " + (w.branch.empty() ? std::string("(detached)") : w.branch) + "@" + std::string(name_of(w.path));
    }
    if (!g.submodules.empty()) {
        out += " | submodules";
        for (const GitSubmodule& m : g.submodules) out += std::string(" ") + (m.state == ' ' ? "" : std::string(1, m.state)) + m.path;
    }
    out += " | commits " + std::to_string(g.log.size());
    if (!g.log_of.empty()) out += " of " + g.log_of;
    if (!g.log_find.empty()) out += " found \"" + g.log_find + "\"";
    if (!g.log.empty()) out += " \"" + g.log[0].subject + "\"";
    const Doc& d = *pane_->doc;
    if (!d.ed.path.empty()) {
        out += " | lines";
        for (const GitHunk& h : d.hunks)
            out += " " + std::to_string(h.line + 1) + (h.kind == GitHunk::kAdded ? "+" : h.kind == GitHunk::kModified ? "~" : "-") + (h.count > 1 ? std::to_string(h.count) : "") +
                   (h.staged ? "s" : "");
        if (blame_shown(d) && d.blame && pane_->cur_line < d.blame->lines.size()) {  // (what shows: the note and the column)
            const GitBlame::Commit& c = d.blame->commits[d.blame->lines[pane_->cur_line]];
            out += " | blame " + (c.time ? c.author + ": " + c.summary : c.author);
        }
    }
    if (!g.note.empty()) out += " | note " + g.note;
    if (g.offer.open) {
        out += " | offer";
        for (u32 i = 0; i < g.offer.count; ++i)
            out += std::string(i ? ", " : " ") + (g.offer.items[i] == kActStageHere ? "stage" : g.offer.items[i] == kActUnstageHere ? "unstage" : "diff");
    }
    return out;
}

// The diff tab in front (for the tests): what it is, the hunk the cursor is
// in, then its lines, with what is coloured as added {+so+} and as taken
// away [-so-] (in two columns, within a changed line only what changed).
std::string App::git_view_dump() {
    const Doc& d = *pane_->doc;
    if (!d.gitview) return "no diff tab";
    const GitView& v = *d.gitview;
    const char* kinds[] = {"changes", "staged", "other"};
    std::string out = std::string(kinds[v.kind]) + " " + (v.path.empty() ? d.title : v.path) + (v.two ? ", two columns" : ", one column");
    const int hunk = v.hunk_at_row(d.ed.buf.line_of(d.ed.cursor()));
    out += ": " + std::to_string(v.diff ? v.diff->hunks.size() : 0) + " hunks, cursor " + (hunk < 0 ? std::string("in none") : "in " + std::to_string(hunk + 1));
    const std::string text = d.ed.buf.text(0, d.ed.buf.size());
    size_t m = 0;
    u8 now = 0, open = 0;  // the style here, and the colour open in the line
    std::string line;
    auto set = [&](u8 st) {
        if (st == open) return;
        if (open) {
            while (line.ends_with(' ')) line.pop_back();
            line += open == 1 ? "+}" : "-]";
        }
        if (st) line += st == 1 ? "{+" : "[-";
        open = st;
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        while (d.marks && m < d.marks->size() && d.marks->at(m) <= i) now = d.marks->style[m++];
        if (i == text.size() || text[i] == '\n') {
            set(0);
            while (line.ends_with(' ')) line.pop_back();
            out += "\n" + line;
            line.clear();
            continue;
        }
        set(now == 1 || now == 2 ? now : 0);
        line += text[i];
    }
    while (out.ends_with("\n")) out.pop_back();
    return out;
}

// The commit message as the field shows it (for the tests): its rows on
// show, a "|" where the guide mutes what follows, "^" at the caret.
std::string App::git_field_dump() {
    const GitUi& g = *gitui_;
    std::string out;
    const u32 caret = caret_row(g.segs, g.caret);
    for (u32 k = 0; k < g.field_rows && g.field_top + k < g.segs.size(); ++k) {
        const MsgRow& m = g.segs[g.field_top + k];
        const u32 limit = m.line == 0 ? 50 : 72;
        std::string row;
        u32 col = m.col;
        for (size_t i = m.b0; i <= m.b1; ++i) {
            if (g.field_top + k == caret && i == g.caret) row += '^';
            if (i == m.b1) break;
            if (col == limit && !is_cont(static_cast<u8>(g.message[i]))) row += '|';
            row += g.message[i];
            if (!is_cont(static_cast<u8>(g.message[i]))) ++col;
        }
        out += (k ? " / " : "") + row;
    }
    return "field " + std::to_string(g.field_rows) + " of " + std::to_string(g.segs.size()) + ": " + out;
}

}  // namespace blit
