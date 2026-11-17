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
    std::vector<GitEntry> entries;
};

struct GitBranch {
    std::string name;
    bool current = false, remote = false;
};

struct GitCommit {
    std::string hash, author, when, subject;
};

// A run of lines that differ from the committed text, in line numbers of
// the text as it is now.
struct GitHunk {
    enum : u8 { kAdded = 1, kModified = 2, kDeleted = 3 };
    u32 line = 0;
    u32 count = 0;  // 0 for kDeleted: lines are gone from before `line`
    u8 kind = 0;
    u32 removed = 0;  // lines of the committed text it took the place of
    bool operator==(const GitHunk&) const = default;
};

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
std::vector<GitBranch> parse_branches(std::string_view text);
std::vector<GitCommit> parse_log(std::string_view text);
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
        std::vector<GitHunk> hunks;
        bool tracked = false;  // kHunks: the file has a committed version to compare with
        std::shared_ptr<GitBlame> blame;
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
