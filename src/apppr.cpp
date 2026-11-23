// Pull requests, through GitHub's own command, gh (its sign-in, its idea of
// which repository this is): the open ones listed; one checked out, its
// changes shown, the comments on its lines listed (each gone to, and
// answered) and shown after those lines in the files; and one approved,
// sent back for changes, or commented on. Each runs in the background;
// what posts to GitHub asks first.
#include <algorithm>
#include <cstdlib>

#include "app.hpp"
#include "json.hpp"
#include "look.hpp"
#include "textbox.hpp"

namespace blit {

struct PrInfo {
    i64 number = 0;
    std::string title, author, branch, base, decision;
    bool draft = false;
};

struct PrComment {
    i64 id = 0, reply_to = 0;
    std::string path, author, body;
    i64 line = 0;  // in the pull request's version of the file (0: the line has changed since: not placed)
    bool taken_out = false;  // on a line the pull request takes out (its side is LEFT, its line the base's): listed, not placed
};

struct PrUi {
    std::vector<PrInfo> open;
    i64 number = 0;                  // the one the comments are of
    std::vector<PrComment> comments;
    // The notes after the commented lines on show, for each view.
    struct Notes {
        u64 key[5] = {};
        std::vector<GridNote> notes;
        std::vector<std::vector<u32>> cells;
    } views[kMaxViews];
    std::vector<GridNote> merged[kMaxViews];
};

namespace {

std::string gh_program() {
    const char* own = std::getenv("BLIT_GH");  // (the tests: a stand-in)
    return own && *own ? own : "gh";
}

std::string first_line(std::string_view s) {
    const size_t nl = s.find('\n');
    std::string out(s.substr(0, nl));
    std::erase(out, '\r');
    return out;
}

// What gh said when it failed (or, never run, why: gh not installed).
std::string gh_failed(int code, const std::string& out, const std::string& err) {
    return code < 0 ? first_line(err) : "gh: " + first_line(err.empty() ? out : err);
}

}  // namespace

// The command gh is run with, in the repository (its answer to done).
void App::pr_run(std::vector<std::string> args, std::function<void(int, const std::string&, const std::string&)> done) {
    const std::string root = git_root();
    if (root.empty()) return notify("Not in a git repository");
    args.insert(args.begin(), gh_program());
    script_run(std::move(args), root, std::move(done));
}

// "Git: Pull requests…": the open ones, then what to do with the one chosen.
void App::pr_list() {
    if (!pr_) pr_ = std::make_shared<PrUi>();
    pr_run({"pr", "list", "--limit", "50", "--json", "number,title,author,headRefName,baseRefName,isDraft,reviewDecision"},
           [this](int code, const std::string& out, const std::string& err) {
               if (code != 0) return notify(gh_failed(code, out, err), code < 0 ? 1 : 0);
               PrUi& u = *pr_;
               u.open.clear();
               json::Doc doc;
               if (doc.parse(out))
                   for (const json::Value p : doc.root()) {
                       PrInfo i;
                       i.number = p["number"].integer();
                       i.title = std::string(p["title"].str());
                       i.author = std::string(p["author"]["login"].str());
                       i.branch = std::string(p["headRefName"].str());
                       i.base = std::string(p["baseRefName"].str());
                       i.draft = p["isDraft"].boolean();
                       i.decision = std::string(p["reviewDecision"].str());
                       u.open.push_back(std::move(i));
                   }
               if (u.open.empty()) return notify("No open pull requests");
               std::vector<std::string> rows;
               for (const PrInfo& i : u.open) {
                   std::string state = i.draft ? "draft" : i.decision == "APPROVED" ? "approved" : i.decision == "CHANGES_REQUESTED" ? "changes asked for" : "";
                   rows.push_back("#" + std::to_string(i.number) + "  " + i.title + "  \xC2\xB7 " + i.author + ", " + i.branch + (state.empty() ? "" : "  \xC2\xB7 " + state));
               }
               find_choose_item("Pull requests", std::move(rows), [this](int k) {
                   if (k >= 0 && static_cast<size_t>(k) < pr_->open.size()) pr_menu(pr_->open[static_cast<size_t>(k)].number);
               });
           });
}

void App::pr_menu(i64 n) {
    const std::string tag = "#" + std::to_string(n);
    std::vector<std::string> rows = {"Check out " + tag, "Show the changes of " + tag, "The comments on its lines", "Approve " + tag, "Ask for changes to " + tag + "\xE2\x80\xA6",
                                     "Comment on " + tag + "\xE2\x80\xA6"};
    find_choose_item("Pull request " + tag, std::move(rows), [this, n](int k) {
        switch (k) {
            case 0: pr_checkout(n); break;
            case 1: pr_changes(n); break;
            case 2: pr_comments(n, true); break;
            case 3: pr_review(n, "approve"); break;
            case 4: pr_review(n, "request-changes"); break;
            case 5: pr_review(n, "comment"); break;
            default: break;
        }
    });
}

// Checked out (gh makes or updates its branch), and its comments asked for:
// they show after their lines in the files from then on.
void App::pr_checkout(i64 n) {
    pr_run({"pr", "checkout", std::to_string(n)}, [this, n](int code, const std::string& out, const std::string& err) {
        if (code != 0) return notify(gh_failed(code, out, err), code < 0 ? 1 : 0);
        if (cfg_.git) git_.refresh();
        reload_changed();  // (the files are the pull request's now)
        notify("Checked out #" + std::to_string(n));
        pr_comments(n, false);
    });
}

// Its changes, as gh gives them, in a tab of their own, coloured as a diff.
void App::pr_changes(i64 n) {
    pr_run({"pr", "diff", std::to_string(n), "--color", "never"}, [this, n](int code, const std::string& out, const std::string& err) {
        if (code != 0) return notify(gh_failed(code, out, err), code < 0 ? 1 : 0);
        std::string text;
        std::vector<u32> off;
        std::vector<u8> style;
        for (size_t at = 0; at < out.size();) {
            size_t nl = out.find('\n', at);
            if (nl == std::string::npos) nl = out.size();
            const std::string_view line(out.data() + at, nl - at);
            at = nl + 1;
            const u8 st = line.starts_with("+++") || line.starts_with("---") || line.starts_with("diff ") || line.starts_with("index ") ? 4
                          : line.starts_with("@@")                                                                             ? 3
                          : line.starts_with("+")                                                                              ? 1
                          : line.starts_with("-")                                                                              ? 2
                                                                                                                               : 0;
            if (style.empty() || style.back() != st) off.push_back(static_cast<u32>(text.size())), style.push_back(st);
            text += line;
            text += '\n';
        }
        if (text.empty()) text = "(no changes)", off = {0}, style = {4};
        Doc* d = add_doc(pane_->tab + 1);
        d->title = "#" + std::to_string(n) + " (changes)";
        d->read_only = true;
        debug_tab_fill(*d, std::move(text), std::move(off), std::move(style), 0, 0);
        tab_show(pane_->tab + 1);
        pick_theme();
    });
}

// The comments on its lines (as threads: each answer under what it
// answers), asked for; listed, if `show`: Enter goes to the line, and the
// comment can be answered from there.
void App::pr_comments(i64 n, bool show) {
    if (!pr_) pr_ = std::make_shared<PrUi>();
    pr_run({"api", "repos/{owner}/{repo}/pulls/" + std::to_string(n) + "/comments", "--paginate"}, [this, n, show](int code, const std::string& out, const std::string& err) {
        if (code != 0) return notify(gh_failed(code, out, err), code < 0 ? 1 : 0);
        PrUi& u = *pr_;
        u.number = n;
        u.comments.clear();
        json::Doc doc;
        if (doc.parse(out))
            for (const json::Value c : doc.root()) {
                PrComment m;
                m.id = c["id"].integer();
                m.reply_to = c["in_reply_to_id"].integer();
                m.path = std::string(c["path"].str());
                m.author = std::string(c["user"]["login"].str());
                m.body = std::string(c["body"].str());
                m.line = c["line"].integer();
                m.taken_out = c["side"].str() == "LEFT";
                u.comments.push_back(std::move(m));
            }
        for (PrUi::Notes& v : u.views) v.key[0] = ~0ull;
        dirty_ = true;
        if (!show) return;
        if (u.comments.empty()) return notify("No comments on the lines of #" + std::to_string(n));
        // Threads: each first comment, then its answers.
        std::vector<size_t> order;
        for (size_t i = 0; i < u.comments.size(); ++i) {
            if (u.comments[i].reply_to) continue;
            order.push_back(i);
            for (size_t j = 0; j < u.comments.size(); ++j)
                if (u.comments[j].reply_to == u.comments[i].id) order.push_back(j);
        }
        std::vector<std::string> rows;
        for (const size_t i : order) {
            const PrComment& c = u.comments[i];
            rows.push_back((c.reply_to ? "    " : "") + c.path + (c.taken_out ? " (taken out)" : c.line ? ":" + std::to_string(c.line) : " (changed since)") + "  " + c.author + ": " + first_line(c.body));
        }
        find_choose_item("Comments on #" + std::to_string(n), std::move(rows), [this, order](int k) {
            if (k < 0 || static_cast<size_t>(k) >= order.size()) return;
            pr_comment_menu(order[static_cast<size_t>(k)]);
        });
    });
}

// A comment chosen: gone to (its file, at its line; one on a line taken
// out, the file), or answered.
void App::pr_comment_menu(size_t i) {
    PrUi& u = *pr_;
    if (i >= u.comments.size()) return;
    const PrComment c = u.comments[i];
    const i64 line = c.taken_out ? 0 : c.line;
    find_choose_item(c.author + ": " + first_line(c.body), {"Go to " + c.path + (line ? ":" + std::to_string(line) : ""), "Answer\xE2\x80\xA6"}, [this, c, line](int k) {
        if (k == 0) {
            remember_place();
            if (!open_path((git_root() + "/" + c.path).c_str())) return;
            if (line > 0) {
                Editor& ed = pane_->doc->ed;
                ed.move_to(ed.buf.line_start(std::min<u64>(static_cast<u64>(line - 1), ed.buf.line_count() - 1)), false);
                scroll_to_cursor();
            }
        } else if (k == 1) {
            const i64 to = c.reply_to ? c.reply_to : c.id;  // (an answer goes to the thread's first comment)
            find_choose(kFindInput, "Answer " + c.author, {}, [this, to](const std::string& text) {
                if (text.empty() || !confirm("Post this answer on GitHub?\n\n" + text, "Post")) return;
                const i64 n = pr_->number;
                pr_run({"api", "-X", "POST", "repos/{owner}/{repo}/pulls/" + std::to_string(n) + "/comments/" + std::to_string(to) + "/replies", "-f", "body=" + text},
                       [this, n](int code, const std::string& out, const std::string& err) {
                           if (code != 0) return notify(gh_failed(code, out, err), code < 0 ? 1 : 0);
                           notify("Answered");
                           pr_comments(n, false);
                       });
            });
        }
    });
}

// Approved, sent back for changes, or commented on (the text asked for, but
// for approving); asked first, for it is posted.
void App::pr_review(i64 n, const std::string& kind) {
    auto send = [this, n, kind](const std::string& text) {
        const std::string what = kind == "approve" ? "Approve #" + std::to_string(n) + " on GitHub?" : kind == "request-changes" ? "Ask for changes to #" + std::to_string(n) + " on GitHub?" : "Post this comment on #" + std::to_string(n) + " on GitHub?";
        if (!confirm(what + (text.empty() ? std::string() : "\n\n" + text), kind == "approve" ? "Approve" : "Post")) return;
        std::vector<std::string> args = {"pr", "review", std::to_string(n), "--" + kind};
        if (!text.empty()) args.insert(args.end(), {"--body", text});
        pr_run(std::move(args), [this, kind, n](int code, const std::string& out, const std::string& err) {
            if (code != 0) return notify(gh_failed(code, out, err), code < 0 ? 1 : 0);
            notify(kind == "approve" ? "Approved #" + std::to_string(n) : kind == "request-changes" ? "Asked for changes to #" + std::to_string(n) : "Commented on #" + std::to_string(n));
        });
    };
    if (kind == "approve") return send({});
    find_choose(kFindInput, kind == "request-changes" ? "What should change" : "Comment", {}, [send](const std::string& text) {
        if (!text.empty()) send(text);
    });
}

// The comments of the pull request whose comments were asked for, after
// their lines in the files on show (those of the repository). One on a
// line the pull request takes out is not: its line is the base's, and
// the file has no such line to put it after.
void App::pr_marks(FrameDesc& fd) {
    if (!pr_ || pr_->comments.empty()) return;
    PrUi& u = *pr_;
    const std::string root = git_root();
    if (root.empty()) return;
    Font& font = *font_;
    for (u32 vi = 0; vi < fd.view_count && vi < kMaxViews; ++vi) {
        GridSpec& g = fd.views[vi].grid;
        Doc* d = find_doc(g.doc);
        if (!d || d->ed.path.empty() || d->read_only) continue;
        const std::string full = full_path(d->ed.path);
        if (!full.starts_with(root + "/")) continue;
        const std::string rel = full.substr(root.size() + 1);
        PrUi::Notes& v = u.views[vi];
        const u64 key[5] = {d->id, d->ed.revision(), g.top_line * 1000003 + g.bottom(), u.comments.size() + static_cast<u64>(u.number) * 1000003, font.generation()};
        if (!std::equal(std::begin(key), std::end(key), std::begin(v.key))) {
            std::copy(std::begin(key), std::end(key), std::begin(v.key));
            v.notes.clear();
            v.cells.clear();
            for (const PrComment& c : u.comments) {
                if (c.path != rel || c.line <= 0 || c.reply_to || c.taken_out) continue;
                const u64 line = static_cast<u64>(c.line - 1);
                if (line < g.top_line || line >= g.bottom() || line >= d->ed.buf.line_count()) continue;
                if (std::ranges::any_of(v.notes, [&](const GridNote& x) { return x.line == line; })) continue;
                size_t answers = 0;
                for (const PrComment& a : u.comments) answers += a.reply_to == c.id;
                std::string said = "\xE2\x97\x86 " + c.author + ": " + first_line(c.body);  // ◆
                if (answers) said += "  (+" + std::to_string(answers) + ")";
                TextBox box;
                box.resize(std::min<u32>(width_of(said), 100), 1);
                put(box, font, 0, 0, said, ink::kAccent);
                v.cells.push_back(std::move(box.cells));
                const u64 start = d->ed.buf.line_start(line);
                v.notes.push_back({line, d->ed.col_between(start, d->ed.line_end(line)) + 3, nullptr, static_cast<u32>(v.cells.back().size())});
            }
            for (size_t k = 0; k < v.notes.size(); ++k) v.notes[k].cells = v.cells[k].data();
        }
        if (v.notes.empty()) continue;
        u.merged[vi].assign(g.notes.begin(), g.notes.end());
        for (const GridNote& n : v.notes)
            if (std::ranges::none_of(g.notes, [&](const GridNote& o) { return o.line == n.line; })) u.merged[vi].push_back(n);
        g.notes = u.merged[vi];
    }
}

// For the tests: the comments asked for, as "path:line author" each.
std::string App::pr_dump() {
    if (!pr_ || pr_->comments.empty()) return "no comments";
    std::string out = "#" + std::to_string(pr_->number) + ":";
    for (const PrComment& c : pr_->comments) out += " " + c.path + ":" + std::to_string(c.line) + " " + c.author + (c.reply_to ? "(answer)" : "") + (c.taken_out ? "(taken out)" : "");
    size_t notes = 0;
    for (const PrUi::Notes& v : pr_->views) notes += v.notes.size();
    return out + " | notes " + std::to_string(notes);
}

}  // namespace blit
