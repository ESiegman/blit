#include "finder.hpp"

#if !defined(BLIT_AGENT)
#include "remote.hpp"  // (blit's, not its agent's: a folder on another machine)
#include "vim.hpp"     // (and the patterns its Vim reads; the agent is asked for plain text)
#endif

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include "qos.hpp"

namespace blit {

namespace {

// One line of a .gitignore.
struct Rule {
    std::string pat;
    bool negate = false;    // "!pattern": what matches is wanted after all
    bool dir_only = false;  // "pattern/": folders only
    bool anchored = false;  // has a slash in it: matched against the path from the .gitignore's folder, not the name
    // What most patterns are, told apart so that they need no pattern matching: a plain name, or "*" and a plain ending.
    enum : u8 { kGlob, kLiteral, kEnding } kind = kGlob;
};

// The rules of one .gitignore, and how much of a path (from the root) is
// the folder it is in.
struct RuleSet {
    std::vector<Rule> rules;
    size_t base = 0;
};

void parse_rule(std::string_view line, std::vector<Rule>* out) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    if (line.empty() || line[0] == '#') return;
    Rule r;
    if (line[0] == '!') {
        r.negate = true;
        line.remove_prefix(1);
    } else if (line[0] == '\\' && line.size() > 1 && (line[1] == '#' || line[1] == '!')) {
        line.remove_prefix(1);
    }
    if (!line.empty() && line.back() == '/') {
        r.dir_only = true;
        line.remove_suffix(1);
    }
    if (line.empty()) return;
    if (line[0] == '/') {
        r.anchored = true;
        line.remove_prefix(1);
    } else if (line.find('/') != std::string_view::npos) {
        r.anchored = true;
    }
    r.pat = std::string(line);
    const bool marks = r.pat.find_first_of("*?[\\") != std::string::npos;
    if (!marks) r.kind = Rule::kLiteral;
    else if (r.pat[0] == '*' && r.pat.find_first_of("*?[\\/", 1) == std::string::npos) r.kind = Rule::kEnding;
    out->push_back(std::move(r));
}

void parse_rules(std::string_view text, std::vector<Rule>* out) {
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        parse_rule(text.substr(0, nl), out);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    }
}

// Left out whatever a .gitignore says: version control's own folders, what
// package managers and build systems make, and compiled things.
constexpr const char* kAlwaysOut =
    ".git/\n.hg/\n.svn/\n.DS_Store\nnode_modules/\n__pycache__/\n.venv/\nvenv/\ntarget/\nbuild/\nbuild-*/\ncmake-build-*/\n.cache/\n"
    "*.o\n*.obj\n*.a\n*.so\n*.dylib\n*.dll\n*.exe\n*.class\n*.pyc\n*.pdb\n*.blit-tmp\n";

// Is this path (from the root; `name` is its last part) left out? The last
// rule that matches decides, as in git.
bool left_out(std::string_view rel, std::string_view name, bool dir, std::span<const RuleSet* const> sets) {
    bool out = false;
    for (const RuleSet* set : sets)
        for (const RuleSet& s = *set; const Rule& r : s.rules) {
            if (r.dir_only && !dir) continue;
            const std::string_view subject = r.anchored ? rel.substr(std::min(s.base, rel.size())) : name;
            const bool hit = r.kind == Rule::kLiteral  ? subject == r.pat
                             : r.kind == Rule::kEnding ? subject.size() >= r.pat.size() - 1 && subject.ends_with(std::string_view(r.pat).substr(1)) &&
                                                             subject.find('/') == std::string_view::npos
                                                       : Finder::glob(r.pat, subject);
            if (hit) out = !r.negate;
        }
    return out;
}

// Reads a file whole, if it is no bigger than max_bytes. With text_only, a
// file with a zero byte in its first 8 KB is taken for one that is not text,
// and left there unread past that (false).
bool read_file(const std::string& path, u64 max_bytes, std::string* into, bool text_only = false) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && static_cast<u64>(st.st_size) <= max_bytes;
    if (ok) {
        const size_t size = static_cast<size_t>(st.st_size);
        size_t got = 0;
        into->resize_and_overwrite(size, [&](char* p, size_t n) {
            constexpr size_t kFirst = 8192;
            while (got < n) {
                const ssize_t k = ::read(fd, p + got, (text_only && got < kFirst ? std::min(n, kFirst) : n) - got);
                if (k <= 0) break;
                got += static_cast<size_t>(k);
                if (text_only && got <= kFirst && (got == n || got == kFirst) && std::memchr(p, 0, got)) {
                    got = 0;  // not text
                    break;
                }
            }
            return got;
        });
        ok = got > 0;
    }
    ::close(fd);
    return ok;
}

// The places in a list of the files with these paths, in order. (One pass
// over the list, comparing only the paths as long as one of these.)
std::vector<u32> files_named(const FileIndex& ix, const std::vector<std::string>& paths) {
    std::vector<u32> out;
    u64 lengths = 0;
    for (const std::string& p : paths) lengths |= 1ull << (p.size() & 63);
    for (size_t i = 0; i < ix.size() && out.size() < paths.size(); ++i) {
        const std::string_view p = ix.path(i);
        if (!((lengths >> (p.size() & 63)) & 1)) continue;
        if (std::ranges::find(paths, p) != paths.end()) out.push_back(static_cast<u32>(i));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- patterns

// .gitignore's patterns: "*" and "?" stop at a slash, "**" does not,
// "[a-z]" is one of a set, a backslash takes the next character as it is.
bool Finder::glob(std::string_view p, std::string_view t) {
    size_t pi = 0, ti = 0;
    while (pi < p.size()) {
        char c = p[pi];
        if (c == '*') {
            if (pi + 1 < p.size() && p[pi + 1] == '*') {  // any number of folders
                size_t rest = pi + 2;
                if (rest < p.size() && p[rest] == '/') ++rest;
                if (rest >= p.size()) return true;
                for (size_t k = ti; k <= t.size(); ++k)
                    if ((k == ti || t[k - 1] == '/') && glob(p.substr(rest), t.substr(k))) return true;
                return false;
            }
            for (size_t k = ti;; ++k) {  // anything but a slash
                if (glob(p.substr(pi + 1), t.substr(k))) return true;
                if (k >= t.size() || t[k] == '/') return false;
            }
        }
        if (ti >= t.size()) return false;
        if (c == '?') {
            if (t[ti] == '/') return false;
        } else if (c == '[' && p.find(']', pi + 2) != std::string_view::npos) {
            size_t k = pi + 1;
            const bool negate = p[k] == '!' || p[k] == '^';
            if (negate) ++k;
            bool in = false;
            for (bool first = true; k < p.size() && (first || p[k] != ']'); first = false) {
                const char lo = p[k];
                char hi = lo;
                if (k + 2 < p.size() && p[k + 1] == '-' && p[k + 2] != ']') hi = p[k + 2], k += 2;
                in = in || (t[ti] >= lo && t[ti] <= hi);
                ++k;
            }
            if (in == negate || t[ti] == '/') return false;
            pi = k;  // (the closing bracket; stepped over below)
        } else {
            if (c == '\\' && pi + 1 < p.size()) c = p[++pi];
            if (c != t[ti]) return false;
        }
        ++pi;
        ++ti;
    }
    return ti == t.size();
}

// ---------------------------------------------------------------- the list of files

// The folders are read on several threads at once, each taking the next
// folder waiting to be read; what each holds is kept in its own place in a
// tree of them, which is gone through in order at the end. (The list is the
// same as a reading of one folder after another would make.)
std::shared_ptr<FileIndex> Finder::walk(const std::string& root, const FinderOptions& o, const std::atomic<u64>* epoch, u64 mine) {
#if !defined(BLIT_AGENT)
    if (is_remote(root)) {  // another machine's: its agent walks it (and keeps the list, to search it)
        auto ix = std::make_shared<FileIndex>();
        std::string there;
        u32 id = 0;
        const std::shared_ptr<Remote> r = remote_for(root, &there, kRemoteWaitMs);
        while (there.size() > 1 && there.back() == '/') there.pop_back();
        if (r && r->walk(there, o, ix.get(), &id)) {
            ix->remote_walk = id;
        } else {
            ix->root = root;
            ix->error = remote_error(root);  // (said by a search of it, not taken for a folder with nothing in it)
            if (!ix->error.empty()) ix->error[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ix->error[0])));
        }
        return ix;
    }
#endif
    auto ix = std::make_shared<FileIndex>();
    ix->root = root;
    while (ix->root.size() > 1 && ix->root.back() == '/') ix->root.pop_back();
    // The rules in force in a folder: those of its own .gitignore, and up
    // from there those of the folders it is in.
    struct Rules {
        RuleSet set;
        std::shared_ptr<const Rules> up;
    };
    auto top = std::make_shared<Rules>();
    parse_rules(kAlwaysOut, &top->set.rules);
    for (const std::string& line : o.ignore) parse_rule(line, &top->set.rules);

    struct Folder {
        std::string rel;  // from the root, with its slash ("" for the root itself)
        std::shared_ptr<const Rules> rules;
        int depth = 0;
        // What is in it, in order: a file (its path is names[ends[i-1], ends[i])) or, with kFolder, a folder of kids.
        std::vector<u32> order;
        std::string names;
        std::vector<u32> ends;
        std::vector<std::unique_ptr<Folder>> kids;
    };
    constexpr u32 kFolder = 1u << 31;
    std::mutex m;
    std::condition_variable cv;
    std::vector<Folder*> waiting;
    size_t pending = 1;  // folders waiting or being read
    std::atomic<u64> found{0};
    std::atomic<bool> too_many{false};
    Folder top_folder;
    top_folder.rules = top;
    waiting.push_back(&top_folder);

    struct Entry {
        std::string name;
        bool dir;
    };
    auto read_folder = [&](Folder& f, std::string& scratch, std::vector<Entry>& entries, std::vector<const RuleSet*>& chain) {
        if ((epoch && epoch->load(std::memory_order_relaxed) != mine) || too_many.load(std::memory_order_relaxed)) return;
        const std::string here = ix->root + "/" + f.rel;
        DIR* d = ::opendir(here.c_str());
        if (!d) return;
        entries.clear();
        bool has_rules = false;
        while (const dirent* e = ::readdir(d)) {
            const char* name = e->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            if (o.gitignore && std::strcmp(name, ".gitignore") == 0) has_rules = true;
            if (!o.hidden && name[0] == '.') continue;
            bool dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
                struct stat st;
                if (::stat((here + name).c_str(), &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    if (e->d_type == DT_LNK) continue;  // a link to a folder is not followed: it may lead round in a circle
                    dir = true;
                } else if (!S_ISREG(st.st_mode)) {
                    continue;
                }
            } else if (!dir && e->d_type != DT_REG) {
                continue;
            }
            entries.push_back({name, dir});
        }
        ::closedir(d);
        std::shared_ptr<const Rules> rules = f.rules;
        if (has_rules && read_file(here + ".gitignore", 1 << 20, &scratch)) {
            auto mine_too = std::make_shared<Rules>();
            mine_too->set.base = f.rel.size();
            parse_rules(scratch, &mine_too->set.rules);
            mine_too->up = rules;
            rules = std::move(mine_too);
        }
        chain.clear();
        for (const Rules* r = rules.get(); r; r = r->up.get()) chain.push_back(&r->set);
        std::reverse(chain.begin(), chain.end());  // (the top folder's first: the last rule that matches decides)
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
        std::string rel = f.rel;
        for (const Entry& e : entries) {
            rel.resize(f.rel.size());
            rel += e.name;
            if (left_out(rel, e.name, e.dir, chain)) continue;
            if (e.dir) {
                if (f.depth >= 64) continue;
                auto kid = std::make_unique<Folder>();
                kid->rel = rel + '/';
                kid->rules = rules;
                kid->depth = f.depth + 1;
                f.order.push_back(kFolder | static_cast<u32>(f.kids.size()));
                f.kids.push_back(std::move(kid));
            } else {
                if (found.fetch_add(1, std::memory_order_relaxed) >= o.max_files) {
                    too_many.store(true);
                    break;
                }
                f.names += rel;
                f.order.push_back(static_cast<u32>(f.ends.size()));
                f.ends.push_back(static_cast<u32>(f.names.size()));
            }
        }
        if (f.kids.empty()) return;
        {
            std::lock_guard lock(m);
            // (Last in, first out: the reading goes down before it goes across, and fewer folders wait at once.)
            for (size_t k = f.kids.size(); k-- > 0;) waiting.push_back(f.kids[k].get());
            pending += f.kids.size();
        }
        cv.notify_all();
    };
    auto work = [&] {
        thread_soon(Soon::kWaitedFor);
        std::string scratch;
        std::vector<Entry> entries;
        std::vector<const RuleSet*> chain;
        std::unique_lock lock(m);
        for (;;) {
            cv.wait(lock, [&] { return !waiting.empty() || pending == 0; });
            if (waiting.empty()) return;
            Folder* f = waiting.back();
            waiting.pop_back();
            lock.unlock();
            read_folder(*f, scratch, entries, chain);
            lock.lock();
            if (--pending == 0) cv.notify_all();
        }
    };
    {
        std::vector<std::jthread> helpers;
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        for (unsigned t = 1; t < threads; ++t) helpers.emplace_back(work);
        work();
    }
    ix->truncated = too_many.load();
    auto flatten = [&](auto&& self, const Folder& f) -> bool {
        for (const u32 item : f.order) {
            if (item & kFolder) {
                if (!self(self, *f.kids[item & ~kFolder])) return false;
                continue;
            }
            if (ix->offs.size() >= o.max_files) {
                ix->truncated = true;
                return false;
            }
            const u32 a = item == 0 ? 0 : f.ends[item - 1];
            ix->offs.push_back(static_cast<u32>(ix->names.size()));
            ix->names.append(f.names, a, f.ends[item] - a);
        }
        return true;
    };
    flatten(flatten, top_folder);
    ix->offs.push_back(static_cast<u32>(ix->names.size()));
    return ix;
}

// ---------------------------------------------------------------- searching

void Finder::rank_names(const FileIndex& ix, std::span<const FuzzyTerm> terms, FindResults* out, const std::atomic<u64>* epoch, u64 mine, const std::vector<u32>* among,
                        std::vector<u32>* matched, std::span<const u32> preferred) {
    out->hits.clear();
    out->matched = 0;
    if (matched) matched->clear();
    const size_t n = ix.size();
    if (terms.empty()) {  // nothing typed: the files as they come
        for (size_t i = 0; i < n && i < kMaxNames; ++i) out->hits.push_back(FindHit{static_cast<u32>(i), 0, 0, 0, {}});
        out->matched = n;
        return;
    }
    struct Scored {
        u32 file;
        int score;
    };
    std::vector<Scored> all;
    const size_t looked = among ? among->size() : n;
    for (size_t k = 0; k < looked; ++k) {
        if ((k & 4095) == 0 && epoch && epoch->load(std::memory_order_relaxed) != mine) return;
        const u32 i = among ? (*among)[k] : static_cast<u32>(k);
        const int s = fuzzy_path_score(terms, ix.path(i));
        if (s >= 0) all.push_back({i, s});
    }
    out->matched = all.size();
    if (matched)
        for (const Scored& x : all) matched->push_back(x.file);
    // The best first; of equals, one of the preferred, then the one earlier in the list.
    const size_t keep = std::min(all.size(), kMaxNames);
    std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(keep), all.end(), [&](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        if (!preferred.empty()) {
            const bool pa = std::ranges::binary_search(preferred, a.file), pb = std::ranges::binary_search(preferred, b.file);
            if (pa != pb) return pa;
        }
        return a.file < b.file;
    });
    for (size_t i = 0; i < keep; ++i) out->hits.push_back(FindHit{all[i].file, 0, 0, all[i].score, {}});
}

bool Finder::grep(std::string_view text, std::span<const FuzzyTerm> terms, u32 file, size_t room, std::vector<FindHit>* out) {
    if (terms.empty()) return true;
    if (std::memchr(text.data(), 0, std::min<size_t>(text.size(), 8192))) return false;  // not text
    // The longest word leads: it is the rarest, as a rule, and the others
    // are only looked for on the lines it is on.
    size_t lead = 0;
    for (size_t i = 1; i < terms.size(); ++i)
        if (terms[i].text.size() > terms[lead].text.size()) lead = i;
    size_t pos = 0, counted_to = 0;
    u32 line_no = 0;
    while (room > 0) {
        const size_t hit = find_term(text, terms[lead], pos);
        if (hit == std::string_view::npos) break;
        size_t ls = text.rfind('\n', hit);
        ls = ls == std::string_view::npos ? 0 : ls + 1;
        size_t le = text.find('\n', hit);
        if (le == std::string_view::npos) le = text.size();
        line_no += static_cast<u32>(std::count(text.begin() + static_cast<std::ptrdiff_t>(counted_to), text.begin() + static_cast<std::ptrdiff_t>(ls), '\n'));
        counted_to = ls;
        std::string_view line = text.substr(ls, le - ls);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        size_t col = hit - ls;
        bool all = true;
        for (size_t i = 0; i < terms.size() && all; ++i) {
            if (i == lead) continue;
            const size_t at = find_term(line, terms[i]);
            all = at != std::string_view::npos;
            if (all) col = std::min(col, at);
        }
        if (all) {
            FindHit h;
            h.file = file;
            h.line = line_no;
            h.col = static_cast<u32>(col);
            // (A very long line is kept from a little before the match on.)
            size_t from = col > 200 ? col - 80 : 0;
            while (from > 0 && (static_cast<u8>(line[from]) & 0xC0) == 0x80) --from;
            h.text = std::string(line.substr(from, 600));
            out->push_back(std::move(h));
            --room;
        }
        pos = le + 1;
        if (pos >= text.size()) break;
    }
    return true;
}

#if !defined(BLIT_AGENT)
namespace {

// The lines a phrase or pattern is on, each with how many matches start
// there (all of them are found, one after another, as :s finds them).
bool grep_lines(std::string_view text, const Vim::Pattern& pat, u32 file, size_t room, std::vector<FindHit>* out) {
    if (std::memchr(text.data(), 0, std::min<size_t>(text.size(), 8192))) return false;  // not text
    size_t counted_to = 0, line_end = 0;
    u32 line_no = 0;
    bool any = false;
    Vim::each_whole(text, pat, 0, text.size() + 1, false, [&](const RegexMatch& m) {
        const size_t at = static_cast<size_t>(std::min<u64>(m.start, text.size()));
        if (any && at <= line_end) {  // (on the line of the last one: up to its "\n")
            ++out->back().count;
            return true;
        }
        if (room == 0) return false;
        size_t ls = at == 0 ? std::string_view::npos : text.rfind('\n', at - 1);
        ls = ls == std::string_view::npos ? 0 : ls + 1;
        size_t le = text.find('\n', at);
        if (le == std::string_view::npos) le = text.size();
        line_no += static_cast<u32>(std::count(text.begin() + static_cast<std::ptrdiff_t>(counted_to), text.begin() + static_cast<std::ptrdiff_t>(ls), '\n'));
        counted_to = ls;
        std::string_view line = text.substr(ls, le - ls);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        FindHit h;
        h.file = file;
        h.line = line_no;
        h.col = static_cast<u32>(std::min(at - ls, line.size()));
        // (A very long line is kept from a little before the match on.)
        size_t from = h.col > 200 ? h.col - 80 : 0;
        while (from > 0 && (static_cast<u8>(line[from]) & 0xC0) == 0x80) --from;
        h.text = std::string(line.substr(from, 600));
        out->push_back(std::move(h));
        --room;
        any = true;
        line_end = le;
        return true;
    });
    return true;
}

}  // namespace

bool Finder::grep(std::string_view text, std::string_view typed, TextQuery how, u32 file, size_t room, std::vector<FindHit>* out) {
    const Vim::Pattern pat = Vim::make_pattern(typed, how.regex, how.exact);
    if (pat.bad() || pat.empty()) return true;
    return grep_lines(text, pat, file, room, out);
}
#endif

const Finder::Base* Finder::Bases::under(std::string_view typed, const std::shared_ptr<const FileIndex>& ix, u64 now, u64 max_age_ns) const {
    for (size_t i = kept.size(); i-- > 0;) {
        const Base& b = kept[i];
        if (b.index == ix && typed.starts_with(b.typed) && now - b.when_ns < max_age_ns) return &b;
    }
    return nullptr;
}

void Finder::Bases::add(Base b) {
    constexpr size_t kMost = 16;
    std::erase_if(kept, [&](const Base& k) { return k.index != b.index || !b.typed.starts_with(k.typed) || k.typed == b.typed; });
    kept.push_back(std::move(b));
    if (kept.size() > kMost) kept.erase(kept.begin());  // (the shortest)
}

void Finder::search_names(u64 serial, const std::string& typed, std::shared_ptr<const FileIndex> ix, u64 mine) {
    auto r = std::make_shared<FindResults>();
    r->index = ix;
    r->serial = serial;
    const std::vector<FuzzyTerm> terms = fuzzy_terms(typed);
    const Base* b = terms.empty() ? nullptr : names_bases_.under(typed, ix, 0, ~0ull);
    // The files to put first among equals, found in this list once (again
    // when they or the list change).
    if (ix && !terms.empty()) {
        std::vector<std::string> paths;
        bool again = false;
        {
            std::lock_guard lock(mu_);
            again = pref_done_ != pref_gen_ || pref_ix_ != ix;
            if (again) {
                paths = pref_;
                pref_done_ = pref_gen_;
            }
        }
        if (again) {
            pref_ix_ = ix;
            pref_files_ = files_named(*ix, paths);
        }
    }
    if (ix && !ix->error.empty()) {  // (another machine's that could not be listed: why)
        r->note = ix->error;
        if (search_epoch_.load() == mine) publish(std::move(r));
        return;
    }
    std::vector<u32> matched;
    if (ix) rank_names(*ix, terms, r.get(), &search_epoch_, mine, b ? &b->files : nullptr, &matched, pref_ix_ == ix ? std::span<const u32>(pref_files_) : std::span<const u32>());
    if (search_epoch_.load() != mine) return;  // overtaken: its answer is no longer wanted
    if (ix && !terms.empty()) names_bases_.add({typed, ix, std::move(matched), 0});
    publish(std::move(r));
}

void Finder::search_text(u64 serial, const std::string& typed, TextQuery how, std::shared_ptr<const FileIndex> ix, u64 mine) {
    // Words (each anywhere on the line), or a phrase or a pattern.
    const bool words = !how.regex && !how.phrase;
    std::vector<FuzzyTerm> terms;
    size_t longest = 0;
    if (words) {
        terms = fuzzy_terms(typed);
        for (FuzzyTerm& t : terms) {
            t.exact_case = t.exact_case || how.exact;
            longest = std::max(longest, t.text.size());
        }
    }
#if !defined(BLIT_AGENT)
    Vim::Pattern pat;
    if (!words) {
        pat = Vim::make_pattern(typed, how.regex, how.exact);
        longest = pat.bad() || pat.empty() ? 0 : how.regex ? std::max<size_t>(typed.size(), 2) : typed.size();
    }
    const u32 runs = pat.re ? pat.re->overruns() : 0;
#endif
    // What a search made to the end is kept by: what was typed, read how.
    const std::string key = std::string(1, words ? 'w' : how.phrase ? 'p' : 'r') + (how.exact ? 'x' : 's') + typed;
    auto fresh = [&] {
        auto r = std::make_shared<FindResults>();
        r->index = ix;
        r->serial = serial;
        r->text = true;
        return r;
    };
    if (!ix || longest < 2) {  // (one letter is on every line)
        publish(fresh());
        return;
    }
    if (!ix->error.empty()) {  // (another machine's that could not be listed: why)
        auto r = fresh();
        r->note = ix->error;
        publish(std::move(r));
        return;
    }
    u64 max_bytes;
    {
        std::lock_guard lock(mu_);
        max_bytes = opt_.max_file_bytes;
    }
    // Typed onto the end of a search made to the end a moment ago: only the
    // files that had something then are read. (Not for long: the other
    // files may have been changed since.)
    constexpr u64 kBaseFor = 10'000'000'000ull;
    const u64 started = now_ns();
    // (Not a pattern: "a" finds fewer files than "a\|b".)
    const Base* base = how.regex ? nullptr : text_bases_.under(key, ix, started, kBaseFor);
    const bool narrow = base != nullptr;
    const u64 looked_ns = narrow ? base->when_ns : started;  // when every file was last looked through
    const std::vector<u32>* among = narrow ? &base->files : nullptr;
    const size_t count = among ? among->size() : ix->size();
    // Several threads read the files, each taking the next one not yet
    // taken; this one reports what they have found so far, now and then,
    // and at the end.
    std::mutex m;
    std::condition_variable done;
    std::vector<FindHit> hits;
    std::vector<u32> hit_files;
    u64 files_hit = 0;
    unsigned active = 0;
    std::atomic<size_t> next{0};
    std::atomic<bool> full{false};
    auto read = [&] {
        thread_soon(Soon::kWaitedFor);
        std::string text, path;
        std::vector<FindHit> found;
        for (;;) {
            if (full.load(std::memory_order_relaxed) || search_epoch_.load(std::memory_order_relaxed) != mine) break;
            const size_t k = next.fetch_add(1);
            if (k >= count) break;
            const u32 i = among ? (*among)[k] : static_cast<u32>(k);
            path.assign(ix->root).append("/").append(ix->path(i));
            if (!read_file(path, max_bytes, &text, true)) continue;
            found.clear();
#if !defined(BLIT_AGENT)
            if (!(words ? grep(text, terms, i, kMaxLines, &found) : grep_lines(text, pat, i, kMaxLines, &found)) || found.empty()) continue;
#else
            if (!grep(text, terms, i, kMaxLines, &found) || found.empty()) continue;
#endif
            std::lock_guard lock(m);
            ++files_hit;
            hit_files.push_back(i);
            for (FindHit& h : found) hits.push_back(std::move(h));
            if (hits.size() >= kMaxLines) full.store(true);
        }
        std::lock_guard lock(m);
        --active;
        done.notify_one();
    };
    auto report = [&](std::vector<FindHit> some, u64 files, bool searching) {
        std::sort(some.begin(), some.end(), [](const FindHit& a, const FindHit& b) { return a.file != b.file ? a.file < b.file : a.line < b.line; });
        auto r = fresh();
        r->capped = some.size() >= kMaxLines;
        if (some.size() > kMaxLines) some.resize(kMaxLines);
        r->matched = some.size();
        r->files_hit = files;
        r->searching = searching;
        r->hits = std::move(some);
#if !defined(BLIT_AGENT)
        // (Back-references and look-around are tried one way at a time,
        // with a budget for each file: one that ran out found what it had.)
        if (pat.re && pat.re->overruns() != runs) r->note = "The pattern took too long in some files and was stopped there";
#endif
        publish(std::move(r));
    };
#if !defined(BLIT_AGENT)
    if (ix->remote_walk) {  // the files are on another machine: searched there, by its agent
        // A phrase or a pattern: its agent is asked for the lines with the
        // plain text every match has (in either case), and they are looked
        // through here.
        std::string asked = typed;
        if (!words) {
            asked = pat.re ? pat.re->required() : pat.text;
            for (char& ch : asked) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
            if (asked.size() < 2) {
                auto r = fresh();
                r->note = "On another machine a pattern needs two letters or more of plain text in it";
                publish(std::move(r));
                return;
            }
        }
        std::string there;
        bool capped = false;
        const std::shared_ptr<Remote> r = remote_for(ix->root, &there, kRemoteWaitMs);
        if (!r || !r->grep(ix->remote_walk, among, asked, max_bytes, kMaxLines, &hits, &hit_files, &capped)) {
            if (search_epoch_.load() != mine) return;
            auto none = fresh();  // (no answer: why, not that no line has it)
            none->note = remote_error(ix->root);
            if (!none->note.empty()) none->note[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(none->note[0])));
            publish(std::move(none));
            return;
        }
        if (search_epoch_.load() != mine) return;
        if (!words) {
            std::erase_if(hits, [&](FindHit& h) {
                u32 n = 0;
                size_t first = std::string_view::npos;
                Vim::each_whole(h.text, pat, 0, h.text.size() + 1, false, [&](const RegexMatch& x) {
                    if (first == std::string_view::npos) first = static_cast<size_t>(x.start);
                    ++n;
                    return true;
                });
                h.count = n;
                if (n && h.col <= 200) h.col = static_cast<u32>(first);  // (the line is all there: where the match is)
                return n == 0;
            });
            hit_files.clear();
            for (const FindHit& h : hits)
                if (hit_files.empty() || hit_files.back() != h.file) hit_files.push_back(h.file);
        }
        if (!capped && !how.regex) {
            std::sort(hit_files.begin(), hit_files.end());
            hit_files.erase(std::unique(hit_files.begin(), hit_files.end()), hit_files.end());
            text_bases_.add({key, ix, hit_files, looked_ns});
        }
        const u64 files = hit_files.size();
        report(std::move(hits), files, false);
        return;
    }
#endif
    {
        std::vector<std::jthread> readers;
        std::unique_lock lock(m);
        active = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        for (unsigned k = 0; k < active; ++k) readers.emplace_back(read);
        size_t shown = 0;
        while (active > 0) {
            done.wait_for(lock, std::chrono::milliseconds(40));
            if (active > 0 && hits.size() != shown && search_epoch_.load() == mine) {
                shown = hits.size();
                std::vector<FindHit> some = hits;
                const u64 files = files_hit;
                lock.unlock();
                report(std::move(some), files, true);
                lock.lock();
            }
        }
    }
    if (search_epoch_.load() != mine) return;
    if (!full.load() && !how.regex) {
        std::sort(hit_files.begin(), hit_files.end());
        text_bases_.add({key, ix, std::move(hit_files), looked_ns});
    }
    report(std::move(hits), files_hit, false);
}

// ---------------------------------------------------------------- the worker

Finder::Finder() : worker_([this](std::stop_token stop) { run(stop); }), walker_([this](std::stop_token stop) { run_walk(stop); }) {}

Finder::~Finder() {
    worker_.request_stop();
    walker_.request_stop();
    ++index_epoch_;
    ++search_epoch_;
    wake_.notify_all();
}

void Finder::set_notify(std::function<void()> fn) {
    std::lock_guard lock(mu_);
    notify_ = std::move(fn);
}

void Finder::set_options(FinderOptions o) {
    std::lock_guard lock(mu_);
    opt_ = std::move(o);
}

void Finder::set_preferred(std::vector<std::string> paths) {
    std::lock_guard lock(mu_);
    if (paths == pref_) return;  // (nothing to look for again)
    pref_ = std::move(paths);
    ++pref_gen_;
}

void Finder::index(const std::string& root) {
    {
        std::lock_guard lock(mu_);
        std::string trimmed = root;
        while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
        if (index_ && index_->root != trimmed) index_ = nullptr;  // the list of another folder answers nothing here
        want_root_ = root;
        want_index_ = true;
        ++index_epoch_;
    }
    wake_.notify_all();
}

u64 Finder::find_names(std::string_view typed) {
    u64 serial;
    {
        std::lock_guard lock(mu_);
        want_typed_ = std::string(typed);
        want_text_ = false;
        live_ = true;
        serial = ++want_serial_;
        ++search_epoch_;
    }
    wake_.notify_all();
    return serial;
}

u64 Finder::find_text(std::string_view typed, TextQuery how) {
    u64 serial;
    {
        std::lock_guard lock(mu_);
        want_typed_ = std::string(typed);
        want_text_ = true;
        want_how_ = how;
        live_ = true;
        serial = ++want_serial_;
        ++search_epoch_;
    }
    wake_.notify_all();
    return serial;
}

void Finder::cancel() {
    std::lock_guard lock(mu_);
    ++search_epoch_;
    done_serial_ = want_serial_;
    live_ = false;
}

std::shared_ptr<const FindResults> Finder::results() const {
    std::lock_guard lock(mu_);
    return results_;
}

std::shared_ptr<const FileIndex> Finder::files() const {
    std::lock_guard lock(mu_);
    return index_;
}

bool Finder::idle() const {
    std::lock_guard lock(mu_);
    return !busy_ && !want_index_ && !indexing_ && want_serial_ == done_serial_;
}

void Finder::wait_idle() const {
    std::unique_lock lock(mu_);
    settled_.wait(lock, [&] { return !busy_ && !want_index_ && !indexing_ && want_serial_ == done_serial_; });
}

bool Finder::wait_idle_for(int ms) const {
    std::unique_lock lock(mu_);
    // The search; and the reading of the folder too while there is no list
    // of it to search yet (a search of nothing is no answer). A folder
    // already listed is searched as it was while it is read again.
    return settled_.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        const bool listed = index_ && index_->root == want_root_ && index_->size() > 0;
        return !busy_ && want_serial_ == done_serial_ && (listed || (!want_index_ && !indexing_));
    });
}

void Finder::publish(std::shared_ptr<FindResults> r) {
    std::function<void()> tell;
    {
        std::lock_guard lock(mu_);
        r->indexing = indexing_ || want_index_;
        results_ = std::move(r);
        tell = notify_;
    }
    if (tell) tell();
}

// Searches are answered at once from the list there is. Reading the folder
// again goes on in the background on a thread of its own (a search never
// waits behind it), and when it is done what was searched for is searched
// for again in the new list.
void Finder::run(std::stop_token stop) {
    thread_soon(Soon::kWaitedFor);
    std::unique_lock lock(mu_);
    for (;;) {
        wake_.wait(lock, stop, [&] { return want_serial_ != done_serial_; });
        if (stop.stop_requested()) return;
        busy_ = true;
        const u64 serial = want_serial_;
        const bool text = want_text_;
        const TextQuery how = want_how_;
        const std::string typed = want_typed_;
        const std::shared_ptr<const FileIndex> ix = index_;
        const u64 mine = search_epoch_.load();  // (under the lock that gave the serial: a search started after it overtakes this one)
        lock.unlock();
        if (text) search_text(serial, typed, how, ix, mine);
        else search_names(serial, typed, ix, mine);
        lock.lock();
        if (want_serial_ == serial && index_ == ix) done_serial_ = serial;  // (a new list came meanwhile: it is searched again)
        busy_ = false;
        if (want_serial_ == done_serial_) settled_.notify_all();
    }
}

void Finder::run_walk(std::stop_token stop) {
    thread_soon(Soon::kWaitedFor);
    std::unique_lock lock(mu_);
    for (;;) {
        wake_.wait(lock, stop, [&] { return want_index_; });
        if (stop.stop_requested()) return;
        want_index_ = false;
        indexing_ = true;
        const std::string root = want_root_;
        const FinderOptions o = opt_;
        const u64 mine = index_epoch_.load();
        lock.unlock();
        std::shared_ptr<FileIndex> ix = walk(root, o, &index_epoch_, mine);
        lock.lock();
        indexing_ = false;
        if (index_epoch_.load() == mine) {
            index_ = std::move(ix);
            if (live_) done_serial_ = 0;  // what was searched for is searched for again, in the new list
            const std::function<void()> tell = notify_;
            lock.unlock();
            wake_.notify_all();
            if (tell) tell();
            lock.lock();
        }
        if (!busy_ && !want_index_ && !indexing_ && want_serial_ == done_serial_) settled_.notify_all();
    }
}

}  // namespace blit
