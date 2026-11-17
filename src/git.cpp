#include "git.hpp"

#include "remote.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "lsp.hpp"
#include "qos.hpp"
#include "utf8.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace blit {

// ---------------------------------------------------------------- reading git's output

namespace {

// The rest of `s` after its n-th space (git's records are fields separated
// by single spaces, with the path, which may hold spaces, last).
std::string_view after_fields(std::string_view s, int n) {
    size_t at = 0;
    for (int i = 0; i < n; ++i) {
        at = s.find(' ', at);
        if (at == std::string_view::npos) return {};
        ++at;
    }
    return s.substr(at);
}

bool is_hex40(std::string_view s) {
    if (s.size() < 41 || s[40] != ' ') return false;
    for (size_t i = 0; i < 40; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

}  // namespace

GitStatus parse_status(std::string_view z) {
    GitStatus st;
    st.repo = true;
    while (!z.empty()) {
        const size_t nul = z.find('\0');
        const std::string_view rec = z.substr(0, nul);
        z = nul == std::string_view::npos ? std::string_view{} : z.substr(nul + 1);
        if (rec.size() < 3) continue;
        if (rec.starts_with("# branch.oid ")) {
            st.head = rec.substr(13, 8);
            if (st.head.starts_with("(")) st.head.clear();  // "(initial)": no commit yet
        } else if (rec.starts_with("# branch.head ")) {
            st.branch = rec.substr(14);
            if (st.branch == "(detached)") st.branch.clear();
        } else if (rec.starts_with("# branch.upstream ")) {
            st.upstream = rec.substr(18);
        } else if (rec.starts_with("# branch.ab ")) {
            std::sscanf(std::string(rec.substr(12)).c_str(), "+%d -%d", &st.ahead, &st.behind);
        } else if (rec[0] == '1' || rec[0] == '2' || rec[0] == 'u') {
            GitEntry e;
            e.index = rec[2];
            e.work = rec.size() > 3 ? rec[3] : '.';
            e.conflict = rec[0] == 'u';
            e.path = after_fields(rec, rec[0] == '1' ? 8 : rec[0] == '2' ? 9 : 10);
            if (rec[0] == '2') {  // a rename: the old name is the next record
                const size_t next = z.find('\0');
                e.from = z.substr(0, next);
                z = next == std::string_view::npos ? std::string_view{} : z.substr(next + 1);
            }
            if (!e.path.empty()) st.entries.push_back(std::move(e));
        } else if (rec[0] == '?') {
            GitEntry e;
            e.index = e.work = '?';
            e.path = rec.substr(2);
            st.entries.push_back(std::move(e));
        }
    }
    return st;
}

namespace {

// Names in the order a person counts them: "v1.10" after "v1.9", the runs
// of digits compared as numbers.
bool version_less(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])), db = std::isdigit(static_cast<unsigned char>(b[j]));
        if (da && db) {
            size_t i1 = i, j1 = j;
            while (i1 < a.size() && std::isdigit(static_cast<unsigned char>(a[i1]))) ++i1;
            while (j1 < b.size() && std::isdigit(static_cast<unsigned char>(b[j1]))) ++j1;
            std::string_view x = a.substr(i, i1 - i), y = b.substr(j, j1 - j);
            while (x.size() > 1 && x[0] == '0') x.remove_prefix(1);
            while (y.size() > 1 && y[0] == '0') y.remove_prefix(1);
            if (x.size() != y.size()) return x.size() < y.size();
            if (x != y) return x < y;
            i = i1, j = j1;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i, ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

}  // namespace

std::vector<GitBranch> parse_branches(std::string_view text, std::vector<std::string>* tags) {
    std::vector<GitBranch> out;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        std::string_view ref = line.substr(tab + 1);
        GitBranch b;
        b.current = line.starts_with("*");
        if (ref.starts_with("refs/heads/")) {
            ref.remove_prefix(11);
        } else if (ref.starts_with("refs/remotes/")) {
            ref.remove_prefix(13);
            b.remote = true;
            if (ref.ends_with("/HEAD")) continue;  // a pointer to another branch, not one itself
        } else {
            if (tags && ref.starts_with("refs/tags/")) tags->emplace_back(ref.substr(10));
            continue;
        }
        b.name = ref;
        out.push_back(std::move(b));
    }
    if (tags) std::sort(tags->begin(), tags->end(), [](const std::string& a, const std::string& b) { return version_less(b, a); });
    return out;
}

std::vector<GitStash> parse_stashes(std::string_view text) {
    std::vector<GitStash> out;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        GitStash s;
        std::string* const fields[] = {&s.name, &s.when, &s.subject};
        for (std::string* f : fields) {
            const size_t sep = line.find('\x1f');
            *f = line.substr(0, sep);
            line = sep == std::string_view::npos ? std::string_view{} : line.substr(sep + 1);
        }
        if (s.name.starts_with("stash@{")) out.push_back(std::move(s));
    }
    return out;
}

std::vector<GitCommit> parse_log(std::string_view text) {
    std::vector<GitCommit> out;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        GitCommit c;
        std::string* const fields[] = {&c.hash, &c.author, &c.when, &c.subject};
        for (std::string* f : fields) {
            const size_t sep = line.find('\x1f');
            *f = line.substr(0, sep);
            line = sep == std::string_view::npos ? std::string_view{} : line.substr(sep + 1);
        }
        if (!c.hash.empty()) out.push_back(std::move(c));
    }
    return out;
}

// `git blame --porcelain`: for each line of the file a header ("<hash> <line
// then> <line now> [<how many>]"), the first time a commit is met some
// "key value" lines about it, and then the line itself after a tab.
bool parse_blame(std::string_view text, GitBlame* out) {
    out->commits.clear();
    out->lines.clear();
    std::unordered_map<std::string, u32> seen;
    u32 current = 0;
    bool have = false;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.starts_with("\t")) {  // the line itself: it belongs to the commit just named
            if (!have) return false;
            out->lines.push_back(current);
            have = false;
        } else if (!have && is_hex40(line)) {
            const std::string hash(line.substr(0, 40));
            const auto [it, fresh] = seen.try_emplace(hash, static_cast<u32>(out->commits.size()));
            if (fresh) {
                out->commits.emplace_back();
                out->commits.back().hash = hash;
            }
            current = it->second;
            have = true;
        } else if (have) {
            GitBlame::Commit& c = out->commits[current];
            if (line.starts_with("author ")) c.author = line.substr(7);
            else if (line.starts_with("author-time ")) c.time = std::strtoll(std::string(line.substr(12)).c_str(), nullptr, 10);
            else if (line.starts_with("summary ")) c.summary = line.substr(8);
        }
    }
    // Lines not committed yet are blamed on a commit of all zeros.
    for (GitBlame::Commit& c : out->commits)
        if (c.hash.find_first_not_of('0') == std::string::npos) {
            c.time = 0;
            c.author = "Not committed yet";
            c.summary.clear();
        }
    return !out->lines.empty();
}

std::string relative_age(i64 then, i64 now) {
    const i64 s = std::max<i64>(0, now - then);
    static const struct {
        i64 seconds;
        const char* one;
        const char* many;
    } units[] = {{365 * 86400, "a year", "years"}, {30 * 86400, "a month", "months"}, {7 * 86400, "a week", "weeks"},
                 {86400, "a day", "days"},         {3600, "an hour", "hours"},        {60, "a minute", "minutes"}};
    for (const auto& u : units)
        if (s >= u.seconds) {
            const i64 n = s / u.seconds;
            return n == 1 ? std::string(u.one) + " ago" : std::to_string(n) + " " + u.many + " ago";
        }
    return "just now";
}

// ---------------------------------------------------------------- comparing lines

std::vector<u64> line_hashes(std::string_view text) {
    const Piece whole{text.data(), static_cast<u32>(std::min<size_t>(text.size(), 0xFFFFFFFFu)), 0};
    return line_hashes(std::span<const Piece>(&whole, text.empty() ? 0 : 1));
}

std::vector<u64> line_hashes(std::span<const Piece> pieces) {
    std::vector<u64> out;
    std::string carry;  // the start of a line that crosses from one piece into the next
    bool open = false;  // a line has begun and not ended
    const std::hash<std::string_view> hash;
    for (const Piece& pc : pieces) {
        const char* p = pc.p;
        size_t n = pc.len;
        while (n > 0) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', n));
            if (!nl) {
                carry.append(p, n);
                open = true;
                break;
            }
            const std::string_view rest(p, static_cast<size_t>(nl - p));
            if (open) {
                carry.append(rest);
                out.push_back(hash(carry));
                carry.clear();
                open = false;
            } else {
                out.push_back(hash(rest));
            }
            n -= rest.size() + 1;
            p = nl + 1;
        }
    }
    if (open) out.push_back(hash(carry));
    return out;
}

std::vector<GitHunk> diff_lines(std::span<const u64> base, std::span<const u64> now) {
    std::vector<GitHunk> out;
    // What the two have in common at either end is no part of the problem.
    size_t pre = 0;
    while (pre < base.size() && pre < now.size() && base[pre] == now[pre]) ++pre;
    size_t suf = 0;
    while (suf < base.size() - pre && suf < now.size() - pre && base[base.size() - 1 - suf] == now[now.size() - 1 - suf]) ++suf;
    const std::span<const u64> a = base.subspan(pre, base.size() - pre - suf), b = now.subspan(pre, now.size() - pre - suf);
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    // Lines [a0, a1) of the old text became lines [b0, b1) of the new.
    auto emit = [&](int a0, int a1, int b0, int b1) {
        if (a0 == a1 && b0 == b1) return;
        const u8 kind = a0 == a1 ? GitHunk::kAdded : b0 == b1 ? GitHunk::kDeleted : GitHunk::kModified;
        out.push_back({static_cast<u32>(pre) + static_cast<u32>(b0), static_cast<u32>(b1 - b0), kind, static_cast<u32>(a1 - a0)});
    };
    if (n == 0 || m == 0) {
        emit(0, n, 0, m);
        return out;
    }

    // Myers: the furthest each diagonal k = x - y reaches with d edits, for
    // d = 0, 1, 2... until one reaches the end of both. What each round
    // started from is kept, to walk back along the path afterwards. That
    // is d numbers for round d, so the search gives up beyond a number of
    // edits past which the answer would be "nearly everything" anyway.
    constexpr int kMaxEdits = 1500;
    const int limit = std::min(n + m, kMaxEdits);
    std::vector<int> v(static_cast<size_t>(2 * limit + 3), 0);
    auto V = [&](int k) -> int& { return v[static_cast<size_t>(k + limit + 1)]; };
    std::vector<std::vector<int>> trace;
    int edits = -1;
    for (int d = 0; d <= limit && edits < 0; ++d) {
        trace.emplace_back(v.begin() + (limit + 1 - d), v.begin() + (limit + 1 + d) + 1);  // diagonals -d..d
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && V(k - 1) < V(k + 1))) ? V(k + 1) : V(k - 1) + 1;
            int y = x - k;
            while (x < n && y < m && a[static_cast<size_t>(x)] == b[static_cast<size_t>(y)]) ++x, ++y;
            V(k) = x;
            if (x >= n && y >= m) {
                edits = d;
                break;
            }
        }
    }
    if (edits < 0) {
        emit(0, n, 0, m);
        return out;
    }
    // Back along the path, noting each edit: where it was in both texts,
    // and whether it took a line of the old one or gave one of the new.
    struct Edit {
        int x, y;
        bool removed;
    };
    std::vector<Edit> path;
    int x = n, y = m;
    for (int d = edits; d > 0; --d) {
        const std::vector<int>& t = trace[static_cast<size_t>(d)];
        auto T = [&](int k) { return t[static_cast<size_t>(k + d)]; };
        const int k = x - y;
        const int from = (k == -d || (k != d && T(k - 1) < T(k + 1))) ? k + 1 : k - 1;
        const int px = T(from), py = px - from;
        path.push_back({px, py, from == k - 1});
        x = px;
        y = py;
    }
    std::reverse(path.begin(), path.end());
    // Edits with nothing unchanged between them are one run.
    int a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    bool open = false;
    for (const Edit& e : path) {
        if (!open || e.x != a1 || e.y != b1) {
            if (open) emit(a0, a1, b0, b1);
            a0 = a1 = e.x;
            b0 = b1 = e.y;
            open = true;
        }
        if (e.removed) ++a1;
        else ++b1;
    }
    if (open) emit(a0, a1, b0, b1);
    return out;
}

// ---------------------------------------------------------------- diffs, read and laid out

namespace {

// A line of a text whose line starts are known, without its end.
std::string_view line_of(std::string_view text, const GitDiff& d, u32 i) {
    std::string_view l = text.substr(d.line_off[i], d.line_off[i + 1] - d.line_off[i]);
    if (l.ends_with('\n')) l.remove_suffix(1);
    return l;
}

// "@@ -12,3 +12,4 @@ ...": where a hunk is in either text, and how long it is.
bool hunk_head(std::string_view l, GitDiff::Hunk* h) {
    if (!l.starts_with("@@ -")) return false;
    const std::string s(l.substr(0, std::min<size_t>(l.size(), 80)));
    unsigned a = 0, b = 1, c = 0, d = 1;
    if (std::sscanf(s.c_str(), "@@ -%u,%u +%u,%u @@", &a, &b, &c, &d) == 4 || std::sscanf(s.c_str(), "@@ -%u +%u,%u @@", &a, &c, &d) == 3 ||
        std::sscanf(s.c_str(), "@@ -%u,%u +%u @@", &a, &b, &c) == 3 || std::sscanf(s.c_str(), "@@ -%u +%u @@", &a, &c) == 2) {
        h->old_start = a, h->old_count = b, h->new_start = c, h->new_count = d;
        return true;
    }
    return false;
}

// Whether two runs of lines [a0, a1) and [b0, b1) meet. An empty run is a
// place between two lines: it meets a run it is in or at either end of.
bool touches(u32 a0, u32 a1, u32 b0, u32 b1) {
    if (a0 == a1 && b0 == b1) return a0 == b0;
    if (a0 == a1) return b0 <= a0 && a0 <= b1;
    if (b0 == b1) return a0 <= b0 && b0 <= a1;
    return a0 < b1 && b0 < a1;
}

// Whole lines, each with its end; and a number for each that is equal for
// equal lines (the end counts: a last line without one differs).
std::vector<std::string_view> whole_lines(std::string_view s) {
    std::vector<std::string_view> out;
    for (size_t at = 0; at < s.size();) {
        const size_t nl = s.find('\n', at);
        const size_t end = nl == std::string_view::npos ? s.size() : nl + 1;
        out.push_back(s.substr(at, end - at));
        at = end;
    }
    return out;
}
std::vector<u64> hashes_of(const std::vector<std::string_view>& lines) {
    std::vector<u64> out;
    out.reserve(lines.size());
    const std::hash<std::string_view> hash;
    for (const std::string_view l : lines) out.push_back(hash(l));
    return out;
}

// Where a run of changed lines starts in the old text: the runs before it
// moved everything along by what they added less what they took away.
struct Spans {
    u32 old0, old1, new0, new1;
};
std::vector<Spans> spans_of(const std::vector<GitHunk>& hunks) {
    std::vector<Spans> out;
    i64 shift = 0;
    for (const GitHunk& h : hunks) {
        const u32 o = static_cast<u32>(static_cast<i64>(h.line) - shift);
        out.push_back({o, o + h.removed, h.line, h.line + h.count});
        shift += static_cast<i64>(h.count) - static_cast<i64>(h.removed);
    }
    return out;
}

}  // namespace

int GitDiff::hunk_at(u32 line) const {
    auto it = std::upper_bound(hunks.begin(), hunks.end(), line, [](u32 l, const Hunk& h) { return l < h.first; });
    if (it == hunks.begin()) return -1;
    --it;
    return line < it->last ? static_cast<int>(it - hunks.begin()) : -1;
}

GitDiff parse_diff(std::string_view text) {
    GitDiff d;
    for (size_t at = 0; at < text.size();) {
        d.line_off.push_back(at);
        const size_t nl = text.find('\n', at);
        at = nl == std::string_view::npos ? text.size() : nl + 1;
    }
    d.line_off.push_back(text.size());
    const u32 n = d.lines();
    u32 head0 = 0, head1 = 0;
    bool have = false, first = false;
    for (u32 i = 0; i < n;) {
        const std::string_view l = line_of(text, d, i);
        if (l.starts_with("diff ")) {
            head0 = i;
            have = first = true;
            ++i;
            continue;
        }
        GitDiff::Hunk h;
        if (!have || !hunk_head(l, &h)) {
            ++i;
            continue;
        }
        if (first) head1 = i, first = false;
        h.first = i;
        h.head0 = head0;
        h.head1 = head1;
        // Its lines: as many as the heading says of each text.
        u32 olds = h.old_count, news = h.new_count, j = i + 1;
        while (j < n && (olds || news)) {
            const std::string_view b = line_of(text, d, j);
            const char c = b.empty() ? ' ' : b[0];
            if (c == ' ') {
                if (!olds || !news) break;
                --olds, --news;
            } else if (c == '-') {
                if (!olds) break;
                --olds;
            } else if (c == '+') {
                if (!news) break;
                --news;
            } else if (c != '\\') {
                break;
            }
            ++j;
        }
        while (j < n && line_of(text, d, j).starts_with("\\")) ++j;  // "\ No newline at end of file"
        h.last = j;
        d.hunks.push_back(h);
        i = j;
    }
    return d;
}

std::string diff_patch(std::string_view text, const GitDiff& diff, std::span<const u8> chosen, bool reverse) {
    std::string out;
    u32 file = ~0u;
    i64 shift = 0;  // in this file: what the hunks put in the patch so far added, less what they took away
    auto picked = [&](u32 i) { return i < chosen.size() && chosen[i]; };
    for (const GitDiff::Hunk& h : diff.hunks) {
        bool any = false;
        for (u32 i = h.first + 1; i < h.last && !any; ++i) {
            const std::string_view l = line_of(text, diff, i);
            any = !l.empty() && (l[0] == '+' || l[0] == '-') && picked(i);
        }
        if (!any) continue;
        if (h.head0 != file) {
            file = h.head0;
            shift = 0;
            for (u32 i = h.head0; i < h.head1; ++i) out.append(line_of(text, diff, i)).push_back('\n');
        }
        std::string body;
        u32 olds = 0, news = 0;
        bool kept = false;  // the line before was kept (a "\ No newline" goes with it)
        for (u32 i = h.first + 1; i < h.last; ++i) {
            const std::string_view l = line_of(text, diff, i);
            const char c = l.empty() ? ' ' : l[0];
            const std::string_view rest = l.empty() ? l : l.substr(1);
            if (c == '\\') {
                if (kept) body.append(l).push_back('\n');
                continue;
            }
            kept = true;
            if ((c == '+' || c == '-') && !picked(i)) {
                if ((c == '+') != reverse) {  // not there before (or, backwards, after): left out
                    kept = false;
                    continue;
                }
                body.append(" ").append(rest).push_back('\n');  // there both before and after: context
                ++olds, ++news;
                continue;
            }
            if (c == '+') ++news;
            else if (c == '-') ++olds;
            else ++olds, ++news;
            body.append(1, c).append(rest).push_back('\n');
        }
        // One side is as git printed it; the other moved by the hunks before
        // it. (A run of no lines is numbered by the line before it.)
        u32 os = h.old_start, ns = h.new_start;
        if (!reverse) {
            const i64 before = (h.old_count ? h.old_start - 1 : h.old_start) + shift;
            ns = static_cast<u32>(std::max<i64>(0, news ? before + 1 : before));
        } else {
            const i64 before = (h.new_count ? h.new_start - 1 : h.new_start) - shift;
            os = static_cast<u32>(std::max<i64>(0, olds ? before + 1 : before));
        }
        shift += static_cast<i64>(news) - static_cast<i64>(olds);
        out += "@@ -" + std::to_string(os) + "," + std::to_string(olds) + " +" + std::to_string(ns) + "," + std::to_string(news) + " @@\n";
        out += body;
    }
    return out;
}

namespace {

// How a line of a diff is coloured: in a hunk, by its first character;
// outside one, git's headings (and a commit's) are muted.
u8 diff_style(std::string_view l, bool in_hunk) {
    if (l.starts_with("@@")) return 3;
    if (in_hunk) return l.starts_with("+") ? 1 : l.starts_with("-") ? 2 : l.starts_with("\\") ? 4 : 0;
    for (const char* head : {"diff ", "index ", "--- ", "+++ ", "commit ", "Author:", "Date:", "Merge:", "new file", "deleted file", "rename ", "similarity ",
                             "old mode", "new mode", "Binary files"})
        if (l.starts_with(head)) return 4;
    return 0;
}

// A line as cells: its characters (bytes [b0, b1) of `text`), each one or
// two columns wide, tabs made spaces and other control characters too.
struct Cells {
    std::string text;
    struct Ch {
        u32 b0, b1;
        u8 w;
        bool changed;
    };
    std::vector<Ch> ch;
    void set(std::string_view s, u32 tab_width) {
        text.clear();
        ch.clear();
        if (s.ends_with('\r')) s.remove_suffix(1);
        u32 col = 0;
        const u8* p = reinterpret_cast<const u8*>(s.data());
        for (size_t i = 0; i < s.size();) {
            const Decoded d = decode_utf8(p + i, s.size() - i);
            const u32 b0 = static_cast<u32>(text.size());
            if (d.cp == '\t') {
                const u32 n = tab_width - col % std::max<u32>(tab_width, 1);
                for (u32 k = 0; k < n; ++k) ch.push_back({b0 + k, b0 + k + 1, 1, false}), text.push_back(' ');
                col += n;
            } else if (d.cp < 0x20 || d.cp == 0x7F) {
                ch.push_back({b0, b0 + 1, 1, false});
                text.push_back(' ');
                ++col;
            } else {
                const u32 w = cp_width(d.cp);
                text.append(s.substr(i, d.len));
                if (w == 0 && !ch.empty()) ch.back().b1 = static_cast<u32>(text.size());  // (a mark: with the character before it)
                else ch.push_back({b0, static_cast<u32>(text.size()), static_cast<u8>(w ? w : 1), false}), col += w ? w : 1;
            }
            i += d.len;
        }
    }
};

// What changed within a line that became another: the words, spaces and
// signs of each are compared. False when so little stayed the same that it
// is clearer to call the whole line changed.
bool mark_changes(Cells& a, Cells& b) {
    auto words = [](const Cells& c, std::vector<u32>* starts, std::vector<u64>* hashes) {
        const std::hash<std::string_view> hash;
        for (u32 i = 0; i < c.ch.size();) {
            const u8 first = static_cast<u8>(c.text[c.ch[i].b0]);
            auto kind = [](u8 x) { return std::isalnum(x) || x == '_' || x >= 0x80 ? 0 : x == ' ' ? 1 : 2; };
            u32 j = i + 1;
            if (kind(first) != 2)
                while (j < c.ch.size() && kind(static_cast<u8>(c.text[c.ch[j].b0])) == kind(first)) ++j;
            starts->push_back(i);
            hashes->push_back(hash(std::string_view(c.text).substr(c.ch[i].b0, c.ch[j - 1].b1 - c.ch[i].b0)));
            i = j;
        }
        starts->push_back(static_cast<u32>(c.ch.size()));
    };
    std::vector<u32> sa, sb;
    std::vector<u64> ha, hb;
    words(a, &sa, &ha);
    words(b, &sb, &hb);
    if (ha.size() > 4000 || hb.size() > 4000) return false;
    const std::vector<GitHunk> hunks = diff_lines(ha, hb);
    for (const Spans& s : spans_of(hunks)) {
        for (u32 w = s.old0; w < s.old1; ++w)
            for (u32 k = sa[w]; k < sa[w + 1]; ++k) a.ch[k].changed = true;
        for (u32 w = s.new0; w < s.new1; ++w)
            for (u32 k = sb[w]; k < sb[w + 1]; ++k) b.ch[k].changed = true;
    }
    // What is left the same, not counting spaces, against what there is.
    u32 same = 0, all = 0;
    for (const Cells* c : {&a, &b})
        for (const Cells::Ch& x : c->ch)
            if (c->text[x.b0] != ' ') ++all, same += !x.changed;
    return same * 3 >= all;
}

}  // namespace

void diff_view(std::string_view text, const GitDiff& diff, u32 cols, u32 tab_width, GitDiffView* out) {
    out->text.clear();
    out->off.clear();
    out->style.clear();
    out->left.clear();
    out->right.clear();
    out->cols = cols;
    std::string& s = out->text;
    auto mark = [&](u8 st) {
        const u32 at = static_cast<u32>(s.size());
        if (!out->style.empty() && out->style.back() == st) return;
        if (!out->off.empty() && out->off.back() == at) {
            out->style.back() = st;
            if (out->style.size() >= 2 && out->style[out->style.size() - 2] == st) out->off.pop_back(), out->style.pop_back();
            return;
        }
        out->off.push_back(at);
        out->style.push_back(st);
    };
    const u32 n = diff.lines();
    if (cols == 0) {  // one column: the diff as it is, coloured line by line
        s.assign(text);
        size_t h = 0;
        for (u32 i = 0; i < n; ++i) {
            while (h < diff.hunks.size() && diff.hunks[h].last <= i) ++h;
            const bool in = h < diff.hunks.size() && diff.hunks[h].first < i;
            const u32 at = static_cast<u32>(diff.line_off[i]);
            const u8 st = diff_style(line_of(text, diff, i), in);
            if (out->style.empty() || out->style.back() != st) out->off.push_back(at), out->style.push_back(st);
        }
        return;
    }

    // Two columns: the old text's line numbers and lines on the left, the
    // new text's on the right, each column as wide as half the room, and
    // lines too long for it carried on in the rows below.
    u32 most = 1;
    for (const GitDiff::Hunk& h : diff.hunks) most = std::max({most, h.old_start + h.old_count, h.new_start + h.new_count});
    u32 numw = 1;
    for (u32 k = most; k >= 10; k /= 10) ++numw;
    numw = std::max<u32>(numw, 3);
    const u32 w = std::max<u32>(20, cols > 2 * numw + 5 ? (cols - 2 * numw - 5) / 2 : 0);
    auto full = [&](u32 i, u8 st) {  // a line across the whole width
        mark(st);
        s.append(line_of(text, diff, i)).push_back('\n');
        out->left.push_back(i);
        out->right.push_back(i);
    };
    auto number = [&](u32 k, bool show) {
        mark(4);
        std::string d = show ? std::to_string(k) : std::string();
        s.append(numw > d.size() ? numw - d.size() : 0, ' ').append(d).push_back(' ');
    };
    Cells a, b;
    // A line of each text beside each other (either may be missing): `ai`
    // and `bi` their lines in the diff, `ak` and `bk` their numbers; `sa`
    // and `sb` their colours, and those of what stayed the same within them.
    auto beside = [&](u32 ai, u32 bi, u32 ak, u32 bk, u8 sa, u8 sb, u8 same_a, u8 same_b) {
        const bool has_a = ai != ~0u, has_b = bi != ~0u;
        size_t ka = 0, kb = 0;  // the next character of each
        bool first = true;
        while (first || ka < a.ch.size() || kb < b.ch.size()) {
            number(ak, has_a && first);
            u32 used = 0;
            if (has_a)
                for (; ka < a.ch.size() && used + a.ch[ka].w <= w; ++ka) {
                    mark(a.ch[ka].changed ? sa : same_a);
                    s.append(std::string_view(a.text).substr(a.ch[ka].b0, a.ch[ka].b1 - a.ch[ka].b0));
                    used += a.ch[ka].w;
                }
            mark(0);
            s.append(w - used + 1, ' ');
            mark(4);
            s.append("\xE2\x94\x82 ");  // a line down between the two
            number(bk, has_b && first);
            used = 0;
            if (has_b)
                for (; kb < b.ch.size() && used + b.ch[kb].w <= w; ++kb) {
                    mark(b.ch[kb].changed ? sb : same_b);
                    s.append(std::string_view(b.text).substr(b.ch[kb].b0, b.ch[kb].b1 - b.ch[kb].b0));
                    used += b.ch[kb].w;
                }
            mark(0);
            s.push_back('\n');
            out->left.push_back(ai);
            out->right.push_back(bi);
            first = false;
            if (!has_a) ka = a.ch.size();
            if (!has_b) kb = b.ch.size();
        }
    };
    auto body = [&](u32 i) { return line_of(text, diff, i).substr(1); };
    size_t h = 0;
    for (u32 i = 0; i < n;) {
        if (h >= diff.hunks.size() || i < diff.hunks[h].first) {
            full(i, diff_style(line_of(text, diff, i), false));
            ++i;
            continue;
        }
        const GitDiff::Hunk& hk = diff.hunks[h++];
        full(i, 3);
        u32 ok = hk.old_start, nk = hk.new_start;
        std::vector<u32> minus, plus;
        // Lines taken away and the lines put in their place, paired off.
        auto flush = [&] {
            for (size_t k = 0; k < std::max(minus.size(), plus.size()); ++k) {
                const u32 ai = k < minus.size() ? minus[k] : ~0u, bi = k < plus.size() ? plus[k] : ~0u;
                a.set(ai != ~0u ? body(ai) : std::string_view{}, tab_width);
                b.set(bi != ~0u ? body(bi) : std::string_view{}, tab_width);
                const bool within = ai != ~0u && bi != ~0u && mark_changes(a, b);
                beside(ai, bi, ok, nk, 2, 1, within ? 4 : 2, within ? 4 : 1);
                if (ai != ~0u) ++ok;
                if (bi != ~0u) ++nk;
            }
            minus.clear();
            plus.clear();
        };
        for (i = hk.first + 1; i < hk.last; ++i) {
            const std::string_view l = line_of(text, diff, i);
            const char c = l.empty() ? ' ' : l[0];
            if (c == '-') {
                if (!plus.empty()) flush();
                minus.push_back(i);
            } else if (c == '+') {
                plus.push_back(i);
            } else if (c != '\\') {
                flush();
                a.set(l.empty() ? l : l.substr(1), tab_width);
                b.ch = a.ch, b.text = a.text;
                beside(i, i, ok++, nk++, 0, 0, 0, 0);
            }
        }
        flush();
        i = hk.last;
    }
}

std::string stage_lines(std::string_view head, std::string_view index, std::string_view now, u32 line0, u32 line1, bool stage) {
    const std::vector<std::string_view> idx = whole_lines(index), cur = whole_lines(now);
    const std::vector<Spans> iw = spans_of(diff_lines(hashes_of(idx), hashes_of(cur)));
    std::string out;
    if (stage) {
        // The index's lines, but where they differ from the text now in a
        // run that meets the lines asked for, the text's.
        u32 i = 0;
        for (const Spans& sp : iw) {
            if (!touches(sp.new0, sp.new1, line0, line1)) continue;
            for (; i < sp.old0; ++i) out += idx[i];
            for (u32 k = sp.new0; k < sp.new1; ++k) out += cur[k];
            i = sp.old1;
        }
        for (; i < idx.size(); ++i) out += idx[i];
        return out;
    }
    // Where those lines are in the index (they are staged, so the same there),
    // and then the index's lines, but where they differ from the committed
    // text in a run that meets those, the committed text's.
    auto in_index = [&](u32 line) {
        i64 shift = 0;
        for (const Spans& sp : iw) {
            if (line < sp.new0) break;
            if (line < sp.new1) return sp.old0 + std::min(line - sp.new0, sp.old1 - sp.old0);
            shift = static_cast<i64>(sp.new1) - static_cast<i64>(sp.old1);
        }
        return static_cast<u32>(std::max<i64>(0, static_cast<i64>(line) - shift));
    };
    const u32 i0 = in_index(line0), i1 = std::max(i0, in_index(line1));
    const std::vector<std::string_view> old = whole_lines(head);
    u32 i = 0;
    for (const Spans& sp : spans_of(diff_lines(hashes_of(old), hashes_of(idx)))) {
        if (!touches(sp.new0, sp.new1, i0, i1)) continue;
        for (; i < sp.new0; ++i) out += idx[i];
        for (u32 k = sp.old0; k < sp.old1; ++k) out += old[k];
        i = sp.new1;
    }
    for (; i < idx.size(); ++i) out += idx[i];
    return out;
}

// ---------------------------------------------------------------- running git

#if !defined(_WIN32)
namespace {

struct Ran {
    int code = -1;
    std::string out, err;
};

// The git running now, to stop it: a process here, or one on another
// machine (its number there, negated, and the connection to that machine).
// A stop asked for while it is still starting is carried out once it has.
struct Child {
    std::atomic<int> pid{0};
    std::atomic<bool> stopping{false};
    std::mutex mu;
    std::shared_ptr<Remote> conn;
    void stop() {
        stopping = true;
        kill_now();
    }
    void started(int p) {
        pid = p;
        if (stopping) kill_now();
    }
    void kill_now() {
        const int p = pid;
        if (p > 0) {
            ::kill(p, SIGTERM);
        } else if (p < 0) {
            std::lock_guard lock(mu);
            if (conn) conn->kill(static_cast<u32>(-p), SIGTERM);  // (sent, not waited for)
        }
    }
};

// Feeds a program its input and collects its output and complaints, until
// it has closed both (the descriptors are non-blocking, and closed here).
void pump(int in, int out, int err, std::string_view input, Ran& r) {
    size_t sent = 0;
    bool in_open = true, out_open = true, err_open = true;
    if (input.empty()) close(in), in_open = false;
    char buf[65536];
    while (out_open || err_open) {
        pollfd fds[3] = {{in_open ? in : -1, POLLOUT, 0}, {out_open ? out : -1, POLLIN, 0}, {err_open ? err : -1, POLLIN, 0}};
        if (::poll(fds, 3, -1) < 0 && errno != EINTR) break;
        if (in_open && (fds[0].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const ssize_t n = write(in, input.data() + sent, input.size() - sent);
            if (n > 0) sent += static_cast<size_t>(n);
            if (sent >= input.size() || (n < 0 && errno != EAGAIN && errno != EINTR)) close(in), in_open = false;
        }
        auto drain = [&](int fd, std::string& into, bool& open) {
            for (;;) {
                const ssize_t n = read(fd, buf, sizeof buf);
                if (n > 0) {
                    into.append(buf, static_cast<size_t>(n));
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    close(fd);
                    open = false;
                    return;
                } else {
                    return;
                }
            }
        };
        if (out_open && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) drain(out, r.out, out_open);
        if (err_open && (fds[2].revents & (POLLIN | POLLERR | POLLHUP))) drain(err, r.err, err_open);
    }
    if (in_open) close(in);
    if (out_open) close(out);
    if (err_open) close(err);
}

// Git on another machine (root is "ssh://host/path"), run there by blit's agent.
Ran run_git_remote(Child& child, const std::string& root, const std::vector<std::string>& args, std::string_view input) {
    Ran r;
    std::string there;
    const std::shared_ptr<Remote> conn = remote_for(root, &there, kRemoteWaitMs);
    if (!conn) {
        r.err = remote_error(root);
        return r;
    }
    std::vector<std::string> words = {"git", "--no-pager", "-c", "core.quotepath=off", "-c", "color.ui=false", "-C", there};
    words.insert(words.end(), args.begin(), args.end());
    RemoteProc p;
    if (!conn->spawn(words, there, {"GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", "GIT_EDITOR=true", "GIT_PAGER=cat"}, &p, &r.err)) return r;
    {
        std::lock_guard lock(child.mu);
        child.conn = conn;
    }
    child.started(-static_cast<int>(p.id));
    for (const int fd : {p.in, p.out, p.err}) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    pump(p.in, p.out, p.err, input, r);
    r.code = conn->wait(p.id);
    child.pid = 0;
    if (r.code < 0) {
        r.code = 128;
        if (r.err.empty()) r.err = remote_error(root);
    }
    return r;
}

// Runs git and waits for it. Its output and its complaints are collected
// together as they come (one at a time would let the other's pipe fill up).
Ran run_git(Child& child, const std::string& program, const std::string& root, const std::vector<std::string>& args,
            std::string_view input) {
    if (is_remote(root)) return run_git_remote(child, root, args, input);
    Ran r;
    int in[2], out[2], err[2];
    if (pipe(in) != 0) return r;
    if (pipe(out) != 0) {
        close(in[0]), close(in[1]);
        return r;
    }
    if (pipe(err) != 0) {
        close(in[0]), close(in[1]), close(out[0]), close(out[1]);
        return r;
    }
    for (const int fd : {in[1], out[0], err[0]}) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    for (const int fd : {in[0], out[1], err[1]}) posix_spawn_file_actions_addclose(&fa, fd);

    std::vector<std::string> words = {program, "--no-pager", "-c", "core.quotepath=off", "-c", "color.ui=false"};
    if (!root.empty()) {
        words.push_back("-C");
        words.push_back(root);
    }
    words.insert(words.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& w : words) argv.push_back(w.data());
    argv.push_back(nullptr);
    // Never stop to ask at a terminal that is not there, never start an
    // editor, and leave the index's lock alone when only looking.
    static const char* const ours[] = {"GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", "GIT_EDITOR=true", "GIT_PAGER=cat"};
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view var(*e);
        bool replaced = false;
        for (const char* o : ours) replaced |= var.starts_with(std::string_view(o).substr(0, std::string_view(o).find('=') + 1));
        if (!replaced) env.emplace_back(var);
    }
    for (const char* o : ours) env.emplace_back(o);
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    // Nothing else this process has open goes into git: not the pipes of
    // another program being started at the same time on another thread
    // (whose reader would then wait for git to end, and git for it).
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, program.c_str(), &fa, &attr, argv.data(), envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]), close(out[1]), close(err[1]);
    if (rc != 0) {
        close(in[1]), close(out[0]), close(err[0]);
        r.err = std::string("cannot run git: ") + std::strerror(rc);
        return r;
    }
    child.started(pid);
    pump(in[1], out[0], err[0], input, r);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    child.pid = 0;
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    return r;
}

bool exists(const std::string& path) {
    if (is_remote(path)) {
        std::string there;
        RemoteStat st;
        const std::shared_ptr<Remote> r = remote_for(path, &there, kRemoteWaitMs);
        return r && r->stat(there, &st) && st.kind != 0;
    }
    struct stat sb;
    return ::stat(path.c_str(), &sb) == 0;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string joined(std::span<const Piece> pieces) {
    std::string s;
    size_t total = 0;
    for (const Piece& p : pieces) total += p.len;
    s.reserve(total);
    for (const Piece& p : pieces) s.append(p.p, p.len);
    return s;
}

}  // namespace
#endif

struct Git::Worker {
    struct Job {
        u32 kind = 0;
        std::vector<std::string> args;
        std::string label, input, path;
        u64 doc = 0, revision = 0;
        std::vector<Piece> pieces;
        std::shared_ptr<const std::string> source;  // kView: a diff to lay out again
        u32 cols = 0, tab = 4;                      // kView: how
        u32 line0 = 0, line1 = 0;                   // kStage: the lines
        bool stage = true;
    };
    // (kStage: staging lines of a document is an action, but its own job.)
    static constexpr u32 kStage = 100;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::vector<Result> done;
    std::function<void()> notify;
    std::string dir;       // where to look for a repository; changed by open()
    bool look = false;     // dir has changed since the repository was last looked for
    std::string log_ref;   // whose commits a status lists (empty: the branch checked out)
    std::string busy;
    bool working = false;  // a job is being done
    u64 working_doc = 0;   // and it is about this document
    // While a job is about a document: the git it runs for it (the committed
    // text, the blame), which forget() stops; when its text is being read,
    // which forget() waits for (a moment: it is read into memory, not
    // waited on); and whether forget() has let go of it.
    u64 git_doc = 0, reading_doc = 0;
    bool dropped = false;
    std::condition_variable cv_idle;
    bool stop = false;
    std::thread thread;
    Child child;
    std::atomic<bool> child_is_action{false};

    // The worker's own.
    std::string program, root, gitdir, head;
    // The committed text of files compared so far, as line hashes; a null
    // entry is a file with no committed version. And the same of what is
    // staged of them (read again whenever the status is).
    std::unordered_map<std::string, std::shared_ptr<std::vector<u64>>> bases, staged;

#if !defined(_WIN32)
    Ran git(const std::vector<std::string>& args, std::string_view input = {}, bool action = false) {
        child.stopping = false;
        child_is_action = action;
        Ran r = run_git(child, program, root, args, input);
        child_is_action = false;
        return r;
    }

    // Git run for a document's job, which forget() stops if the document goes.
    Ran git_for(u64 doc, const std::vector<std::string>& args, std::string_view input = {}) {
        {
            std::lock_guard lock(mu);
            if (dropped) return {};
            git_doc = doc;
            child.stopping = false;
        }
        Ran r = run_git(child, program, root, args, input);
        std::lock_guard lock(mu);
        git_doc = 0;
        return r;
    }
    bool let_go() {
        std::lock_guard lock(mu);
        return dropped;
    }
    // The job's document's text is about to be read: false if it has gone.
    bool reading(u64 doc) {
        std::lock_guard lock(mu);
        if (dropped) return false;
        reading_doc = doc;
        return true;
    }
    void done_reading() {
        {
            std::lock_guard lock(mu);
            reading_doc = 0;
        }
        cv_idle.notify_all();
    }

    // The path of a file inside the repository, or empty if it is not in it.
    std::string inside(const std::string& path) const {
        char real[PATH_MAX];
        std::string full = !is_remote(path) && realpath(path.c_str(), real) ? real : path;
        if (root.empty() || !full.starts_with(root + "/")) return {};
        return full.substr(root.size() + 1);
    }

    void find_repository(const std::string& where) {
        root.clear();
        gitdir.clear();
        bases.clear();
        staged.clear();
        std::string host, there;
        const bool remote = split_remote(where, &host, &there);  // (another machine's: its own git, found there)
        if (!remote && program.empty()) program = Lsp::find_program("git");
        if ((!remote && program.empty()) || where.empty()) return;
        root = where;  // (run from there)
        const Ran r = git({"rev-parse", "--show-toplevel", "--absolute-git-dir"});
        root.clear();
        if (r.code != 0) return;
        const size_t nl = r.out.find('\n');
        if (nl == std::string::npos) return;
        root = r.out.substr(0, nl);
        gitdir = trimmed(r.out.substr(nl + 1));
        if (remote) root = remote_path(host, root), gitdir = remote_path(host, gitdir);
    }

    void read_status(Result& res) {
        res.kind = kStatus;
        res.ok = true;
        if (root.empty()) return;
        // The branches and tags, the latest commits and the stashes are
        // asked for at the same time as the status, by programs of their
        // own (the status is the slow one in a big repository).
        std::string of;
        {
            std::lock_guard lock(mu);
            of = log_ref;
        }
        Child refs_child, log_child, stash_child;
        Ran refs, log, stashes;
        std::jthread refs_thread([&] {
            refs = run_git(refs_child, program, root, {"for-each-ref", "--format=%(HEAD)%09%(refname)", "refs/heads", "refs/remotes", "refs/tags"}, {});
        });
        std::jthread log_thread([&] {
            std::vector<std::string> args = {"log", "-n", "40", "--format=%h%x1f%an%x1f%ar%x1f%s"};
            if (!of.empty()) args.insert(args.end(), {of, "--"});
            log = run_git(log_child, program, root, args, {});
        });
        std::jthread stash_thread([&] { stashes = run_git(stash_child, program, root, {"stash", "list", "--format=%gd%x1f%cr%x1f%gs"}, {}); });
        const Ran st = git({"status", "--porcelain=v2", "--branch", "-z"});
        refs_thread.join();
        log_thread.join();
        stash_thread.join();
        staged.clear();  // (the index may have changed)
        if (st.code != 0) {
            res.ok = false;
            res.text = trimmed(st.err);
            return;
        }
        res.status = parse_status(st.out);
        res.status.root = root;
        res.status.merging = exists(gitdir + "/MERGE_HEAD");
        res.status.picking = exists(gitdir + "/CHERRY_PICK_HEAD");
        // (A rebase that has stopped has no branch checked out.)
        res.status.rebasing = res.status.branch.empty() && (exists(gitdir + "/rebase-merge") || exists(gitdir + "/rebase-apply"));
        if (res.status.head != head) {  // another commit is checked out: what files are compared with has changed
            head = res.status.head;
            bases.clear();
        }
        res.branches = parse_branches(refs.out, &res.tags);
        if (!of.empty() && log.code != 0) {  // (the branch has gone: the one checked out, then)
            {
                std::lock_guard lock(mu);
                if (log_ref == of) log_ref.clear();
            }
            of.clear();
            log = git({"log", "-n", "40", "--format=%h%x1f%an%x1f%ar%x1f%s"});
        }
        res.log_of = of;
        if ((!res.status.head.empty() || !of.empty()) && log.code == 0) res.log = parse_log(log.out);
        if (stashes.code == 0) res.stashes = parse_stashes(stashes.out);
    }

    // The text of a file in the index (":path") or a commit ("HEAD:path"),
    // as it would be in the working tree. False if it has none.
    bool blob(u64 doc, const std::string& spec, std::string* out) {
        Ran r = doc ? git_for(doc, {"cat-file", "--filters", spec}) : git({"cat-file", "--filters", spec});
        if (r.code != 0) return false;
        *out = std::move(r.out);
        return true;
    }

    // Lines of a document staged, or put back as they were committed.
    void stage_job(Job& job, Result& res) {
        res.kind = kAction;
        const std::string rel = inside(job.path);
        if (root.empty() || rel.empty()) {
            res.text = root.empty() ? "not in a git repository" : "not in the repository";
            return;
        }
        if (!reading(job.doc)) return;
        const std::string now = joined(job.pieces);
        done_reading();
        const Ran ls = git({"ls-files", "-s", "--", rel});
        std::string mode = "100644";
        if (ls.code == 0 && !ls.out.empty()) {
            mode = ls.out.substr(0, ls.out.find(' '));
            const size_t tab = ls.out.find('\t');
            if (tab != std::string::npos && tab >= 2 && ls.out[tab - 1] != '0') {
                res.text = rel + " is in conflict";
                return;
            }
        }
        std::string index, committed;
        blob(0, ":" + rel, &index);
        if (!job.stage) blob(0, "HEAD:" + rel, &committed);
        const std::string next = stage_lines(committed, index, now, job.line0, job.line1, job.stage);
        if (next == index) {
            res.text = job.stage ? "nothing there to stage" : "nothing there to unstage";
            return;
        }
        const Ran h = git({"hash-object", "-w", "--stdin", "--path=" + rel}, next, true);
        const std::string sha = trimmed(h.out);
        if (h.code != 0 || sha.empty()) {
            res.text = trimmed(h.err);
            return;
        }
        const Ran u = git({"update-index", "--add", "--cacheinfo", mode + "," + sha + "," + rel}, {}, true);
        res.ok = u.code == 0;
        if (!res.ok) res.text = trimmed(u.err);
    }

    void work(Job& job, std::vector<Result>& out) {
        Result res;
        res.kind = job.kind;
        res.doc = job.doc;
        res.revision = job.revision;
        res.label = job.label;
        switch (job.kind) {
            case kStatus: read_status(res); break;
            case kAction: {
                if (root.empty()) {
                    res.text = "not in a git repository";
                    break;
                }
                const Ran r = git(job.args, job.input, true);
                res.ok = r.code == 0;
                // Git says what it did, and what went wrong, on either stream.
                res.text = trimmed(res.ok ? (r.out.empty() ? r.err : r.out) : (r.err.empty() ? r.out : r.err));
                bases.clear();
                out.push_back(std::move(res));
                res = Result{};
                read_status(res);
                break;
            }
            case kHunks: {
                const std::string rel = inside(job.path);
                if (rel.empty()) break;
                auto it = bases.find(rel);
                if (it == bases.end()) {
                    const Ran r = git_for(job.doc, {"show", "HEAD:" + rel});
                    if (let_go()) return;  // (the document went, and its git was stopped: what it gave is not kept)
                    it = bases.emplace(rel, r.code == 0 ? std::make_shared<std::vector<u64>>(line_hashes(std::string_view(r.out))) : nullptr).first;
                }
                res.ok = true;
                res.tracked = it->second != nullptr;
                if (res.tracked) {
                    // And what of it is staged: a run of changed lines is,
                    // when the index has those lines as they are now.
                    auto in = staged.find(rel);
                    if (in == staged.end()) {
                        std::string text;
                        const bool known = blob(job.doc, ":" + rel, &text);
                        if (let_go()) return;
                        in = staged.emplace(rel, known ? std::make_shared<std::vector<u64>>(line_hashes(std::string_view(text))) : nullptr).first;
                    }
                    if (!reading(job.doc)) return;
                    const std::vector<u64> now = line_hashes(std::span<const Piece>(job.pieces));
                    done_reading();
                    res.hunks = diff_lines(*it->second, now);
                    if (in->second) {
                        const std::vector<GitHunk> unstaged = diff_lines(*in->second, now);
                        for (GitHunk& h : res.hunks) {
                            h.staged = true;
                            for (const GitHunk& u : unstaged) h.staged = h.staged && !touches(h.line, h.line + h.count, u.line, u.line + u.count);
                        }
                    }
                }
                break;
            }
            case kBlame: {
                const std::string rel = inside(job.path);
                if (rel.empty()) break;
                if (!reading(job.doc)) return;
                std::string text = joined(job.pieces);
                done_reading();
                const Ran r = git_for(job.doc, {"blame", "--porcelain", "--contents", "-", "--", rel}, text);
                if (let_go()) return;
                auto blame = std::make_shared<GitBlame>();
                res.ok = r.code == 0 && parse_blame(r.out, blame.get());
                if (res.ok) res.blame = std::move(blame);
                else res.text = trimmed(r.err);
                break;
            }
            case kText: {
                if (root.empty()) {
                    res.text = "not in a git repository";
                    break;
                }
                const Ran r = git(job.args);
                res.ok = r.code == 0;
                res.text = res.ok ? r.out : trimmed(r.err);
                break;
            }
            case kView: {
                std::shared_ptr<const std::string> source = job.source;
                if (!source) {
                    if (root.empty()) {
                        res.text = "not in a git repository";
                        break;
                    }
                    Ran r = git(job.args);
                    if (r.code != 0) {
                        res.text = trimmed(r.err);
                        break;
                    }
                    source = std::make_shared<const std::string>(std::move(r.out));
                }
                auto diff = std::make_shared<GitDiff>(parse_diff(*source));
                auto view = std::make_shared<GitDiffView>();
                diff_view(*source, *diff, job.cols, job.tab, view.get());
                res.ok = true;
                res.source = std::move(source);
                res.diff = std::move(diff);
                res.view = std::move(view);
                break;
            }
            case kStage: {
                stage_job(job, res);
                if (let_go()) return;
                bases.clear();
                out.push_back(std::move(res));
                res = Result{};
                read_status(res);
                break;
            }
            default: break;
        }
        out.push_back(std::move(res));
    }

    void loop() {
        thread_soon(Soon::kBackground);
        signal(SIGPIPE, SIG_IGN);  // a git that stops reading what it is fed must not take the editor with it
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mu);
                busy.clear();
                working = false;
                working_doc = 0;
                cv_idle.notify_all();
                cv.wait(lock, [&] { return stop || !jobs.empty(); });
                if (stop) return;
                job = std::move(jobs.front());
                jobs.pop_front();
                working = true;
                dropped = false;
                if (job.kind == kHunks || job.kind == kBlame || job.kind == kStage) working_doc = job.doc;
                if (job.kind == kAction || job.kind == kStage) busy = job.label;
                if (look) {
                    look = false;
                    const std::string where = dir;
                    lock.unlock();
                    find_repository(where);
                }
            }
            std::vector<Result> results;
            work(job, results);
            {
                std::lock_guard lock(mu);
                for (Result& r : results) done.push_back(std::move(r));
            }
            if (notify) notify();
        }
    }
#else
    void loop() {}
#endif

    void push(Job job) {
        {
            std::lock_guard lock(mu);
            // One status is as good as two, and only the latest look at a
            // document matters.
            for (Job& queued : jobs) {
                if (queued.kind != job.kind) continue;
                if (job.kind == kStatus) return;
                if ((job.kind == kHunks || job.kind == kBlame || job.kind == kView) && queued.doc == job.doc) {
                    queued = std::move(job);
                    return;
                }
            }
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }
};

Git::Git() : w_(std::make_shared<Worker>()) {
#if !defined(_WIN32)
    w_->thread = std::thread([w = w_.get()] { w->loop(); });
#endif
}

Git::~Git() {
    {
        std::lock_guard lock(w_->mu);
        w_->stop = true;
        w_->jobs.clear();
    }
    w_->cv.notify_one();
#if !defined(_WIN32)
    w_->child.stop();  // do not wait for a push to finish
#endif
    if (w_->thread.joinable()) w_->thread.join();
}

void Git::set_notify(std::function<void()> fn) {
    std::lock_guard lock(w_->mu);
    w_->notify = std::move(fn);
}

void Git::open(const std::string& dir) {
    {
        std::lock_guard lock(w_->mu);
        if (w_->dir == dir) return;
        w_->dir = dir;
        w_->look = true;
    }
    refresh();
}

void Git::refresh() {
    Worker::Job job;
    job.kind = kStatus;
    w_->push(std::move(job));
}

void Git::run(std::vector<std::string> args, std::string label, std::string input) {
    Worker::Job job;
    job.kind = kAction;
    job.args = std::move(args);
    job.label = std::move(label);
    job.input = std::move(input);
    w_->push(std::move(job));
}

void Git::cancel() {
#if !defined(_WIN32)
    if (w_->child_is_action) w_->child.stop();
#endif
}

void Git::hunks(u64 doc, u64 revision, const std::string& path, std::vector<Piece> pieces) {
    Worker::Job job;
    job.kind = kHunks;
    job.doc = doc;
    job.revision = revision;
    job.path = path;
    job.pieces = std::move(pieces);
    w_->push(std::move(job));
}

void Git::blame(u64 doc, u64 revision, const std::string& path, std::vector<Piece> pieces) {
    Worker::Job job;
    job.kind = kBlame;
    job.doc = doc;
    job.revision = revision;
    job.path = path;
    job.pieces = std::move(pieces);
    w_->push(std::move(job));
}

void Git::text(u64 doc, std::vector<std::string> args, std::string label) {
    Worker::Job job;
    job.kind = kText;
    job.doc = doc;
    job.args = std::move(args);
    job.label = std::move(label);
    w_->push(std::move(job));
}

void Git::view(u64 doc, std::vector<std::string> args, std::shared_ptr<const std::string> source, u32 cols, u32 tab_width) {
    Worker::Job job;
    job.kind = kView;
    job.doc = doc;
    job.args = std::move(args);
    job.source = std::move(source);
    job.cols = cols;
    job.tab = tab_width;
    w_->push(std::move(job));
}

void Git::stage(u64 doc, const std::string& path, std::vector<Piece> pieces, u32 line0, u32 line1, bool stage, std::string label) {
    Worker::Job job;
    job.kind = Worker::kStage;
    job.doc = doc;
    job.path = path;
    job.pieces = std::move(pieces);
    job.line0 = line0;
    job.line1 = line1;
    job.stage = stage;
    job.label = std::move(label);
    w_->push(std::move(job));
}

void Git::log_of(const std::string& branch) {
    {
        std::lock_guard lock(w_->mu);
        if (w_->log_ref == branch) return;
        w_->log_ref = branch;
    }
    refresh();
}

void Git::forget(u64 doc) {
    std::unique_lock lock(w_->mu);
    std::erase_if(w_->jobs, [doc](const Worker::Job& j) {
        return (j.kind == kHunks || j.kind == kBlame || j.kind == kView || j.kind == Worker::kStage) && j.doc == doc;
    });
    if (w_->working_doc != doc) return;
    // The job under way for it: its git is stopped rather than waited for
    // (a blame of a long file, or one on a slow connection, can take a
    // while), and the text is not read after this returns. Only a read of
    // the text going on now is waited for.
    w_->dropped = true;
#if !defined(_WIN32)
    if (w_->git_doc == doc) w_->child.stop();
#endif
    w_->cv_idle.wait(lock, [&] { return w_->reading_doc != doc; });
}

bool Git::working_on(u64 doc) const {
    std::lock_guard lock(w_->mu);
    return w_->git_doc == doc;
}

bool Git::idle() const {
    std::lock_guard lock(w_->mu);
    return w_->jobs.empty() && !w_->working;
}

std::vector<Git::Result> Git::poll() {
    std::lock_guard lock(w_->mu);
    return std::exchange(w_->done, {});
}

std::string Git::busy() const {
    std::lock_guard lock(w_->mu);
    return w_->busy;
}

}  // namespace blit
