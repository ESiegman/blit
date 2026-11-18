#pragma once
// Git, by running the `git` program. Everything that waits (a status, a
// fetch from a server, a blame of a long file) happens on a thread of its
// own; the editor queues a request and later picks up a result that is
// already parsed. Nothing here links against a git library, so whatever the
// user's git can do (credential helpers, ssh agents, hooks, their settings)
// works the way it does in their terminal.
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.hpp"

namespace blit {

// One changed file. `index` and `work` are git's letters for what is staged
// and what is not: M modified, A added, D deleted, R renamed, C copied, T
// type changed, U unmerged, '.' nothing; an untracked file is '?' in both.
struct GitEntry {
    std::string path;
    std::string from;  // what it was renamed from
    char index = '.', work = '.';
    bool conflict = false;
    bool staged() const { return index != '.' && index != '?' && !conflict; }
    bool unstaged() const { return (work != '.' || index == '?') && !conflict; }
};

struct GitStatus {
    bool repo = false;
    std::string root;      // the top of the working tree
    std::string branch;    // empty when no branch is checked out
    std::string upstream;  // the branch it follows, if any
    std::string head;      // the commit, shortened
    int ahead = 0, behind = 0;
    bool merging = false;  // a merge is under way (it stopped for conflicts)
    bool rebasing = false, picking = false;  // and a rebase, or a cherry-pick
    std::vector<GitEntry> entries;
};

struct GitBranch {
    std::string name;
    bool current = false, remote = false;
};

struct GitCommit {
    std::string hash, author, when, subject;
    std::string full;                  // the whole hash
    std::vector<std::string> parents;  // theirs, whole
    std::string refs;                  // what points at it, as git decorates it ("HEAD -> main, origin/main, tag: v1")
    i64 time = 0;                      // committed, in seconds since 1970
};

// History as a graph: for each commit of a log (newest first), the row of
// cells before it that draws the lanes, worked out from the parents (a
// lane for each line of descent shown: a commit on its lane, ● ; lanes
// passing │, a branch that started there joining it ╯ ╰, a merge's other
// parent starting one ╮ ╭ or joining one that is there ┤ ├, ─ and ┼ between).
// Each cell is a character (U' ' for none) and the lane it is of, by which
// it is coloured; at most `most_lanes` are drawn, the rest cut.
struct GitGraphRow {
    std::u32string cells;
    std::vector<u8> lanes;
};
std::vector<GitGraphRow> graph_lanes(std::span<const GitCommit> log, u32 most_lanes = 8);

// A stash: its name ("stash@{0}"), how long ago it was made, and what git
// called it ("WIP on main: 1a2b3c4 Start").
struct GitStash {
    std::string name, when, subject;
};

// A run of lines that differ from the committed text, in line numbers of
// the text as it is now.
struct GitHunk {
    enum : u8 { kAdded = 1, kModified = 2, kDeleted = 3 };
    u32 line = 0;
    u32 count = 0;  // 0 for kDeleted: lines are gone from before `line`
    u8 kind = 0;
    u32 removed = 0;  // lines of the committed text it took the place of
    bool staged = false;  // the text there is what is staged (the index has it)
    bool operator==(const GitHunk&) const = default;
};

// A diff as `git diff` prints it, read: where each hunk is, by line of the
// text, and the header of the file it belongs to.
struct GitDiff {
    struct Hunk {
        u32 first = 0, last = 0;      // its "@@" line, and one past its last line
        u32 head0 = 0, head1 = 0;     // its file's header: "diff --git" up to the first "@@"
        u32 old_start = 0, new_start = 0;
        u32 old_count = 0, new_count = 0;
    };
    std::vector<Hunk> hunks;
    std::vector<u64> line_off;  // where each line starts, and then the end of the text
    u32 lines() const { return line_off.empty() ? 0 : static_cast<u32>(line_off.size() - 1); }
    // The hunk a line of the diff is in (its "@@" line too), or -1.
    int hunk_at(u32 line) const;
};
GitDiff parse_diff(std::string_view text);
// A patch of the chosen lines of a diff (`chosen` has a flag per line of
// it), for `git apply --cached`: each hunk with a chosen line added or
// taken away, with its other changes left undone. Forwards (staging), a
// line added that is not chosen is left out and one taken away that is not
// is kept, as context; backwards (unstaging, applied with -R) the other way
// about. Empty when no change is chosen.
std::string diff_patch(std::string_view text, const GitDiff& diff, std::span<const u8> chosen, bool reverse);

// What a diff tab shows, made on git's thread: the diff, or its hunks in two
// columns side by side (each line beside the one it became, and within a
// changed line what stayed the same muted), with the colours of its text.
struct GitDiffView {
    std::string text;
    // Colours: from each offset on, a style: 1 added, 2 taken away, 3 a
    // hunk's heading, 4 muted (the other headings, line numbers, and in
    // two columns what a changed line kept).
    std::vector<u32> off;
    std::vector<u8> style;
    // Two columns: for each line of the text, the lines of the diff it shows
    // on the left and on the right (~0u: none). Empty for one column, whose
    // lines are the diff's own.
    std::vector<u32> left, right;
    u32 cols = 0;  // two columns: the room they were made for; 0 for one column
};
// The view of a diff: in one column (cols 0) or two, laid out for a text
// area `cols` wide, with tabs `tab_width` apart.
void diff_view(std::string_view text, const GitDiff& diff, u32 cols, u32 tab_width, GitDiffView* out);

// The text the index should hold for a file so that lines [line0, line1)
// of the text now are staged (stage), or go back to the committed text
// (not stage): the rest is left as the index has it. Lines in all three
// are whole lines with their ends.
std::string stage_lines(std::string_view head, std::string_view index, std::string_view now, u32 line0, u32 line1, bool stage);

// Who last changed each line.
struct GitBlame {
    struct Commit {
        std::string hash, author, summary;
        i64 time = 0;  // seconds since 1970; 0 for lines not committed yet
    };
    std::vector<Commit> commits;
    std::vector<u32> lines;  // per line of the text, an index into commits
};

// What git prints, read. (Separate from running it, so that tests can.)
GitStatus parse_status(std::string_view porcelain_v2_z);
std::vector<GitBranch> parse_branches(std::string_view text, std::vector<std::string>* tags = nullptr);
std::vector<GitCommit> parse_log(std::string_view text);
std::vector<GitStash> parse_stashes(std::string_view text);
bool parse_blame(std::string_view porcelain, GitBlame* out);

// A number per line of a text, equal for equal lines. A last line without a
// newline counts; the nothing after a final newline does not.
std::vector<u64> line_hashes(std::string_view text);
std::vector<u64> line_hashes(std::span<const Piece> pieces);
// The runs of lines in `now` that differ from `base` (Myers' algorithm).
// Texts too far apart to be worth comparing come back as one modified run.
std::vector<GitHunk> diff_lines(std::span<const u64> base, std::span<const u64> now);
// "3 days ago", from two times in seconds.
std::string relative_age(i64 then, i64 now);

class Git {
public:
    enum : u32 {
        kStatus = 1,  // status, branches and history, read afresh
        kAction,      // a command that changes something has finished
        kHunks,       // a document's changed lines
        kBlame,
        kText,        // the output of a command, for showing in a tab
        kView,        // a diff for a tab: read, and laid out (see diff_view)
    };
    struct Result {
        u32 kind = 0;
        bool ok = false;
        u64 doc = 0, revision = 0;  // which document it is about, and which state of its text
        std::string label;          // an action: what it was ("Push")
        std::string text;           // output, or what went wrong
        GitStatus status;
        std::vector<GitBranch> branches;
        std::vector<GitCommit> log;
        std::string log_of;                // kStatus: the branch the log is of (empty: the one checked out; "--all": every one)
        std::string log_find;              // and what it was searched for (empty: none)
        std::vector<std::string> tags;     // newest (by version) first
        std::vector<GitStash> stashes;
        std::vector<GitHunk> hunks;
        bool tracked = false;  // kHunks: the file has a committed version to compare with
        std::shared_ptr<GitBlame> blame;
        // kView: the diff as git printed it, read, and as the tab shows it.
        std::shared_ptr<const std::string> source;
        std::shared_ptr<const GitDiff> diff;
        std::shared_ptr<GitDiffView> view;
    };

    Git();
    Git(const Git&) = delete;
    Git& operator=(const Git&) = delete;
    ~Git();

    // Called on the worker thread whenever poll() has something.
    void set_notify(std::function<void()> fn);
    // The folder whose repository this is about (the repository may start
    // above it). Asks for a status.
    void open(const std::string& dir);
    void refresh();
    // Runs `git ARGS` in the repository, then reads the status again.
    // `input` goes to its standard input.
    void run(std::vector<std::string> args, std::string label, std::string input = {});
    // Stops the command that is running, if it is one started by run().
    void cancel();
    // Compares a document with its committed version. `path` is the file's
    // full path; the pieces are the text as it is now.
    void hunks(u64 doc, u64 revision, const std::string& path, std::vector<Piece> pieces);
    void blame(u64 doc, u64 revision, const std::string& path, std::vector<Piece> pieces);
    // The output of `git ARGS`, for a tab.
    void text(u64 doc, std::vector<std::string> args, std::string label);
    // A diff for a tab (kView): the output of `git ARGS` read and laid out
    // as diff_view() does; or, with `source`, that diff laid out again
    // (git is not run).
    void view(u64 doc, std::vector<std::string> args, std::shared_ptr<const std::string> source, u32 cols, u32 tab_width);
    // Stages lines [line0, line1) of a document's text as it is (saved or
    // not), or puts them back in the index as they were committed: the
    // index is written with git's own plumbing, not through a patch.
    // Reported as an action, then a status.
    void stage(u64 doc, const std::string& path, std::vector<Piece> pieces, u32 line0, u32 line1, bool stage, std::string label);
    // The branch whose commits a status lists (empty: the one checked out;
    // "--all": every branch's).
    void log_of(const std::string& branch);
    // Those of its commits a status lists: whose message or author has this
    // in it (any case), or that changed it, a path; empty: all.
    void log_find(const std::string& text);

    // A document is about to go, or its text to be replaced: nothing asked
    // about it may still be reading that text when this returns. Git running
    // for it is stopped, not waited for.
    void forget(u64 doc);
    // Git is running for that document now (its committed text, its blame).
    bool working_on(u64 doc) const;

    std::vector<Result> poll();
    // What run() is doing right now ("Push"), or empty.
    std::string busy() const;
    // Nothing queued and nothing running.
    bool idle() const;

private:
    struct Worker;
    std::shared_ptr<Worker> w_;
};

}  // namespace blit
