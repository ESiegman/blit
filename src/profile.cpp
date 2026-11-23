#include "profile.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>

#include "json.hpp"
#include "missing.hpp"
#include "remote.hpp"

extern char** environ;

namespace blit {

// ---------------------------------------------------------------- the profile

namespace {

std::string_view base_name(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}  // namespace

void Profile::add(std::span<const Frame> frames, u64 w, u64 thread, std::string_view thread_name, double time) {
    if (nodes.empty()) {
        nodes.push_back({});
        funcs.push_back({std::string(base_name(program)), program, {}, 0, 0, 0, true});  // [0]: the program, all of it
    }
    if (w == 0) w = 1;
    ++samples;
    ++sample_no_;
    weight += w;
    nodes[0].total += w;
    funcs[0].total += w;
    const u32 leaf = add_path(0, frames, w, true);
    // And in its thread's own tree, under a root of its own.
    auto [it, fresh] = thread_ids_.try_emplace(thread, static_cast<u32>(by_thread.size()));
    if (fresh) {
        nodes.push_back({0, 0, 0, 0, {}});
        by_thread.push_back({thread_name.empty() ? "Thread " + std::to_string(thread) : std::string(thread_name), thread, 0, 0, static_cast<u32>(nodes.size() - 1)});
    }
    ProfileThread& t = by_thread[it->second];
    // A thread that names itself once it is going (pthread_setname_np) is
    // called by its program's name in perf's first samples of it: the name
    // it has last is its name.
    if (!fresh && !thread_name.empty() && t.name != thread_name) t.name = thread_name;
    t.weight += w;
    ++t.samples;
    nodes[t.root].total += w;
    const u32 thread_leaf = add_path(t.root, frames, w, false);
    if (time >= 0) timeline.push_back({time, leaf, thread_leaf, thread, w});
}

// A sample's frames into the tree under `root` (counts: and into the
// functions' weights and the lines', which are the whole profile's).
u32 Profile::add_path(u32 root, std::span<const Frame> frames, u64 w, bool counts) {
    std::vector<std::pair<std::string_view, u32>> lines_now;  // (each line counted once in a sample)
    u32 node = root;
    for (size_t k = frames.size(); k-- > 0;) {  // from the outermost in
        const Frame& f = frames[k];
        std::string key;
        key.reserve(f.name.size() + f.module.size() + 1);
        key.append(f.name).push_back('\0');
        key.append(f.module);
        auto [fi, fresh_func] = func_ids_.try_emplace(std::move(key), static_cast<u32>(funcs.size()));
        const u32 fid = fi->second;
        if (fresh_func) funcs.push_back({std::string(f.name), std::string(f.module), {}, 0, 0, 0, false});
        if (counts) {
            ProfileFunc& func = funcs[fid];
            if (seen_.size() <= fid) seen_.resize(fid + 1, 0);
            if (seen_[fid] != sample_no_) {
                seen_[fid] = sample_no_;
                func.total += w;
            }
            if (!f.file.empty() && f.line) {
                if (func.file.empty()) func.file = f.file;
                if (func.file == f.file && (func.line == 0 || f.line < func.line)) func.line = f.line;
                const std::pair<std::string_view, u32> here{f.file, f.line};
                if (std::ranges::find(lines_now, here) == lines_now.end()) {
                    lines_now.push_back(here);
                    lines[std::string(f.file)][f.line].total += w;
                }
            }
        }
        auto [ni, fresh_node] = node_ids_.try_emplace(static_cast<u64>(node) << 32 | fid, static_cast<u32>(nodes.size()));
        if (fresh_node) {
            nodes.push_back({fid, node, 0, 0, {}});
            nodes[node].kids.push_back(ni->second);
        }
        node = ni->second;
        nodes[node].total += w;
    }
    nodes[node].self += w;
    if (frames.empty() || !counts) return node;
    funcs[nodes[node].func].self += w;
    const Frame& leaf = frames.front();
    if (!leaf.file.empty() && leaf.line) lines[std::string(leaf.file)][leaf.line].self += w;
    return node;
}

Profile narrow_profile(const Profile& p, double t0, double t1) {
    Profile q = p;
    q.weight = q.samples = 0;
    q.seconds = std::max(0.0, t1 - t0);
    for (ProfileNode& n : q.nodes) n.total = n.self = 0;
    for (ProfileFunc& f : q.funcs) f.self = f.total = 0;
    for (ProfileThread& t : q.by_thread) t.weight = t.samples = 0;
    std::unordered_map<u64, size_t> thread_of;
    for (size_t i = 0; i < q.by_thread.size(); ++i) thread_of.emplace(q.by_thread[i].id, i);
    std::vector<u64> seen(q.funcs.size(), ~0ull);
    for (size_t k = 0; k < p.timeline.size(); ++k) {
        const Profile::Sample& s = p.timeline[k];
        if (s.t < t0 || s.t > t1) continue;
        q.weight += s.w;
        ++q.samples;
        if (s.leaf < q.nodes.size()) {  // the whole tree, and the functions' weights (each once a sample)
            q.nodes[s.leaf].self += s.w;
            q.funcs[q.nodes[s.leaf].func].self += s.w;
            for (u32 n = s.leaf;; n = q.nodes[n].parent) {
                q.nodes[n].total += s.w;
                if (seen[q.nodes[n].func] != k) seen[q.nodes[n].func] = k, q.funcs[q.nodes[n].func].total += s.w;
                if (n == 0) break;
            }
        }
        if (const auto it = thread_of.find(s.thread); it != thread_of.end() && s.thread_leaf < q.nodes.size()) {  // its thread's tree
            ProfileThread& t = q.by_thread[it->second];
            t.weight += s.w;
            ++t.samples;
            q.nodes[s.thread_leaf].self += s.w;
            for (u32 n = s.thread_leaf;; n = q.nodes[n].parent) {
                q.nodes[n].total += s.w;
                if (n == t.root) break;
            }
        }
    }
    for (ProfileNode& node : q.nodes)
        std::ranges::stable_sort(node.kids, [&](u32 a, u32 b) { return q.nodes[a].total > q.nodes[b].total; });
    return q;
}

Profile invert_profile(const Profile& p) {
    Profile q;
    q.program = p.program, q.args = p.args, q.backend = p.backend, q.host = p.host, q.when = p.when;
    q.weight = p.weight, q.samples = p.samples, q.seconds = p.seconds, q.threads = p.threads;
    q.unit = p.unit, q.kind = p.kind, q.funcs = p.funcs, q.lines = p.lines, q.output = p.output, q.note = p.note;
    if (p.nodes.empty()) return q;
    q.nodes.push_back({p.nodes[0].func, 0, p.nodes[0].total, 0, {}});
    std::vector<bool> is_root(p.nodes.size(), false);
    std::vector<u32> root_of(p.nodes.size(), 0);  // (an original root -> its root here)
    is_root[0] = true;
    for (const ProfileThread& t : p.by_thread) {
        is_root[t.root] = true;
        root_of[t.root] = static_cast<u32>(q.nodes.size());
        q.nodes.push_back({p.nodes[t.root].func, 0, p.nodes[t.root].total, 0, {}});
        ProfileThread nt = t;
        nt.root = root_of[t.root];
        q.by_thread.push_back(nt);
    }
    std::unordered_map<u64, u32> ids;  // parent << 32 | func
    auto child = [&](u32 parent, u32 func) {
        const u64 key = static_cast<u64>(parent) << 32 | func;
        if (const auto it = ids.find(key); it != ids.end()) return it->second;
        const u32 id = static_cast<u32>(q.nodes.size());
        q.nodes.push_back({func, parent, 0, 0, {}});
        q.nodes[parent].kids.push_back(id);
        ids.emplace(key, id);
        return id;
    };
    for (u32 n = 1; n < p.nodes.size(); ++n) {
        if (is_root[n] || p.nodes[n].self == 0) continue;
        const u64 w = p.nodes[n].self;
        u32 r = n;  // (the root it is under)
        while (!is_root[r]) r = p.nodes[r].parent;
        u32 at = root_of[r];
        bool first = true;
        for (u32 m = n; !is_root[m]; m = p.nodes[m].parent) {
            at = child(at, p.nodes[m].func);
            q.nodes[at].total += w;
            if (first) q.nodes[at].self += w;  // (its own time: where it was spent)
            first = false;
        }
    }
    for (ProfileNode& node : q.nodes)
        std::ranges::sort(node.kids, [&](u32 a, u32 b) { return q.nodes[a].total != q.nodes[b].total ? q.nodes[a].total > q.nodes[b].total : q.funcs[q.nodes[a].func].name < q.funcs[q.nodes[b].func].name; });
    return q;
}

ProfileDiff diff_profiles(const Profile& before, const Profile& after) {
    ProfileDiff d;
    d.node_before.assign(after.nodes.size(), -1.0);
    if (after.nodes.empty() || before.nodes.empty()) return d;
    const double bw = before.weight ? static_cast<double>(before.weight) : 1.0, aw = after.weight ? static_cast<double>(after.weight) : 1.0;
    auto key = [](const ProfileFunc& f) { return f.name + '\0' + f.module; };
    // The same paths: from the roots down, each kid matched by its function.
    d.node_before[0] = 1.0;
    std::vector<std::pair<u32, u32>> todo{{0, 0}};
    while (!todo.empty()) {
        const auto [a, b] = todo.back();
        todo.pop_back();
        for (const u32 ka : after.nodes[a].kids) {
            const std::string want = key(after.funcs[after.nodes[ka].func]);
            for (const u32 kb : before.nodes[b].kids)
                if (key(before.funcs[before.nodes[kb].func]) == want) {
                    d.node_before[ka] = static_cast<double>(before.nodes[kb].total) / bw;
                    todo.emplace_back(ka, kb);
                    break;
                }
        }
    }
    // The functions, by their names.
    std::unordered_map<std::string, u32> was;
    for (u32 f = 1; f < before.funcs.size(); ++f) was.emplace(key(before.funcs[f]), f);
    std::vector<bool> seen(before.funcs.size(), false);
    for (u32 f = 1; f < after.funcs.size(); ++f) {
        const ProfileFunc& af = after.funcs[f];
        if (!af.total) continue;
        ProfileDiff::Row r;
        r.func = f;
        r.name = af.name;
        r.self_after = static_cast<double>(af.self) / aw, r.total_after = static_cast<double>(af.total) / aw;
        if (const auto it = was.find(key(af)); it != was.end()) {
            seen[it->second] = true;
            r.self_before = static_cast<double>(before.funcs[it->second].self) / bw, r.total_before = static_cast<double>(before.funcs[it->second].total) / bw;
        }
        d.rows.push_back(std::move(r));
    }
    for (u32 f = 1; f < before.funcs.size(); ++f)
        if (!seen[f] && before.funcs[f].total) {
            ProfileDiff::Row r;
            r.name = before.funcs[f].name;
            r.self_before = static_cast<double>(before.funcs[f].self) / bw, r.total_before = static_cast<double>(before.funcs[f].total) / bw;
            d.rows.push_back(std::move(r));
        }
    std::ranges::stable_sort(d.rows, [](const ProfileDiff::Row& x, const ProfileDiff::Row& y) {
        const double dx = std::abs(x.self_after - x.self_before), dy = std::abs(y.self_after - y.self_before);
        return dx != dy ? dx > dy : std::abs(x.total_after - x.total_before) > std::abs(y.total_after - y.total_before);
    });
    return d;
}

bool Profile::thread_root(u32 node) const {
    return node != 0 && std::ranges::any_of(by_thread, [&](const ProfileThread& t) { return t.root == node; });
}

void Profile::weights_under(u32 root, std::vector<u64>* self, std::vector<u64>* total) const {
    self->assign(funcs.size(), 0);
    total->assign(funcs.size(), 0);
    if (root >= nodes.size()) return;
    // Depth first, each function's total counted at the outermost of it on a path (a recursion is not counted twice).
    std::vector<u32> on_path(funcs.size(), 0);
    std::vector<std::pair<u32, bool>> todo{{root, false}};  // (node, leaving it)
    while (!todo.empty()) {
        const auto [n, leaving] = todo.back();
        todo.pop_back();
        const ProfileNode& node = nodes[n];
        if (leaving) {
            --on_path[node.func];
            continue;
        }
        (*self)[node.func] += node.self;
        if (on_path[node.func]++ == 0) (*total)[node.func] += node.total;
        todo.push_back({n, true});
        for (const u32 k : node.kids) todo.push_back({k, false});
    }
}

void Profile::finish() {
    if (nodes.empty()) {
        nodes.push_back({});
        funcs.push_back({std::string(base_name(program)), program, {}, 0, 0, 0, true});
    }
    for (ProfileNode& n : nodes)
        std::ranges::sort(n.kids, [&](u32 a, u32 b) { return nodes[a].total != nodes[b].total ? nodes[a].total > nodes[b].total : funcs[nodes[a].func].name < funcs[nodes[b].func].name; });
    const std::string_view prog = base_name(program);
    for (size_t i = 1; i < funcs.size(); ++i) funcs[i].own = !prog.empty() && base_name(funcs[i].module) == prog;
    std::ranges::stable_sort(by_thread, [](const ProfileThread& a, const ProfileThread& b) { return a.weight > b.weight; });
    threads = std::max<u32>(threads, static_cast<u32>(by_thread.size()));
    if (!timeline.empty()) {  // (times from the first sample)
        double t0 = timeline[0].t;
        for (const Sample& s : timeline) t0 = std::min(t0, s.t);
        for (Sample& s : timeline) s.t -= t0;
    }
    func_ids_.clear();
    node_ids_.clear();
    thread_ids_.clear();
    seen_.clear();
}

// ---------------------------------------------------------------- as a file

// JSON: what was run and how, then the functions, the nodes (a node's
// parent; -1 for a root, which is no node's kid), the threads, the lines.
std::string Profile::to_json() const {
    std::string o = "{\"blit_profile\":1";
    auto field = [&](const char* k, std::string_view v) {
        o += ",\"";
        o += k;
        o += "\":";
        json::quote(o, v);
    };
    auto num = [&](const char* k, double v) { o += std::format(",\"{}\":{}", k, v); };
    field("program", program), field("args", args), field("backend", backend), field("host", host), field("when", when);
    field("unit", unit), field("output", output), field("note", note), field("kind", kind);
    num("weight", static_cast<double>(weight)), num("samples", static_cast<double>(samples)), num("seconds", seconds), num("threads", threads);
    o += ",\"funcs\":[";
    for (size_t i = 0; i < funcs.size(); ++i) {
        const ProfileFunc& f = funcs[i];
        o += i ? ",[" : "[";
        json::quote(o, f.name), o += ",", json::quote(o, f.module), o += ",", json::quote(o, f.file);
        o += std::format(",{},{},{},{}]", f.line, f.self, f.total, f.own ? 1 : 0);
    }
    o += "],\"nodes\":[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        const ProfileNode& n = nodes[i];
        const bool root = i == 0 || thread_root(static_cast<u32>(i));
        o += std::format("{}[{},{},{},{}]", i ? "," : "", n.func, root ? -1 : static_cast<i64>(n.parent), n.total, n.self);
    }
    o += "],\"by_thread\":[";
    for (size_t i = 0; i < by_thread.size(); ++i) {
        const ProfileThread& t = by_thread[i];
        o += i ? ",[" : "[";
        json::quote(o, t.name);
        o += std::format(",{},{},{},{}]", t.id, t.weight, t.samples, t.root);
    }
    o += "],\"timeline\":[";
    for (size_t i = 0; i < timeline.size(); ++i) {
        const Sample& s = timeline[i];
        o += std::format("{}[{},{},{},{},{}]", i ? "," : "", s.t, s.leaf, s.thread_leaf, s.thread, s.w);
    }
    o += "],\"lines\":{";
    bool first = true;
    for (const auto& [file, by_line] : lines) {
        if (!first) o += ",";
        first = false;
        json::quote(o, file);
        o += ":[";
        bool one = true;
        for (const auto& [line, w] : by_line) o += std::format("{}[{},{},{}]", one ? "" : ",", line, w.self, w.total), one = false;
        o += "]";
    }
    return o + "}}";
}

bool Profile::from_json(std::string_view text, Profile* out, std::string* err) {
    json::Doc doc;
    if (!doc.parse(std::string(text)) || doc.root()["blit_profile"].integer() != 1) {
        if (err) *err = "not a profile blit saved";
        return false;
    }
    const json::Value r = doc.root();
    Profile p;
    p.program = r["program"].str(), p.args = r["args"].str(), p.backend = r["backend"].str(), p.host = r["host"].str(), p.when = r["when"].str();
    p.unit = r["unit"].str(), p.output = r["output"].str(), p.note = r["note"].str();
    p.kind = r["kind"].str().empty() ? "time" : r["kind"].str();
    p.weight = static_cast<u64>(r["weight"].integer()), p.samples = static_cast<u64>(r["samples"].integer());
    p.seconds = r["seconds"].number(), p.threads = static_cast<u32>(r["threads"].integer());
    for (json::Value f : r["funcs"])
        p.funcs.push_back({std::string(f.at(0).str()), std::string(f.at(1).str()), std::string(f.at(2).str()), static_cast<u32>(f.at(3).integer()),
                           static_cast<u64>(f.at(4).integer()), static_cast<u64>(f.at(5).integer()), f.at(6).integer() != 0});
    for (json::Value n : r["nodes"]) {
        const i64 parent = n.at(1).integer(-1);
        const u32 func = static_cast<u32>(n.at(0).integer());
        if (func >= p.funcs.size() || parent >= static_cast<i64>(p.nodes.size())) {  // (a node comes after its parent)
            if (err) *err = "the profile's file is damaged";
            return false;
        }
        p.nodes.push_back({func, parent < 0 ? 0u : static_cast<u32>(parent), static_cast<u64>(n.at(2).integer()), static_cast<u64>(n.at(3).integer()), {}});
        if (parent >= 0) p.nodes[static_cast<size_t>(parent)].kids.push_back(static_cast<u32>(p.nodes.size() - 1));
    }
    for (json::Value t : r["by_thread"]) {
        const u32 root = static_cast<u32>(t.at(4).integer());
        if (root >= p.nodes.size()) continue;
        p.by_thread.push_back({std::string(t.at(0).str()), static_cast<u64>(t.at(1).integer()), static_cast<u64>(t.at(2).integer()), static_cast<u64>(t.at(3).integer()), root});
    }
    for (json::Value s : r["timeline"]) {
        const u32 leaf = static_cast<u32>(s.at(1).integer()), thread_leaf = static_cast<u32>(s.at(2).integer());
        if (leaf >= p.nodes.size() || thread_leaf >= p.nodes.size()) continue;
        p.timeline.push_back({s.at(0).number(), leaf, thread_leaf, static_cast<u64>(s.at(3).integer()), static_cast<u64>(s.at(4).integer())});
    }
    for (json::Value file : r["lines"])
        for (json::Value l : file) p.lines[std::string(file.key())][static_cast<u32>(l.at(0).integer())] = {static_cast<u64>(l.at(1).integer()), static_cast<u64>(l.at(2).integer())};
    if (p.nodes.empty() || p.funcs.empty()) {
        if (err) *err = "the profile's file is damaged";
        return false;
    }
    for (ProfileNode& n : p.nodes)
        std::ranges::sort(n.kids, [&](u32 a, u32 b) { return p.nodes[a].total != p.nodes[b].total ? p.nodes[a].total > p.nodes[b].total : p.funcs[p.nodes[a].func].name < p.funcs[p.nodes[b].func].name; });
    *out = std::move(p);
    return true;
}

std::vector<u32> Profile::by_self() const {
    std::vector<u32> out;
    for (u32 i = 1; i < funcs.size(); ++i) out.push_back(i);
    std::ranges::sort(out, [&](u32 a, u32 b) { return funcs[a].self != funcs[b].self ? funcs[a].self > funcs[b].self : funcs[a].total > funcs[b].total; });
    return out;
}

// ---------------------------------------------------------------- perf

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// "file:line" (and " (inlined)" after it, or not): the file and line, or false.
bool split_srcline(std::string_view s, std::string_view* file, u32* line) {
    s = trim(s);
    if (s.ends_with(" (inlined)")) s.remove_suffix(10);
    const size_t colon = s.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= s.size()) return false;
    u32 n = 0;
    const auto [p, ec] = std::from_chars(s.data() + colon + 1, s.data() + s.size(), n);
    if (ec != std::errc() || p != s.data() + s.size() || n == 0) return false;
    *file = s.substr(0, colon);
    if (*file == "??") return false;
    *line = n;
    return true;
}

}  // namespace

// `perf script -F comm,tid,time,period,ip,sym,dso,srcline`: a line for each
// sample ("comm tid time: period"), then a line for each frame, the
// innermost first ("ip symbol (dso)", no dso for an inlined one), each
// followed by its source line ("  file:line") when perf knows it, and a
// blank line.
bool parse_perf_script(std::string_view text, const std::string& prefix, Profile* out, std::string* err, u64 scale) {
    struct F {
        std::string_view name, module, file;
        u32 line = 0;
    };
    std::vector<F> frames;
    std::deque<std::string> files;  // (prefixed paths, which the sample's frames point into: a deque does not move them)
    u64 period = 0, tid = 0;
    double when = -1;       // (the sample's time)
    std::string_view comm;  // (the sample's thread's name)
    bool in_sample = false, any_line = false;
    double first = -1, last = 0;
    std::vector<u64> tids;
    std::vector<Profile::Frame> pf;
    auto flush = [&] {
        if (!in_sample) return;
        in_sample = false;
        // An inlined frame is in the module of the frame it was inlined into (the next one out).
        for (size_t k = frames.size(); k-- > 0;)
            if (frames[k].module.empty() && k + 1 < frames.size()) frames[k].module = frames[k + 1].module;
        // One perf has no name for is named after where it is ("[libc.so.6]").
        for (F& f : frames)
            if (f.name == "[unknown]" && !f.module.empty()) {
                files.push_back("[" + std::string(base_name(f.module)) + "]");
                f.name = files.back();
            }
        pf.clear();
        for (const F& f : frames) pf.push_back({f.name, f.module, f.file, f.line});
        out->add(pf, period * scale, tid, comm, when);
        frames.clear();
        files.clear();
    };
    size_t at = 0;
    while (at <= text.size()) {
        const size_t nl = text.find('\n', at);
        const std::string_view line = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (trim(line).empty()) {
            flush();
            continue;
        }
        if (line[0] != ' ' && line[0] != '\t') {  // a sample: "comm  tid  time:  period"
            flush();
            in_sample = true;
            period = 1;
            tid = 0;
            when = -1;
            comm = {};
            std::string_view s = trim(line);
            const size_t sp = s.find_last_of(" \t");
            if (sp != std::string_view::npos) {
                u64 p = 0;
                const std::string_view tok = s.substr(sp + 1);
                if (std::from_chars(tok.data(), tok.data() + tok.size(), p).ec == std::errc()) period = p ? p : 1;
                s = trim(s.substr(0, sp));
            }
            if (s.ends_with(':')) {  // the time
                const size_t t0 = s.find_last_of(" \t");
                const std::string_view tok = s.substr(t0 == std::string_view::npos ? 0 : t0 + 1, s.size() - 1 - (t0 == std::string_view::npos ? 0 : t0 + 1));
                const double t = std::strtod(std::string(tok).c_str(), nullptr);
                when = t;
                if (first < 0) first = t;
                last = std::max(last, t);
                s = t0 == std::string_view::npos ? std::string_view{} : trim(s.substr(0, t0));
                const size_t t1 = s.find_last_of(" \t");  // the thread, and its name before it
                const std::string_view tt = s.substr(t1 == std::string_view::npos ? 0 : t1 + 1);
                if (std::from_chars(tt.data(), tt.data() + tt.size(), tid).ec == std::errc() && std::ranges::find(tids, tid) == tids.end()) tids.push_back(tid);
                if (t1 != std::string_view::npos) comm = trim(s.substr(0, t1));
            }
            continue;
        }
        if (!in_sample) continue;
        if (line[0] == '\t' || (line.size() > 2 && line[0] == ' ' && line[1] == ' ' && line[2] == ' ')) {
            // A frame: "ip symbol (dso)". The dso is the last group in
            // brackets, and starts with / or [ (a C++ name can end in
            // brackets too: an inlined frame has no dso).
            std::string_view s = trim(line);
            const size_t sp = s.find(' ');
            if (sp == std::string_view::npos) continue;
            s = trim(s.substr(sp + 1));
            F f;
            if (s.ends_with(')')) {
                int depth = 0;
                size_t open = std::string_view::npos;
                for (size_t k = s.size(); k-- > 0;) {
                    if (s[k] == ')') ++depth;
                    else if (s[k] == '(' && --depth == 0) {
                        open = k;
                        break;
                    }
                }
                if (open != std::string_view::npos && open + 1 < s.size() && (s[open + 1] == '/' || s[open + 1] == '[')) {
                    f.module = s.substr(open + 1, s.size() - open - 2);
                    s = trim(s.substr(0, open));
                }
            }
            f.name = s;
            frames.push_back(f);
            continue;
        }
        // The frame's source line.
        std::string_view file;
        u32 ln = 0;
        if (!frames.empty() && split_srcline(line, &file, &ln)) {
            files.push_back(prefix + std::string(file));
            frames.back().file = files.back();
            frames.back().line = ln;
            any_line = true;
        }
    }
    flush();
    if (out->samples == 0) {
        if (err) *err = "perf recorded no samples";
        return false;
    }
    out->seconds = first >= 0 ? last - first : 0;
    out->threads = static_cast<u32>(tids.size());
    out->unit = "ns";
    if (!any_line) out->note = "No source lines: build the program with -g to see them.";
    return true;
}

// perf's context switches as waits. A sample of the "context-switches"
// event is where a thread stopped running (its stack); the switch records
// after it say when it ran again ("PERF_RECORD_SWITCH IN", for its thread)
// or that it was only preempted ("OUT preempt": it did not wait). Each wait
// comes out as a sample of that stack whose period is the nanoseconds it
// lasted, for parse_perf_script().
std::string perf_waits_as_samples(std::string_view text) {
    struct Waiting {
        double at = 0;
        std::string head, frames;  // the sample's line (without its period) and its frames
        bool on = false;
    };
    std::unordered_map<u64, Waiting> waiting;
    std::unordered_map<u64, double> preempted;  // when each thread was last preempted
    std::string out;
    // A line's thread and time: "comm  tid  time: ...", the name may have spaces.
    auto parse_head = [](std::string_view line, u64* tid, double* at, std::string_view* rest) {
        const size_t colon = line.find(": ");
        const size_t end = colon == std::string_view::npos ? (line.ends_with(':') ? line.size() - 1 : std::string_view::npos) : colon;
        if (end == std::string_view::npos) return false;
        const std::string_view left = trim(line.substr(0, end));
        const size_t t0 = left.find_last_of(" \t");
        if (t0 == std::string_view::npos) return false;
        *at = std::strtod(std::string(left.substr(t0 + 1)).c_str(), nullptr);
        const std::string_view before = trim(left.substr(0, t0));
        const size_t t1 = before.find_last_of(" \t");
        const std::string_view tt = before.substr(t1 == std::string_view::npos ? 0 : t1 + 1);
        if (std::from_chars(tt.data(), tt.data() + tt.size(), *tid).ec != std::errc()) return false;
        *rest = end + 1 < line.size() ? trim(line.substr(end + 1)) : std::string_view{};
        return true;
    };
    size_t at = 0;
    Waiting* building = nullptr;  // the sample whose frames are being read
    while (at <= text.size()) {
        const size_t nl = text.find('\n', at);
        const std::string_view line = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (trim(line).empty()) {
            building = nullptr;
            continue;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            if (building) building->frames.append(line).push_back('\n');
            continue;
        }
        building = nullptr;
        u64 tid = 0;
        double t = 0;
        std::string_view rest;
        if (!parse_head(line, &tid, &t, &rest)) continue;
        if (rest.starts_with("PERF_RECORD_SWITCH")) {
            Waiting& w = waiting[tid];
            if (rest.find("OUT") != std::string_view::npos) {
                if (rest.find("preempt") != std::string_view::npos) {
                    preempted[tid] = t;
                    if (w.on && t - w.at < 1e-4) w.on = false;  // (the sample just taken was of a preemption)
                } else if (w.on) {
                    w.at = t;  // (the switch itself: when it stopped)
                }
            } else if (rest.find("IN") != std::string_view::npos && w.on) {
                const u64 ns = static_cast<u64>(std::llround(std::max(0.0, (t - w.at) * 1e9)));
                if (ns > 0) out += w.head + " " + std::to_string(ns) + "\n" + w.frames + "\n";
                w.on = false;
            }
            continue;
        }
        // A sample: where the thread stopped.
        Waiting& w = waiting[tid];
        const auto p = preempted.find(tid);
        w.on = !(p != preempted.end() && t - p->second < 1e-4 && t >= p->second);
        w.at = t;
        const size_t colon = line.find(": ");
        w.head = std::string(colon == std::string_view::npos ? line : line.substr(0, colon + 1));
        w.frames.clear();
        building = &w;
    }
    return out;
}

// ---------------------------------------------------------------- xctrace

namespace {

// Just enough XML for xctrace's export: elements, their attributes, their
// text; entities decoded.
std::string decode(std::string_view s) {
    if (s.find('&') == std::string_view::npos) return std::string(s);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        const std::string_view e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (!e.empty() && e[0] == '#') {
            u32 c = 0;
            if (e.size() > 1 && (e[1] == 'x' || e[1] == 'X')) std::from_chars(e.data() + 2, e.data() + e.size(), c, 16);
            else std::from_chars(e.data() + 1, e.data() + e.size(), c);
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) out += static_cast<char>(0xC0 | (c >> 6)), out += static_cast<char>(0x80 | (c & 0x3F));
            else if (c < 0x10000) out += static_cast<char>(0xE0 | (c >> 12)), out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
            else out += static_cast<char>(0xF0 | (c >> 18)), out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)), out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out.append(s.substr(i, semi - i + 1));
        }
        i = semi;
    }
    return out;
}

struct Tag {
    std::string_view name;
    std::string_view attrs;
    bool close = false, empty = false;
    std::string attr(std::string_view key) const {
        size_t at = 0;
        while ((at = attrs.find(key, at)) != std::string_view::npos) {
            const bool start = at == 0 || attrs[at - 1] == ' ' || attrs[at - 1] == '\t' || attrs[at - 1] == '\n';
            const size_t eq = at + key.size();
            if (start && eq + 1 < attrs.size() && attrs[eq] == '=' && (attrs[eq + 1] == '"' || attrs[eq + 1] == '\'')) {
                const char q = attrs[eq + 1];
                const size_t end = attrs.find(q, eq + 2);
                if (end == std::string_view::npos) return {};
                return decode(attrs.substr(eq + 2, end - eq - 2));
            }
            at = eq;
        }
        return {};
    }
};

}  // namespace

// The "time-profile" table: a row for each sample, with its thread, its
// weight and its backtrace (frames, the innermost first, each with its
// binary and, when there is debug information, its source line). What has
// come once is given an id and later only referred to (ref="id"). The
// "virtual-memory" table is the same with a fault in each row (its kind,
// its bytes: only new memory, "Zero Fill", is kept), and "syscall" with a
// system call in each (how long it took).
bool parse_xctrace(std::string_view xml, const std::string& prefix, Profile* out, std::string* err) {
    enum Mode { kSamples, kFaults, kCalls } mode = kSamples;
    std::string_view weight_tag = "weight";
    std::unordered_map<std::string, std::string> ops;  // a fault's kind, by id
    std::string row_op;
    struct XFrame {
        std::string name, module, file;
        u32 line = 0;
    };
    std::unordered_map<std::string, u64> weights;
    std::unordered_map<std::string, std::vector<u32>> backtraces;
    std::unordered_map<std::string, u32> frame_ids;
    std::unordered_map<std::string, std::string> binaries, paths, thread_names;  // (by the thread's id: "Main Thread", "worker")
    std::vector<XFrame> frames;
    std::vector<std::string> tids;
    u64 first = ~0ull, last = 0;
    bool any_line = false, any_row = false;

    // The row being read.
    u64 row_weight = 0, row_thread = 0;
    double row_time = -1;  // (the row's time, in seconds)
    std::vector<u32> row_bt;
    bool in_row = false;
    // What is being read inside it.
    std::vector<u32>* bt = nullptr;  // a backtrace being defined
    std::string bt_id;
    int frame = -1;                  // a frame being defined
    std::string text_of;             // an element whose text is wanted: "weight:id", "path:id", "time"
    std::string text;
    std::vector<u32> building;
    std::vector<Profile::Frame> pf;

    size_t at = 0;
    while (at < xml.size()) {
        const size_t lt = xml.find('<', at);
        if (lt == std::string_view::npos) break;
        if (!text_of.empty()) text.append(xml.substr(at, lt - at));
        const size_t gt = xml.find('>', lt);
        if (gt == std::string_view::npos) break;
        at = gt + 1;
        std::string_view body = xml.substr(lt + 1, gt - lt - 1);
        if (body.empty() || body[0] == '?' || body[0] == '!') continue;
        Tag t;
        if (body[0] == '/') {
            t.close = true;
            body.remove_prefix(1);
        }
        if (!body.empty() && body.back() == '/') {
            t.empty = true;
            body.remove_suffix(1);
        }
        const size_t sp = body.find_first_of(" \t\n");
        t.name = body.substr(0, sp);
        t.attrs = sp == std::string_view::npos ? std::string_view{} : body.substr(sp + 1);

        if (t.close) {
            if (!text_of.empty() && (t.name == weight_tag || t.name == "path" || t.name == "sample-time" || t.name == "start-time" || t.name == "vm-op")) {
                const std::string value = decode(text);
                if (text_of.starts_with("op:")) {
                    ops[text_of.substr(3)] = value;
                    if (in_row) row_op = value;
                } else if (text_of.starts_with("weight:")) {
                    const u64 v = std::strtoull(value.c_str(), nullptr, 10);
                    weights[text_of.substr(7)] = v;
                    if (in_row) row_weight = v;
                } else if (text_of.starts_with("path:")) {
                    paths[text_of.substr(5)] = value;
                    if (frame >= 0) frames[static_cast<size_t>(frame)].file = prefix + value;
                } else if (text_of == "time") {
                    const u64 v = std::strtoull(value.c_str(), nullptr, 10);
                    first = std::min(first, v);
                    last = std::max(last, v);
                    if (in_row) row_time = static_cast<double>(v) * 1e-9;
                }
                text_of.clear();
                text.clear();
            } else if (t.name == "frame") {
                frame = -1;
            } else if (t.name == "tagged-backtrace" || t.name == "backtrace") {
                if (bt) {
                    backtraces[bt_id] = *bt;
                    if (in_row) row_bt = *bt;
                    bt = nullptr;
                }
            } else if (t.name == "row" && in_row) {
                in_row = false;
                any_row = true;
                if (mode == kFaults && row_op != "Zero Fill") continue;  // (memory that was there already: paged in, copied, cached)
                if (mode != kSamples && row_bt.empty()) continue;         // (no stack to put it on)
                pf.clear();
                for (const u32 f : row_bt) {
                    const XFrame& x = frames[f];
                    pf.push_back({x.name, x.module, x.file, x.line});
                }
                out->add(pf, row_weight, row_thread, row_thread < tids.size() ? thread_names[tids[row_thread]] : std::string_view(), row_time);
            }
            continue;
        }

        const std::string id = t.attr("id"), ref = t.attr("ref");
        if (t.name == "schema" && !in_row) {
            const std::string name = t.attr("name");
            if (name == "virtual-memory") mode = kFaults, weight_tag = "size-in-bytes";
            else if (name == "syscall") mode = kCalls, weight_tag = "duration";
            continue;
        }
        if (t.name == "row") {
            in_row = true;
            row_weight = 0;
            row_time = -1;
            row_thread = 0;
            row_bt.clear();
            row_op.clear();
        } else if (!in_row) {
            continue;
        } else if (t.name == "vm-op") {
            if (!ref.empty()) row_op = ops[ref];
            else if (!t.empty) text_of = "op:" + id, text.clear();
        } else if (t.name == "sample-time" || t.name == "start-time") {
            if (!t.empty) text_of = "time", text.clear();
        } else if (t.name == "thread" && frame < 0 && !bt) {
            const std::string key = id.empty() ? ref : id;
            if (!key.empty() && std::ranges::find(tids, key) == tids.end()) tids.push_back(key);
            row_thread = static_cast<u64>(std::ranges::find(tids, key) - tids.begin());
            if (!id.empty()) {  // its name: what it says it is, without its number and its process ("worker (0x5bf25f) (spin, pid: 84305)")
                std::string name = t.attr("fmt");
                if (const size_t cut = name.find(" (0x"); cut != std::string::npos) name.resize(cut);
                thread_names[id] = name;
            }
        } else if (t.name == weight_tag && frame < 0) {
            if (!ref.empty()) row_weight = weights[ref];
            else if (!t.empty) text_of = "weight:" + id, text.clear();
        } else if ((t.name == "tagged-backtrace" || t.name == "backtrace") && !bt) {
            if (!ref.empty()) {
                if (auto it = backtraces.find(ref); it != backtraces.end()) row_bt = it->second;
            } else if (!t.empty) {
                building.clear();
                bt = &building;
                bt_id = id;
            }
        } else if (t.name == "frame" && bt) {
            if (!ref.empty()) {
                if (auto it = frame_ids.find(ref); it != frame_ids.end()) bt->push_back(it->second);
            } else {
                frames.push_back({t.attr("name"), {}, {}, 0});
                if (frames.back().name.empty()) frames.back().name = t.attr("addr");
                frame = static_cast<int>(frames.size() - 1);
                frame_ids[id] = static_cast<u32>(frame);
                bt->push_back(static_cast<u32>(frame));
                if (t.empty) frame = -1;
            }
        } else if (t.name == "binary" && frame >= 0) {
            if (!ref.empty()) frames[static_cast<size_t>(frame)].module = binaries[ref];
            else {
                std::string path = t.attr("path");
                if (path.empty()) path = t.attr("name");
                binaries[id] = path;
                frames[static_cast<size_t>(frame)].module = path;
            }
        } else if (t.name == "source" && frame >= 0) {
            frames[static_cast<size_t>(frame)].line = static_cast<u32>(std::strtoul(t.attr("line").c_str(), nullptr, 10));
            any_line = true;
        } else if (t.name == "path" && frame >= 0) {
            if (!ref.empty()) frames[static_cast<size_t>(frame)].file = prefix + paths[ref];
            else if (!t.empty) text_of = "path:" + id, text.clear();
        }
    }
    if (!any_row || out->samples == 0) {
        if (err) *err = mode == kFaults ? "the Virtual Memory Trace saw no new memory" : mode == kCalls ? "the System Call Trace saw no calls" : "the Time Profiler recorded no samples";
        return false;
    }
    out->seconds = last > first ? static_cast<double>(last - first) * 1e-9 : 0;
    out->threads = static_cast<u32>(tids.size());
    out->unit = mode == kFaults ? "bytes" : "ns";
    if (!any_line) out->note = "No source lines: build the program with -g to see them.";
    return true;
}

const char* profiler_for(std::string_view system) { return system.starts_with("Darwin") ? "xctrace" : "perf"; }

// ---------------------------------------------------------------- recording

std::unordered_map<u64, AllocPlace> alloc_places(std::string_view text) {
    std::unordered_map<u64, AllocPlace> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.starts_with("a ")) continue;
        // "a ADDR BASE IMAGE\tSYMBOL"
        const std::string_view rest = line.substr(2);
        const size_t sp1 = rest.find(' '), sp2 = sp1 == std::string_view::npos ? sp1 : rest.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos) continue;
        const u64 addr = std::strtoull(std::string(rest.substr(0, sp1)).c_str(), nullptr, 16);
        AllocPlace p;
        p.base = std::strtoull(std::string(rest.substr(sp1 + 1, sp2 - sp1 - 1)).c_str(), nullptr, 16);
        const std::string_view tail = rest.substr(sp2 + 1);
        const size_t tab = tail.find('\t');
        p.image = tail.substr(0, tab);
        if (tab != std::string_view::npos) p.symbol = tail.substr(tab + 1);
        out.emplace(addr, std::move(p));
    }
    return out;
}

bool parse_alloc(std::string_view text, const std::unordered_map<u64, AllocName>& names, Profile* all, Profile* held, std::string* err) {
    if (!text.starts_with("blit-alloc 1")) {
        if (err) *err = "not what blit's allocation sampler writes";
        return false;
    }
    const std::unordered_map<u64, AllocPlace> places = alloc_places(text);
    u64 bytes = 0, count = 0, rate = 0;
    std::vector<std::string> keep;  // (names made here, which the frames point into)
    keep.reserve(1024);
    std::unordered_map<u64, AllocName> made;
    auto name_of = [&](u64 addr) -> const AllocName& {
        if (const auto it = names.find(addr); it != names.end()) return it->second;
        auto [it, fresh] = made.try_emplace(addr);
        if (fresh) {
            AllocName& n = it->second;
            if (const auto pl = places.find(addr); pl != places.end()) {
                n.module = pl->second.image;
                n.name = !pl->second.symbol.empty() ? pl->second.symbol : "[" + std::string(base_name(pl->second.image)) + "]";
            } else {
                n.name = std::format("0x{:x}", addr);
            }
        }
        return it->second;
    };
    std::vector<Profile::Frame> frames;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = text.substr(at, nl - at);
        at = nl + 1;
        if (line.starts_with("total ")) {
            std::sscanf(std::string(line).c_str(), "total %llu %llu", reinterpret_cast<unsigned long long*>(&bytes), reinterpret_cast<unsigned long long*>(&count));
            continue;
        }
        if (line.starts_with("rate ")) {
            rate = std::strtoull(std::string(line.substr(5)).c_str(), nullptr, 10);
            continue;
        }
        if (!line.starts_with("s ")) continue;
        // "s WEIGHT SIZE LIVE ADDR ..." (the innermost frame first)
        std::vector<std::string_view> words;
        for (size_t k = 2; k < line.size();) {
            size_t e = line.find(' ', k);
            if (e == std::string_view::npos) e = line.size();
            if (e > k) words.push_back(line.substr(k, e - k));
            k = e + 1;
        }
        if (words.size() < 3) continue;
        const u64 weight = std::strtoull(std::string(words[0]).c_str(), nullptr, 10);
        const bool live = words[2] == "1";
        frames.clear();
        for (size_t k = 3; k < words.size(); ++k) {
            const AllocName& n = name_of(std::strtoull(std::string(words[k]).c_str(), nullptr, 16));
            frames.push_back({n.name, n.module, n.file, n.line});
        }
        const std::string_view who = base_name(all->program);
        all->add(frames, weight, 0, who);
        if (live) held->add(frames, weight, 0, who);
    }
    if (all->samples == 0) {
        if (err) *err = "no allocation was sampled (" + std::to_string(count) + " allocations, " + std::to_string(bytes) + " bytes)";
        return false;
    }
    all->unit = held->unit = "bytes";
    all->note = std::format("{} allocations, {:.1f} MB; sampled every {} KB", count, static_cast<double>(bytes) / 1048576.0, rate / 1024);
    held->note = std::format("{:.1f} MB still held at the end (sampled)", static_cast<double>(held->weight) / 1048576.0);
    return true;
}

namespace {

// A program the job runs: here, or on another machine through its agent.
struct Run {
    std::shared_ptr<Remote> remote;  // null: here
    pid_t pid = -1;
    u32 rid = 0;
    int out = -1, err = -1;
    bool missing = false;  // (here: it could not be started, as no program of its name is on the PATH)
    void signal(int sig) const {
        if (remote) remote->kill(rid, sig);
        else if (pid > 0) ::kill(-pid, sig);  // (its whole group: the profiler and what it runs)
    }
};

// Has the child start in a folder (as lsp.cpp does it).
inline int spawn_chdir(posix_spawn_file_actions_t* fa, const char* dir) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
    return posix_spawn_file_actions_addchdir(fa, dir);
#else
    return posix_spawn_file_actions_addchdir_np(fa, dir);
#endif
}

// (env: more of the environment, "NAME=value", put in the program's own:
// here only, and not through a program, so that a Mac keeps DYLD_ ones.)
bool start(const std::shared_ptr<Remote>& remote, const std::vector<std::string>& argv, const std::string& cwd, Run* run, std::string* err,
           const std::vector<std::string>& env = {}) {
    run->remote = remote;
    if (remote) {
        RemoteProc p;
        p.keep = p.work = true;  // (a long run outlives a dropped connection, and counts as working)
        if (!remote->spawn(argv, cwd, {}, &p, err)) return false;
        ::close(p.in);  // (it reads nothing)
        run->rid = p.id;
        run->out = p.out;
        run->err = p.err;
        return true;
    }
    int out[2], er[2];
    if (::pipe(out) != 0) return *err = "cannot make a pipe", false;
    if (::pipe(er) != 0) {
        ::close(out[0]), ::close(out[1]);
        return *err = "cannot make a pipe", false;
    }
    for (const int fd : {out[0], er[0]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, er[1], 2);
    if (!cwd.empty()) spawn_chdir(&fa, cwd.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETPGROUP;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    std::vector<std::string> words = argv;
    std::vector<char*> args;
    for (std::string& w : words) args.push_back(w.data());
    args.push_back(nullptr);
    std::vector<std::string> vars = env;
    std::vector<char*> envp;
    for (std::string& v : vars) envp.push_back(v.data());
    for (char** e = environ; e && *e; ++e) {
        const std::string_view have(*e);
        const size_t eq = have.find('=');
        if (std::ranges::none_of(env, [&](const std::string& v) { return v.compare(0, eq + 1, have.substr(0, eq + 1)) == 0; })) envp.push_back(*e);
    }
    envp.push_back(nullptr);
    const int rc = ::posix_spawnp(&run->pid, args[0], &fa, &attr, args.data(), env.empty() ? environ : envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    ::close(out[1]), ::close(er[1]);
    if (rc != 0) {
        ::close(out[0]), ::close(er[0]);
        *err = std::string("cannot run ") + argv[0] + ": " + std::strerror(rc);
        run->missing = rc == ENOENT && argv[0].find('/') == std::string::npos;
        return false;
    }
    run->out = out[0];
    run->err = er[0];
    return true;
}

// Reads what it prints until it ends, and its exit status. (`keep`: the
// most of its output kept, its end.)
int finish(Run& run, std::string* out, std::string* err, size_t keep = std::string::npos) {
    bool out_open = true, err_open = true;
    char buf[65536];
    while (out_open || err_open) {
        pollfd fds[2] = {{out_open ? run.out : -1, POLLIN, 0}, {err_open ? run.err : -1, POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0 && errno != EINTR) break;
        auto drain = [&](int fd, std::string* into, bool& open, short rev) {
            if (!(rev & (POLLIN | POLLHUP | POLLERR))) return;
            const ssize_t k = ::read(fd, buf, sizeof buf);
            if (k > 0) {
                if (into) {
                    into->append(buf, static_cast<size_t>(k));
                    if (into->size() > keep + keep / 2) into->erase(0, into->size() - keep);
                }
            } else if (k == 0 || errno != EINTR) {
                ::close(fd);
                open = false;
            }
        };
        if (out_open) drain(run.out, out, out_open, fds[0].revents);
        if (err_open) drain(run.err, err, err_open, fds[1].revents);
    }
    if (out && out->size() > keep) out->erase(0, out->size() - keep);
    if (err && err->size() > keep) err->erase(0, err->size() - keep);
    if (run.remote) return run.remote->wait(run.rid);
    int status = 0;
    while (::waitpid(run.pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

std::string last_line(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    const size_t nl = s.rfind('\n');
    return nl == std::string::npos ? s : s.substr(nl + 1);
}

// xctrace through xcrun, with Xcode's developer folder when the one chosen
// is only the command line tools (which have no xctrace).
const std::vector<std::string> kXctrace = {
    "/bin/sh", "-c",
    "if ! xcrun --find xctrace >/dev/null 2>&1 && [ -d /Applications/Xcode.app/Contents/Developer ]; then "
    "DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer; export DEVELOPER_DIR; fi; exec xcrun xctrace \"$@\"",
    "xctrace"};

// The allocation sampler blit carries (src/alloc, built with it), and
// where it is written out to be put into a program: the temporary folder,
// under a name that changes with it.
#if defined(BLIT_ALLOC_HERE) && __has_embed(BLIT_ALLOC_HERE)
constexpr unsigned char kAllocLib[] = {
#embed BLIT_ALLOC_HERE
};
#endif

std::string alloc_library(std::string* err) {
#if defined(BLIT_ALLOC_HERE) && __has_embed(BLIT_ALLOC_HERE)
    const std::string_view bytes(reinterpret_cast<const char*>(kAllocLib), sizeof kAllocLib);
    u64 h = 1469598103934665603ull;
    for (const char c : bytes) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = tmp && *tmp ? tmp : "/tmp";
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
#if defined(__APPLE__)
    const std::string path = std::format("{}/blit-alloc-{:016x}.dylib", dir, h);
#else
    const std::string path = std::format("{}/blit-alloc-{:016x}.so", dir, h);
#endif
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) == bytes.size() && !ec) return path;
    const std::string part = path + ".part" + std::to_string(::getpid());
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return *err = "cannot write the allocation sampler to " + dir, std::string();
    const bool ok = ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
    ::close(fd);
    if (!ok || ::rename(part.c_str(), path.c_str()) != 0) return *err = "cannot write the allocation sampler to " + dir, std::string();
    return path;
#else
    *err = "this blit was built without its allocation sampler";
    return {};
#endif
}

// The sampler's addresses named, with their source lines: by atos on a Mac
// (an image's addresses at a time, with its base), by addr2line elsewhere.
// The system's own images are left to the names dladdr gave. (`missing`:
// that the one to name them with is not installed, when it is not.)
std::unordered_map<u64, AllocName> alloc_names(std::string_view text, std::string* missing) {
    std::unordered_map<u64, AllocName> out;
    std::map<std::pair<std::string, u64>, std::vector<u64>> by_image;
    for (const auto& [addr, pl] : alloc_places(text)) {
        if (pl.image.starts_with("/usr/lib") || pl.image.starts_with("/System") || pl.image.starts_with("/lib")) continue;
        by_image[{pl.image, pl.base}].push_back(addr);
    }
    for (auto& [img, addrs] : by_image) {
        const auto& [image, base] = img;
#if !defined(__APPLE__)
        // (A shared object's addresses, and a position-independent program's, are offsets from its base.)
        char head[18] = {};
        bool shared = true;
        if (FILE* f = std::fopen(image.c_str(), "rb")) {
            shared = std::fread(head, 1, sizeof head, f) == sizeof head && head[16] == 3;
            std::fclose(f);
        }
#endif
        for (size_t from = 0; from < addrs.size(); from += 400) {
            const size_t to = std::min(addrs.size(), from + 400);
#if defined(__APPLE__)
            std::vector<std::string> argv{"atos", "-fullPath", "-o", image, "-l", std::format("0x{:x}", base)};
            for (size_t k = from; k < to; ++k) argv.push_back(std::format("0x{:x}", addrs[k]));
#else
            std::vector<std::string> argv{"addr2line", "-C", "-f", "-e", image};
            for (size_t k = from; k < to; ++k) argv.push_back(std::format("0x{:x}", shared ? addrs[k] - base : addrs[k]));
#endif
            Run r;
            std::string said, ignore;
            if (!start(nullptr, argv, "", &r, &ignore)) {
                if (!r.missing) break;
                *missing = not_installed(argv[0], {});  // (nor will it be for the next image)
                return out;
            }
            finish(r, &said, &ignore);
            std::vector<std::string_view> lines;
            for (size_t at = 0; at < said.size();) {
                size_t nl = said.find('\n', at);
                if (nl == std::string::npos) nl = said.size();
                lines.push_back(std::string_view(said).substr(at, nl - at));
                at = nl + 1;
            }
            for (size_t k = from; k < to; ++k) {
                AllocName n;
                n.module = image;
#if defined(__APPLE__)
                // "name (in image) (file:line)", or "0x... (in image)": no name known
                if (k - from >= lines.size()) break;
                const std::string_view l = lines[k - from];
                const size_t in = l.find(" (in ");
                if (in == std::string_view::npos || l.starts_with("0x")) continue;
                n.name = l.substr(0, in);
                if (l.ends_with(")")) {
                    const size_t open = l.rfind('(');
                    std::string_view where = l.substr(open + 1, l.size() - open - 2);
                    std::string_view file;
                    u32 line = 0;
                    if (open > in + 4 && split_srcline(where, &file, &line)) n.file = file, n.line = line;
                }
#else
                // two lines: the function ("??": not known), then "file:line" ("??:0")
                const size_t at = (k - from) * 2;
                if (at + 1 >= lines.size()) break;
                if (lines[at] == "??") continue;
                n.name = lines[at];
                std::string_view where = lines[at + 1];
                if (const size_t d = where.find(" (discriminator"); d != std::string_view::npos) where = where.substr(0, d);
                std::string_view file;
                u32 line = 0;
                if (split_srcline(where, &file, &line)) n.file = file, n.line = line;
#endif
                out.emplace(addrs[k], std::move(n));
            }
        }
    }
    return out;
}

}  // namespace

struct ProfileJob::Impl {
    mutable std::mutex mu;
    State state = kStarting;
    std::string status = "Starting", error;
    std::shared_ptr<Profile> result, held;
    std::function<void()> notify;
    std::chrono::steady_clock::time_point started{};
    bool recording = false;
    bool reading = false;  // (the profiler's output is being read: run is that)
    Run run;  // the recording, or the reading of it, while it is under way (to stop it)
    std::atomic<bool> stop{false};
    std::atomic<bool> quit{false};  // the job is going away: no more waiting on anything
    std::atomic<bool> ended{false};  // the thread is done with everything
    std::jthread thread;

    void set(State s, std::string what) {
        {
            std::lock_guard lock(mu);
            state = s;
            status = std::move(what);
        }
        if (notify) notify();
    }
    void fail(std::string why) {
        {
            std::lock_guard lock(mu);
            state = kFailed;
            error = std::move(why);
            status = "Failed";
        }
        if (notify) notify();
    }

    // Allocations: the program run with blit's sampler in front of its
    // allocator (here only), then what the sampler wrote, named.
    template <class Clean>
    void allocations(const ProfileRequest& req, std::shared_ptr<Profile> p, const std::string& there, const std::string& cwd, const std::string& dir, bool remote, Clean& clean) {
        if (remote) {
            clean();
            return fail("allocations are recorded on this machine only");
        }
        std::string err;
        const std::string lib = alloc_library(&err);
        if (lib.empty()) {
            clean();
            return fail(err);
        }
        std::vector<std::string> env = req.env;
#if defined(__APPLE__)
        env.push_back("DYLD_INSERT_LIBRARIES=" + lib);
#else
        env.push_back("LD_PRELOAD=" + lib);
#endif
        env.push_back("BLIT_ALLOC_OUT=" + dir + "/alloc.txt");
        std::vector<std::string> argv{there};
        argv.insert(argv.end(), req.args.begin(), req.args.end());
        Run r;
        if (!start(nullptr, argv, cwd, &r, &err, env)) {
            clean();
            return fail(err);
        }
        {
            std::lock_guard lock(mu);
            run = r;
            recording = true;
            started = std::chrono::steady_clock::now();
        }
        set(kRecording, "Recording allocations");
        if (stop) r.signal(SIGINT);
        std::string output, rec_err;
        finish(r, &output, &rec_err, 64 * 1024);
        {
            std::lock_guard lock(mu);
            recording = false;
        }
        p->output = output.empty() ? rec_err : output + (rec_err.empty() ? "" : "\n" + rec_err);
        set(kReading, "Reading the allocations");
        std::string text;
        bool wrote = false;
        if (FILE* f = std::fopen((dir + "/alloc.txt").c_str(), "rb")) {
            char buf[65536];
            for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (512u << 20);) text.append(buf, n);
            std::fclose(f);
            wrote = !text.empty();
        }
        clean();
        if (quit) return fail("Stopped");
        if (!wrote) return fail("the program wrote no allocations (one the system protects, or signed to refuse it, cannot be given the sampler)");
        auto held_p = std::make_shared<Profile>(*p);
        std::string perr, missing;
        if (!parse_alloc(text, alloc_names(text, &missing), p.get(), held_p.get(), &perr)) return fail(perr);
        // (In each one's heading: its stacks named only as far as dladdr could, which on a Mac
        // finds the program's own functions, and elsewhere only those it exports.)
#if defined(__APPLE__)
        constexpr const char* kLost = ", so no source lines; ";
#else
        constexpr const char* kLost = ", so no function names or source lines; ";
#endif
        if (!missing.empty())
            for (Profile* q : {p.get(), held_p.get()}) q->note = missing + kLost + q->note;
        p->kind = "allocations";
        held_p->kind = "held";
        p->backend = held_p->backend = "blit";  // (its own sampler, not the system's profiler)
        held_p->output.clear();
        p->finish();
        held_p->finish();
        {
            std::lock_guard lock(mu);
            result = std::move(p);
            held = std::move(held_p);
        }
        set(kDone, "Done");
    }

    void work(const ProfileRequest& req) {
        // Where it runs, and with which profiler.
        std::string host, there = req.program, cwd = req.cwd;
        std::shared_ptr<Remote> remote;
        std::string system;
        if (split_remote(req.program, &host, &there)) {
            set(kStarting, "Connecting to " + host);
            remote = remote_for(req.program, &there, kRemoteWaitMs);
            if (!remote) return fail(remote_error(req.program));
            system = remote->system();
            std::string h2;
            if (!cwd.empty() && !split_remote(cwd, &h2, &cwd)) cwd.clear();
        } else {
#if defined(__APPLE__)
            system = "Darwin";
#else
            system = "Linux";
#endif
        }
        if (cwd.empty()) {
            const size_t slash = there.find_last_of('/');
            cwd = slash == std::string::npos || slash == 0 ? "/" : there.substr(0, slash);
        }
        const std::string tool = profiler_for(system);
        const std::string where = host.empty() ? "" : " on " + host;
        auto p = std::make_shared<Profile>();
        p->program = there;
        for (const std::string& a : req.args) p->args += (p->args.empty() ? "" : " ") + a;
        p->backend = tool;
        p->host = host;
        {
            const std::time_t now = std::time(nullptr);
            char when[32];
            std::tm local{};
            ::localtime_r(&now, &local);
            std::strftime(when, sizeof when, "%H:%M:%S", &local);
            p->when = when;
        }

        // A folder for what is recorded.
        std::string dir;
        if (remote) {
            Run mk;
            std::string out, err;
            if (!start(remote, {"/bin/sh", "-c", "mktemp -d \"${TMPDIR:-/tmp}/blit-profile.XXXXXX\""}, "", &mk, &err)) return fail(err);
            if (finish(mk, &out, &err) != 0) return fail("cannot make a folder" + where + ": " + last_line(err));
            dir = last_line(out);
        } else {
            const char* tmp = std::getenv("TMPDIR");
            std::string t = std::string(tmp && *tmp ? tmp : "/tmp");
            while (t.size() > 1 && t.back() == '/') t.pop_back();
            t += "/blit-profile.XXXXXX";
            if (!::mkdtemp(t.data())) return fail("cannot make a folder for the recording");
            dir = t;
        }
        auto clean = [&] {
            if (dir.empty()) return;
            if (remote) {
                Run rm;
                std::string ignore;
                if (start(remote, {"rm", "-rf", "--", dir}, "", &rm, &ignore)) finish(rm, nullptr, nullptr);
            } else {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            }
        };

        if (req.kind == ProfileRequest::kAllocations) return allocations(req, p, there, cwd, dir, remote != nullptr, clean);

        // Record.
        std::vector<std::string> argv;
        const std::string freq = std::to_string(std::clamp<u32>(req.frequency, 1, 20000));
        if (tool == "perf") {
            // Time: samples at a frequency. Memory: every page fault (where
            // memory is first touched). Waiting: every context switch, and
            // the switches themselves (when each thread ran again).
            if (req.kind == ProfileRequest::kMemory) argv = {"perf", "record", "-e", "page-faults", "-c", "1", "-g", "--user-callchains", "-o", dir + "/perf.data"};
            else if (req.kind == ProfileRequest::kWaiting) argv = {"perf", "record", "-e", "context-switches", "-c", "1", "--switch-events", "-g", "--user-callchains", "-o", dir + "/perf.data"};
            else argv = {"perf", "record", "-F", freq, "-g", "--user-callchains", "-o", dir + "/perf.data"};
            if (req.dwarf) argv.insert(std::ranges::find(argv, std::string("-g")), {"--call-graph", "dwarf"});

        } else {
            // (The two besides the Time Profiler trace the kernel, as it
            // does: neither needs Developer Mode, as Allocations would.)
            argv = kXctrace;
            const char* what = req.kind == ProfileRequest::kMemory ? "Virtual Memory Trace" : req.kind == ProfileRequest::kWaiting ? "System Call Trace" : nullptr;
            if (what) argv.insert(argv.end(), {"record", "--instrument", what});
            else argv.insert(argv.end(), {"record", "--template", "Time Profiler"});
            argv.insert(argv.end(), {"--no-prompt", "--output", dir + "/profile.trace", "--target-stdout", "-"});
            for (const std::string& e : req.env) argv.insert(argv.end(), {"--env", e});
            argv.push_back("--launch");
        }
        argv.push_back("--");
#if defined(__linux__)
        // Memory here: the program without transparent huge pages (a fault
        // of one would count as a single page), through this blit.
        if (tool == "perf" && req.kind == ProfileRequest::kMemory && !remote) argv.insert(argv.end(), {program_path(), "--exec-without-huge-pages"});
#endif
        if (tool == "perf" && !req.env.empty()) {  // (through env, which perf follows into the program)
            argv.push_back("env");
            argv.insert(argv.end(), req.env.begin(), req.env.end());
        }
        argv.push_back(there);
        argv.insert(argv.end(), req.args.begin(), req.args.end());
        std::string output, rec_err;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Run r;
            std::string err;
            if (!start(remote, argv, cwd, &r, &err)) {
                clean();
                return fail(err.find("No such file") != std::string::npos || err.find("not found") != std::string::npos
                                ? tool + " is not installed" + where
                                : err);
            }
            {
                std::lock_guard lock(mu);
                run = r;
                recording = true;
                started = std::chrono::steady_clock::now();
            }
            set(kRecording, "Recording" + where);
            if (stop) r.signal(SIGINT);
            output.clear();
            rec_err.clear();
            const int code = finish(r, &output, &rec_err, 64 * 1024);
            {
                std::lock_guard lock(mu);
                recording = false;
            }
            // An older perf has no --user-callchains: once more without it.
            if (tool == "perf" && attempt == 0 && rec_err.find("user-callchains") != std::string::npos) {
                std::erase(argv, std::string("--user-callchains"));
                continue;
            }
            if (code == 127 || (code < 0 && rec_err.empty())) {
                clean();
                return fail(code < 0 ? remote_error(req.program) : tool + " is not installed" + where);
            }
            break;
        }
        // perf and xctrace say how the recording went on the error stream;
        // the program's own output is in both. Keep it for the profile.
        p->output = output.empty() ? rec_err : output + (rec_err.empty() ? "" : "\n" + rec_err);

        // Read it.
        set(kReading, "Reading the recording" + where);
        std::vector<std::string> read;
        u64 page = 4096;  // (perf's page faults: a page's bytes each, as the machine has them)
        bool huge = false;  // (another machine's program had transparent huge pages: a fault of one counted as a page)
        if (tool == "perf") {
            read = {"perf", "script", "-i", dir + "/perf.data", "-F", "comm,tid,time,period,ip,sym,dso,srcline", "--full-source-path"};
            if (req.kind == ProfileRequest::kWaiting) read.push_back("--show-switch-events");
            if (req.kind == ProfileRequest::kMemory) {
                Run pg;
                std::string out, ignore;
                if (start(remote, {"/bin/sh", "-c", "getconf PAGESIZE; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null"}, "", &pg, &ignore) &&
                    finish(pg, &out, &ignore) == 0) {
                    if (const u64 n = std::strtoull(out.c_str(), nullptr, 10); n >= 512) page = n;
                    huge = remote && out.find("[always]") != std::string::npos;
                }
            }
        } else {
            const char* table = req.kind == ProfileRequest::kMemory ? "virtual-memory" : req.kind == ProfileRequest::kWaiting ? "syscall" : "time-profile";
            read = kXctrace;
            read.insert(read.end(), {"export", "--input", dir + "/profile.trace", "--xpath", "/trace-toc/run[@number=\"1\"]/data/table[@schema=\"" + std::string(table) + "\"]"});
        }
        Run r;
        std::string text, err;
        if (quit) {
            clean();
            return fail("Stopped");
        }
        if (!start(remote, read, cwd, &r, &err)) {
            clean();
            return fail(err);
        }
        {
            std::lock_guard lock(mu);
            run = r;
            reading = true;
            if (quit) r.signal(SIGTERM);
        }
        const int code = finish(r, &text, &err);
        {
            std::lock_guard lock(mu);
            reading = false;
        }
        clean();
        if (quit) return fail("Stopped");
        if (code != 0 && text.empty()) {
            const std::string why = last_line(rec_err.empty() ? err : rec_err);
            return fail(tool + " could not record it" + (why.empty() ? std::string() : ": " + why));
        }
        const std::string prefix = host.empty() ? std::string() : remote_prefix(host);
        std::string perr;
        bool ok;
        if (tool != "perf") {
            ok = parse_xctrace(text, prefix, p.get(), &perr);
        } else if (req.kind == ProfileRequest::kWaiting) {
            ok = parse_perf_script(perf_waits_as_samples(text), prefix, p.get(), &perr);
            if (!ok) perr = "perf saw no waits: off-CPU stacks need kernel.perf_event_paranoid at 1 or lower";
        } else {
            ok = parse_perf_script(text, prefix, p.get(), &perr, req.kind == ProfileRequest::kMemory ? page : 1);
            if (ok && req.kind == ProfileRequest::kMemory) p->unit = "bytes";
            if (ok && huge) p->note = "Transparent huge pages are always on there: a fault of a huge page counts as one page.";
        }
        p->kind = req.kind == ProfileRequest::kMemory ? "memory" : req.kind == ProfileRequest::kWaiting ? "waiting" : "time";
        if (!ok) {
            const std::string why = last_line(rec_err);
            return fail(perr + (why.empty() ? std::string() : " (" + why + ")"));
        }
        if (!host.empty()) p->program = remote_prefix(host) + there;
        p->finish();
        {
            std::lock_guard lock(mu);
            result = std::move(p);
        }
        set(kDone, "Done");
    }
};

ProfileJob::ProfileJob(ProfileRequest r, std::function<void()> notify) : req_(std::move(r)), impl_(std::make_unique<Impl>()) {
    impl_->notify = std::move(notify);
    Impl* m = impl_.get();
    const ProfileRequest* req = &req_;
    impl_->thread = std::jthread([m, req] {
        m->work(*req);
        m->ended = true;
    });
}

ProfileJob::~ProfileJob() {
    impl_->quit = true;
    stop();
    {
        // (Reading what was recorded can take long: it ends too.)
        std::lock_guard lock(impl_->mu);
        if (impl_->reading) impl_->run.signal(SIGTERM);
    }
    // A profiler that does not end when asked (xctrace stuck on a machine
    // that will not let it trace) is killed, it and what it runs, after a
    // few seconds: blit does not wait on it for ever as it quits.
    for (int i = 0; i < 500 && !impl_->ended; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!impl_->ended) {
        std::lock_guard lock(impl_->mu);
        if (impl_->recording || impl_->reading) impl_->run.signal(SIGKILL);
    }
    if (impl_->thread.joinable()) impl_->thread.join();
}

void ProfileJob::stop() {
    impl_->stop = true;
    std::lock_guard lock(impl_->mu);
    if (impl_->recording) impl_->run.signal(SIGINT);
}

ProfileJob::State ProfileJob::state() const {
    std::lock_guard lock(impl_->mu);
    return impl_->state;
}

std::string ProfileJob::status() const {
    std::lock_guard lock(impl_->mu);
    return impl_->status;
}

double ProfileJob::elapsed() const {
    std::lock_guard lock(impl_->mu);
    if (impl_->state != kRecording) return 0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->started).count();
}

std::shared_ptr<Profile> ProfileJob::result() const {
    std::lock_guard lock(impl_->mu);
    return impl_->result;
}

std::shared_ptr<Profile> ProfileJob::held() const {
    std::lock_guard lock(impl_->mu);
    return impl_->held;
}

std::string ProfileJob::error() const {
    std::lock_guard lock(impl_->mu);
    return impl_->error;
}

}  // namespace blit
