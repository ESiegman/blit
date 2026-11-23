// Profiling in the editor: the side panel's Profile tab (the program, its
// arguments and folder, recording, the profiles made and those kept beside
// the project, the threads of the one shown), a tab for each profile (a
// flame graph of all its threads or of one, and its functions by their own
// time), and the inline performance labels: at each function that took
// time, and on lines that took much (profile.hpp records and reads).
#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "profile.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

// The frames' tones (Popup::tones): 1-4 the program's own code, 5-6 the
// libraries', 7 the function chosen or under the mouse.
constexpr u32 kOwnTones = 4, kLibTones = 2, kFocusTone = 7;

// Function names as short as can still be told apart in a frame: no
// parameters, no namespaces or classes in front.
std::string_view short_name(std::string_view name) {
    int depth = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '<') ++depth;
        else if (c == '>' && depth > 0) --depth;
        else if (c == '(' && depth == 0 && i > 0) {
            name = name.substr(0, i);
            break;
        }
    }
    depth = 0;
    for (size_t i = name.size(); i-- > 0;) {
        const char c = name[i];
        if (c == '>') ++depth;
        else if (c == '<' && depth > 0) --depth;
        else if (depth == 0 && ((c == ':' && i > 0 && name[i - 1] == ':') || c == '.')) return name.substr(i + 1);
    }
    return name;
}

// The name without template arguments either: what a definition in the source says.
std::string bare_name(std::string_view name) {
    std::string s(short_name(name));
    if (const size_t lt = s.find('<'); lt != std::string::npos && lt > 0) s.resize(lt);
    return s;
}

std::string percent(u64 part, u64 whole) {
    char buf[32];
    const double p = whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0;
    std::snprintf(buf, sizeof buf, p >= 10 ? "%.0f%%" : "%.1f%%", p);
    return buf;
}

// A share (0..1) as percent; a change of share in points, signed.
std::string share_text(double f) {
    char buf[32];
    const double p = 100.0 * f;
    std::snprintf(buf, sizeof buf, p >= 10 ? "%.0f%%" : "%.1f%%", p);
    return buf;
}
std::string change_text(double d) {
    char buf[32];
    const double p = 100.0 * d;
    if (std::abs(p) < 0.05) return "0";
    std::snprintf(buf, sizeof buf, std::abs(p) >= 10 ? "%+.0f" : "%+.1f", p);
    std::string s = buf;
    if (s[0] == '-') s.replace(0, 1, "\xE2\x88\x92");  // (a minus sign)
    return s;
}

std::string with_commas(u64 n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

std::string clip(std::string_view s, u32 n) {
    if (n == 0) return {};
    if (width_of(s) <= n) return std::string(s);
    size_t at = 0;
    for (u32 k = 0; k + 1 < n && at < s.size(); ++k) at += decode_utf8(reinterpret_cast<const u8*>(s.data()) + at, s.size() - at).len;
    return std::string(s.substr(0, at)) + "\xE2\x80\xA6";
}

std::string short_path(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// A row that is picked: lit as selected while the panel has the keyboard,
// in the quieter tone of the mouse's while it does not.
void light_row(TextBox& box, u32 row, bool focused) {
    if (focused) return box.select_row(row);
    if (row >= box.rows) return;
    for (u32 c = 0; c < box.cols; ++c) box.cells[static_cast<size_t>(row) * box.cols + c] |= kCellHover;
}

u32 hash_of(std::string_view s) {
    u32 h = 2166136261u;
    for (const char c : s) h = (h ^ static_cast<u8>(c)) * 16777619u;
    return h;
}

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 13;
    SDL_PushEvent(&ev);
}

Uint32 tick(void*, SDL_TimerID, Uint32 interval) {  // (while recording: the time it has run, kept current)
    wake();
    return interval;
}

}  // namespace

// A profile's tab: what it shows, and where its parts were drawn.
struct ProfView {
    std::shared_ptr<Profile> p;     // the tree shown: orig, or (bottom-up) flipped
    std::shared_ptr<Profile> orig;  // the profile, top-down (whose weights the list has)
    std::shared_ptr<Profile> flipped;  // turned over (invert_profile), made when first asked for
    bool bottom_up = false;
    // In place of the list of functions: the chosen one's callers and
    // callees (counted in orig, under the thread shown), each with its weight.
    bool relations = false;
    struct Rel {
        u32 func = 0;
        u64 weight = 0;
        u8 kind = 0;  // 0 a heading (func: 1 the callers', 2 the callees'), 1 a caller, 2 a callee
    };
    std::vector<Rel> rel;
    u32 rel_of = ~0u;
    int rel_thread = -2;
    u32 rel_sel = 0, rel_top = 0;
    // Compared with an earlier profile (base): the difference, and the row
    // of its list chosen.
    std::shared_ptr<Profile> base;
    std::shared_ptr<ProfileDiff> diff;
    u32 dsel = 0, dtop = 0;
    // The timeline: all of the profile (orig is a stretch of it while one is
    // chosen), the stretch chosen (t0 < 0: none), a drag across it under way;
    // where its lanes are, the thread each is, and the seconds it spans.
    std::shared_ptr<Profile> full;
    double t0 = -1, t1 = -1;
    bool tl_drag = false;
    double tl_anchor = 0;
    u32 tl_row = 0, tl_rows = 0, tl_col = 0, tl_cols = 0;
    std::vector<u64> lane_ids;
    double tl_span = 0;
    double time_at(u32 col) const {
        if (!tl_cols) return 0;
        const double at = (static_cast<double>(col) - static_cast<double>(tl_col) + 0.5) / static_cast<double>(tl_cols);
        return std::clamp(at, 0.0, 1.0) * tl_span;
    }
    std::vector<u16> depth;  // of each node (a thread's root is at 0)
    std::vector<bool> is_root;  // (the roots: of all threads, and of each)
    u16 deepest = 0;
    std::vector<u32> order;  // the functions, the heaviest by their own time first
    // The thread whose graph it is (-1: all of them): its root, the weight
    // of all it ran, and (one thread's) each function's weight in it.
    int thread = -1;
    u32 root = 0;
    u64 whole = 0;
    std::vector<u64> self_w, total_w;
    u64 self(u32 f) const { return self_w.empty() ? p->funcs[f].self : self_w[f]; }
    u64 total(u32 f) const { return total_w.empty() ? p->funcs[f].total : total_w[f]; }
    std::string root_name() const { return thread < 0 ? std::string("all") : p->by_thread[static_cast<size_t>(thread)].name; }
    bool under_root(u32 n) const {  // (a node of the graph shown)
        while (n != root && !is_root[n]) n = p->nodes[n].parent;
        return n == root;
    }
    u32 zoom = 0;            // the node shown across the whole width
    int hover = -1;          // the node under the mouse
    u32 sel = 0;             // the function chosen in the list (an index into order)
    u32 top = 0;             // the list's first row on show
    u32 shift = 0;           // flame rows scrolled away at the top (a deep graph)
    // Its cells, and what they were made from.
    TextBox box;
    u64 key = 0;
    // Where things are, for the mouse.
    int x = 0, y = 0, pad_x = 0, pad_y = 0;
    u32 cols = 0, rows = 0;
    u32 flame_row = 0, flame_rows = 0, list_row = 0, list_rows = 0;
    struct Hit {
        u32 row, c0, c1, node;
    };
    std::vector<Hit> hits;
};

struct App::ProfUi {
    // What the next recording is of.
    std::string program, args, cwd;
    std::vector<std::string> env;  // ("NAME=value": a run configuration's)
    u32 frequency = 999;
    bool dwarf = false;
    bool annotate = true;
    ProfileRequest::Kind kind = ProfileRequest::kTime;  // what it records: time, memory, waiting
    std::unique_ptr<ProfileJob> job;
    SDL_TimerID timer = 0;
    std::string error;
    std::vector<std::shared_ptr<Profile>> done;  // the latest first
    std::vector<std::string> done_files;          // and the file each is kept in (beside the project)
    std::shared_ptr<Profile> shown;               // whose times are in the code
    // The profiles kept beside the project (in its .blit/profiles): the
    // folder, its files (the newest first), and what is done with them on
    // threads of their own (listing the folder, writing one, reading one),
    // handed back here.
    struct Kept {
        std::mutex mu;
        std::string folder, listed;  // (listed: the folder whose files these are)
        std::vector<std::string> files;
        int busy = 0;  // listings, saves and readings under way
        std::vector<std::pair<std::string, std::shared_ptr<Profile>>> read;  // (a file read, to be opened)
        std::string error;
    };
    std::shared_ptr<Kept> kept = std::make_shared<Kept>();
    std::string opening;  // a kept one being read
    // The panel: what each row does, and the row chosen.
    enum Act : u8 { kNone, kProgram, kArgs, kCwd, kRecord, kStop, kDwarf, kAnnotate, kOpen, kOpenKept, kThread, kKind };
    struct Row {
        Act act = kNone;
        u32 index = 0;
    };
    std::vector<Row> rows;
    u32 sel = 0;
    float tones[8][4] = {};  // the frames' colours (Popup::tones)
    float diff_tones[8][4] = {};  // and a comparison's: grown, the same, shrunk
    // The inline performance labels, for each document: made again when its text,
    // the profile shown or the font change.
    struct Notes {
        u64 rev = ~0ull, font = ~0ull;
        const Profile* p = nullptr;
        std::vector<std::pair<u64, std::vector<u32>>> lines;  // line (from 0), cells
        std::vector<char> kinds;                               // 'F' a function's line, 'L' a hot line
    };
    std::unordered_map<u64, Notes> notes;
    // Where each document's file is in the profile shown (looked for once, not each frame).
    struct Where {
        std::string raw, full;  // the path the document has, and its full path
        const std::map<u32, ProfileLine>* lines = nullptr;  // the file's lines in the profile shown; none: it has none
        bool known = false;
    };
    std::unordered_map<u64, Where> where;
    std::vector<GridNote> view_notes[kMaxViews];
    ~ProfUi() {
        if (timer) SDL_RemoveTimer(timer);
    }
};

void App::prof_start() { profui_ = std::make_shared<ProfUi>(); }

// A run configuration chosen: what it runs is what is recorded.
void App::prof_take(const std::string& program, const std::string& args, const std::string& cwd, const std::vector<std::string>& env) {
    ProfUi& u = *profui_;
    u.program = program;
    u.args = args;
    u.cwd = cwd;
    u.env = env;
    dirty_ = true;
}

std::string App::prof_target() const {
    const ProfUi& u = *profui_;
    auto last = [](const std::string& path) { return path.substr(path.find_last_of('/') + 1); };  // (a file's name, a folder's)
    std::string out = last(u.program) + (u.args.empty() ? "" : " " + u.args) + " in " + last(u.cwd);
    for (const std::string& e : u.env) out += " " + e;
    return out;
}

// ---------------------------------------------------------------- recording

void App::prof_record(bool built) {
    ProfUi& u = *profui_;
    if (u.job || u.program.empty()) return;
    if (!built) return build_first("Record", [this] { prof_record(true); });  // (the folder's build first, where it has one)
    ProfileRequest r;
    r.program = u.program;
    r.cwd = u.cwd;
    r.env = u.env;
    r.frequency = u.frequency;
    r.dwarf = u.dwarf;
    r.kind = u.kind;
    // The arguments as a shell would split them (quotes keep spaces in).
    std::string word;
    bool any = false;
    char quote = 0;
    for (const char c : u.args) {
        if (quote) {
            if (c == quote) quote = 0;
            else word += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            any = true;
        } else if (c == ' ' || c == '\t') {
            if (any || !word.empty()) r.args.push_back(std::move(word));
            word.clear();
            any = false;
        } else {
            word += c;
        }
    }
    if (any || !word.empty()) r.args.push_back(std::move(word));
    u.error.clear();
    u.job = std::make_unique<ProfileJob>(std::move(r), wake);
    if (!u.timer) u.timer = SDL_AddTimer(250, tick, nullptr);
    ++prof_gen_;
    dirty_ = true;
}

void App::prof_stop() {
    if (profui_->job) profui_->job->stop();
}

namespace {

// A profile's file's name: the program, and when it was kept.
std::string kept_name(const Profile& p) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    char when[40];
    std::strftime(when, sizeof when, "%Y-%m-%d-%H%M%S", &local);
    std::string prog = short_path(p.program);
    for (char& c : prog)
        if (c == ' ' || c == '/') c = '_';
    return prog + (p.kind == "time" || p.kind.empty() ? "" : "-" + p.kind) + "-" + when + ".blitprofile";
}

// What a profile is of, as its tab and the panel say it ("Profile" for
// time on the CPU).
std::string kind_title(const Profile& p) {
    return p.kind == "memory" ? "Memory" : p.kind == "waiting" ? "Waiting" : p.kind == "allocations" ? "Allocations" : p.kind == "held" ? "Held at the end" : "Profile";
}

// The time a kept profile's name says it was kept, to sort them by ("2026-11-16-230501").
std::string kept_when(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot != std::string::npos && dot >= 17 ? name.substr(dot - 17, 17) : name;
}

// A kept profile's name as the panel shows it: the program (and what it measured), and when.
std::string kept_shown(const std::string& name) {
    const std::string when = kept_when(name);
    std::string prog = name.substr(0, std::min(name.size(), name.size() > when.size() + 13 ? name.size() - when.size() - 13 : name.size()));
    if (when.size() == 17) return prog + "  " + when.substr(0, 10) + " " + when.substr(11, 2) + ":" + when.substr(13, 2) + ":" + when.substr(15, 2);
    return name;
}

}  // namespace

// The folder profiles are kept in: the .blit/profiles of the folder being
// worked in, or of the program's (none: nowhere to keep them).
std::string App::prof_kept_folder() const {
    const std::string* root = panel_root();
    const std::string dir = root ? *root : profui_->program.empty() ? std::string() : parent_of(profui_->program);
    return dir.empty() ? std::string() : dir + "/.blit/profiles";
}

// The kept profiles' folder listed again (on a thread), if it is another one now.
void App::prof_list_kept() {
    ProfUi& u = *profui_;
    const std::string folder = prof_kept_folder();
    std::shared_ptr<ProfUi::Kept> k = u.kept;
    {
        std::lock_guard lock(k->mu);
        if (folder == k->folder) return;
        k->folder = folder;
        k->files.clear();
        if (folder.empty()) return;
        ++k->busy;
    }
    std::thread([k, folder] {
        std::vector<std::string> files;
        for (const FsEntry& e : fs_list(folder))
            if (!e.dir && e.name.ends_with(".blitprofile")) files.push_back(e.name);
        std::ranges::sort(files, [](const std::string& a, const std::string& b) { return kept_when(a) > kept_when(b); });
        {
            std::lock_guard lock(k->mu);
            if (k->folder == folder) {
                k->files = std::move(files);
                k->listed = folder;
            }
            --k->busy;
        }
        wake();
    }).detach();
}

// A profile just made, kept beside the project (written on a thread).
void App::prof_keep(const std::shared_ptr<Profile>& p) {
    ProfUi& u = *profui_;
    const std::string folder = prof_kept_folder();
    if (folder.empty()) return;
    const std::string name = kept_name(*p);
    u.done_files.insert(u.done_files.begin(), folder + "/" + name);
    std::shared_ptr<ProfUi::Kept> k = u.kept;
    {
        std::lock_guard lock(k->mu);
        ++k->busy;
    }
    std::thread([k, p, folder, name] {
        std::string err;
        const bool ok = fs_mkdirs(folder, &err) && fs_write(folder + "/" + name, p->to_json(), &err);
        {
            std::lock_guard lock(k->mu);
            if (ok && k->listed == folder && std::ranges::find(k->files, name) == k->files.end()) k->files.insert(k->files.begin(), name);
            if (!ok) k->error = "cannot keep the profile: " + err;
            --k->busy;
        }
        wake();
    }).detach();
}

// A kept profile read (on a thread), and opened when it has been.
void App::prof_open_kept(const std::string& path) {
    ProfUi& u = *profui_;
    for (size_t i = 0; i < u.done_files.size() && i < u.done.size(); ++i)
        if (u.done_files[i] == path) return prof_open(u.done[i]);  // (read already, or made here)
    if (!u.opening.empty()) return;
    u.opening = path;
    std::shared_ptr<ProfUi::Kept> k = u.kept;
    {
        std::lock_guard lock(k->mu);
        ++k->busy;
    }
    std::thread([k, path] {
        std::string bytes, why;
        auto p = std::make_shared<Profile>();
        const bool ok = fs_read_whole(path, &bytes, &why, 1ull << 30, std::stop_token()) && Profile::from_json(bytes, p.get(), &why);
        {
            std::lock_guard lock(k->mu);
            if (ok) k->read.emplace_back(path, std::move(p));
            else k->error = "cannot read " + path.substr(path.find_last_of('/') + 1) + ": " + why, k->read.emplace_back(path, nullptr);
            --k->busy;
        }
        wake();
    }).detach();
}

// The recording has news: it has finished (its tab opens), or failed. And
// so may the kept profiles: a file read (its tab opens), or one that failed.
void App::prof_sync() {
    ProfUi& u = *profui_;
    ++prof_gen_;
    dirty_ = true;
    {
        std::vector<std::pair<std::string, std::shared_ptr<Profile>>> read;
        std::string error;
        {
            std::lock_guard lock(u.kept->mu);
            read.swap(u.kept->read);
            error.swap(u.kept->error);
        }
        if (!error.empty()) u.error = error;
        for (auto& [path, p] : read) {
            if (u.opening == path) u.opening.clear();
            if (!p) continue;
            u.done.insert(u.done.begin(), p);
            u.done_files.insert(u.done_files.begin(), path);
            prof_open(p);
        }
    }
    if (!u.job) return;
    const ProfileJob::State st = u.job->state();
    if (st != ProfileJob::kDone && st != ProfileJob::kFailed) return;
    if (u.timer) {
        SDL_RemoveTimer(u.timer);
        u.timer = 0;
    }
    if (st == ProfileJob::kFailed) {
        u.error = u.job->error();
        u.job.reset();
        tell(SDL_MESSAGEBOX_ERROR, ("Profiling failed: " + u.error).c_str());
        return;
    }
    std::shared_ptr<Profile> p = u.job->result();
    std::shared_ptr<Profile> held = u.job->held();
    u.job.reset();
    if (!p) return;
    if (held) {  // (allocations: what was still held at the end too, in a tab behind)
        u.done.insert(u.done.begin(), held);
        prof_keep(held);
        prof_open(held);
    }
    u.done.insert(u.done.begin(), p);
    prof_keep(p);
    if (u.done.size() > 20) u.done.resize(20);
    u.done_files.resize(std::min(u.done_files.size(), u.done.size()));
    prof_open(p);
}

// The graph and the list as they were in the stretch of time chosen (all
// of it, with none chosen); the function chosen stays chosen.
void App::prof_narrow(ProfView& v) {
    const u32 focus = v.sel < v.order.size() ? v.order[v.sel] : ~0u;
    v.orig = v.t0 < 0 ? v.full : std::make_shared<Profile>(narrow_profile(*v.full, v.t0, v.t1));
    v.flipped = v.bottom_up ? std::make_shared<Profile>(invert_profile(*v.orig)) : nullptr;
    v.p = v.bottom_up ? v.flipped : v.orig;
    prof_view_init(v);
    prof_thread(v, v.thread);
    for (u32 k = 0; k < v.order.size(); ++k)
        if (v.order[k] == focus) v.sel = k;
    v.rel_of = ~0u;  // (counted again)
    v.key = 0;
    dirty_ = true;
}

// The profile tab in front narrowed to a stretch of time (t0 < 0: all of it). For the tests.
void App::prof_set_stretch(double t0, double t1) {
    if (!pane_->doc->prof) return;
    ProfView& v = *pane_->doc->prof;
    v.t0 = t0 < 0 ? -1 : t0, v.t1 = t0 < 0 ? -1 : t1;
    prof_narrow(v);
}

// A drag across the timeline let go of: that stretch, or (no more than a
// click) all of it again.
void App::prof_mouse_up() {
    if (!profui_) return;
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->prof || !pane->doc->prof->tl_drag) continue;
        ProfView& v = *pane->doc->prof;
        v.tl_drag = false;
        if (v.tl_cols == 0 || v.t1 - v.t0 < v.tl_span / static_cast<double>(v.tl_cols)) v.t0 = v.t1 = -1;
        prof_narrow(v);
    }
}

// Two profiles compared in a tab of their own: the later's flame graph,
// each frame coloured by how its share moved since the earlier, and each
// function's share before and after.
void App::prof_compare(std::shared_ptr<Profile> before, std::shared_ptr<Profile> after) {
    Doc* d = add_doc(pane_->tab + 1);
    d->title = "Compared: " + short_path(after->program) + " " + before->when + " \xE2\x86\x92 " + after->when;
    d->read_only = true;
    auto v = std::make_shared<ProfView>();
    v->p = v->orig = v->full = after;
    v->base = std::move(before);
    v->diff = std::make_shared<ProfileDiff>(diff_profiles(*v->base, *after));
    prof_view_init(*v);
    if (!v->diff->rows.empty())  // (the function chosen is the list's first, the one that moved most)
        for (u32 j = 0; j < v->order.size(); ++j)
            if (v->order[j] == v->diff->rows[0].func) v->sel = j;
    d->prof = v;
    tab_show(pane_->tab + 1);
    panel_focus(false);
    ++prof_gen_;
    dirty_ = true;
}

// With a profile's tab in front: another of those made or opened, chosen,
// as what it is compared with (the earlier).
void App::prof_compare_pick() {
    if (!profui_ || !pane_->doc->prof || pane_->doc->prof->diff) return notify("A profile's tab is what is compared");
    ProfUi& u = *profui_;
    const std::shared_ptr<Profile> after = pane_->doc->prof->orig;
    std::vector<std::shared_ptr<Profile>> others;
    std::vector<std::string> items;
    for (const std::shared_ptr<Profile>& p : u.done)
        if (p != after) {
            others.push_back(p);
            items.push_back(kind_title(*p) + ": " + short_path(p->program) + " " + p->when + (p->args.empty() ? std::string() : "  " + p->args));
        }
    if (others.empty()) return notify("No other profile to compare with (make one, or open one kept)");
    find_choose_item("Compare with", std::move(items), [this, others, after](int i) {
        if (i >= 0 && static_cast<size_t>(i) < others.size()) prof_compare(others[static_cast<size_t>(i)], after);
    });
}

// A profile made elsewhere (read from a file, for the tests): among those
// made, and in a tab.
void App::prof_take_profile(std::shared_ptr<Profile> p) {
    p->finish();
    ProfUi& u = *profui_;
    u.done.insert(u.done.begin(), p);
    prof_open(std::move(p));
}

// A profile in a tab of its own (the one it has already, if it has one).
// Its times show in the code from now on.
void App::prof_open(std::shared_ptr<Profile> p) {
    ProfUi& u = *profui_;
    u.shown = p;
    u.notes.clear();
    u.where.clear();
    panel_focus(false);  // (the profile's tab, in front, has the keyboard: the panel's Profile tab had it to start the recording)
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (size_t i = 0; i < pane->docs.size(); ++i)
            if (pane->docs[i]->prof && pane->docs[i]->prof->orig == p) {
                focus_pane(pane.get());
                tab_show(i);
                return;
            }
    Doc* d = add_doc(pane_->tab + 1);
    d->title = kind_title(*p) + ": " + short_path(p->program) + " " + p->when;
    d->read_only = true;
    auto v = std::make_shared<ProfView>();
    v->p = v->orig = v->full = p;
    prof_view_init(*v);
    d->prof = v;
    tab_show(pane_->tab + 1);
    ++prof_gen_;
    dirty_ = true;
}

// ---------------------------------------------------------------- the panel's tab

void App::prof_panel_draw(TextBox& box, bool focused) {
    ProfUi& u = *profui_;
    Font& font = *font_;
    const u32 cols = box.cols, rows = box.rows;
    u.rows.assign(rows, {});
    // Where it runs, and with what.
    std::string host, there;
    const bool remote = split_remote(u.program, &host, &there);
    std::string tool;
    if (remote) {
        for (const std::shared_ptr<Remote>& r : Remote::all())
            if (r->host() == host && r->state() == Remote::kReady) tool = profiler_for(r->system());
    } else {
#if defined(__APPLE__)
        tool = "xctrace";
#else
        tool = "perf";
#endif
    }
    const std::string by = u.kind == ProfileRequest::kAllocations ? std::string("blit's sampler") : tool.empty() ? std::string("Profiler") : tool == "perf" ? "perf" : "xctrace";
    put(box, font, 1, 0, fit_text(by + (remote ? " on " + host : ", local"), cols), ink::kMuted);
    u32 r = 2;
    // A field: its heading, and under it its value, or (quietly) what it is when not set.
    auto field = [&](const char* label, const std::string& value, const char* none, ProfUi::Act act) {
        if (r + 1 >= rows) return;
        put(box, font, r, 0, label, ink::kHeading);
        u.rows[r] = {act, 0};
        u.rows[r + 1] = {act, 0};
        std::string shown = value;
        if (const char* home = SDL_getenv("HOME"); home && *home && shown.starts_with(home)) shown = "~" + shown.substr(std::strlen(home));
        const u32 room = cols > 2 ? cols - 2 : 1;
        if (value.empty()) put(box, font, r + 1, 2, fit_text(none, room), ink::kMuted);
        else put(box, font, r + 1, 2, fit_text(shown, room, true), ink::kText);  // (a long path loses its start)
        r += 2;
    };
    field("Program", u.program, "Choose a program\xE2\x80\xA6", ProfUi::kProgram);
    field("Arguments", u.args, "None", ProfUi::kArgs);
    field("Working directory", u.cwd, "The program's folder", ProfUi::kCwd);
    if (r < rows) {  // what is recorded: chosen in turn
        u.rows[r] = {ProfUi::kKind, 0};
        const u32 c = put(box, font, r, 0, "Record", ink::kText);
        put(box, font, r++, c + 2,
            u.kind == ProfileRequest::kMemory ? "Memory first touched" : u.kind == ProfileRequest::kWaiting ? "Time waiting" : u.kind == ProfileRequest::kAllocations ? "Every allocation" : "Time on the CPU",
            ink::kMuted);
    }
    ++r;
    if (r < rows) {  // record, or stop: the button, with its key on the right
        const bool running = u.job != nullptr;
        u.rows[r] = {running ? ProfUi::kStop : ProfUi::kRecord, 0};
        const u32 room = cols > 3 ? cols - 3 : 1;
        u32 c = put(box, font, r, 0, running ? "\xE2\x96\xA0 Stop" : "\xE2\x97\x8F Record", running ? ink::kText : ink::kAccentStrong);
        std::string after;
        if (running) {
            char t[64];
            std::snprintf(t, sizeof t, "%.1f s", u.job->elapsed());
            after = u.job->state() == ProfileJob::kRecording ? std::string(t) : u.job->status() + "\xE2\x80\xA6";
        } else if (u.program.empty()) {
            after = "choose a program first";
        }
        if (!after.empty() && c + 4 < room) put(box, font, r, c + 2, fit_text(after, room - c - 2), ink::kMuted);
        if (cols > 3) put(box, font, r, cols - 1, "r", ink::kMuted);
        ++r;
    }
    if (!u.error.empty() && r < rows) put(box, font, r++, 0, fit_text(u.error, cols), ink::kError);
    ++r;
    if (r < rows) {
        u.rows[r] = {ProfUi::kDwarf, 0};
        // (How perf walks the stack: frame pointers, or DWARF for code built without them.)
        const u32 c = put(box, font, r, 0, "Call stack", ink::kText);
        put(box, font, r++, c + 2, u.dwarf ? "DWARF" : "Frame pointers", ink::kMuted);
    }
    if (r < rows) {
        u.rows[r] = {ProfUi::kAnnotate, 0};
        put(box, font, r++, 0, fit_text(std::string(u.annotate ? "\xE2\x97\x89" : "\xE2\x97\x8B") + " Inline performance labels", cols), u.annotate ? ink::kText : ink::kMuted);
    }
    ++r;
    // The profiles: those made (or read) since blit started, then those kept
    // beside the project that have not been read yet.
    prof_list_kept();
    std::vector<std::string> kept;
    std::string folder;
    {
        std::lock_guard lock(u.kept->mu);
        folder = u.kept->listed;
        for (const std::string& f : u.kept->files)
            if (std::ranges::find(u.done_files, folder + "/" + f) == u.done_files.end()) kept.push_back(f);
    }
    if (r < rows && (!u.done.empty() || !kept.empty())) {
        const u32 c = put(box, font, r, 0, "Profiles", ink::kHeading);
        put(box, font, r++, c + 1, std::to_string(u.done.size() + kept.size()), ink::kMuted);
    }
    for (u32 i = 0; i < u.done.size() && r < rows; ++i, ++r) {
        const Profile& p = *u.done[i];
        u.rows[r] = {ProfUi::kOpen, i};
        const bool on = u.shown == u.done[i];
        if (on) put(box, font, r, 0, "\xE2\x96\xB8", ink::kAccent);  // (the one shown)
        const u32 c = put(box, font, r, 2, fit_text(short_path(p.program), cols > 2 ? cols - 2 : 1), on ? ink::kStrong : ink::kText);
        char t[96];
        if (p.kind == "memory") std::snprintf(t, sizeof t, "%s  memory %.1f MB", p.when.c_str(), static_cast<double>(p.weight) / 1048576.0);
        else if (p.kind == "allocations") std::snprintf(t, sizeof t, "%s  allocated %.1f MB", p.when.c_str(), static_cast<double>(p.weight) / 1048576.0);
        else if (p.kind == "held") std::snprintf(t, sizeof t, "%s  held %.1f MB", p.when.c_str(), static_cast<double>(p.weight) / 1048576.0);
        else if (p.kind == "waiting") std::snprintf(t, sizeof t, "%s  waiting %.2f s", p.when.c_str(), static_cast<double>(p.weight) * 1e-9);
        else std::snprintf(t, sizeof t, "%s  %.1f s", p.when.c_str(), p.seconds);
        if (c + 4 < cols) put(box, font, r, c + 2, fit_text(t, cols - c - 2), ink::kMuted);
    }
    for (u32 i = 0; i < kept.size() && r < rows; ++i, ++r) {
        u.rows[r] = {ProfUi::kOpenKept, i};
        const std::string path = folder + "/" + kept[i];
        const u32 c = put(box, font, r, 2, fit_text(kept_shown(kept[i]), cols > 2 ? cols - 2 : 1), ink::kMuted);
        if (u.opening == path && c + 4 < cols) put(box, font, r, c + 2, "Opening\xE2\x80\xA6", ink::kMuted);
    }
    // The threads of the profile shown, when it has more than one: the graph of all, or of one.
    const ProfView* view = nullptr;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::shared_ptr<Doc>& d : pane->docs)
            if (d->prof && d->prof->orig == u.shown) view = d->prof.get();
    if (view && view->p->by_thread.size() > 1) {
        const Profile& p = *view->p;
        ++r;
        if (r < rows) {
            const u32 c = put(box, font, r, 0, "Threads", ink::kHeading);
            put(box, font, r++, c + 1, std::to_string(p.by_thread.size()), ink::kMuted);
        }
        for (u32 i = 0; i <= p.by_thread.size() && r < rows; ++i, ++r) {
            u.rows[r] = {ProfUi::kThread, i};
            const bool on = view->thread == static_cast<int>(i) - 1;
            if (on) put(box, font, r, 0, "\xE2\x96\xB8", ink::kAccent);
            const std::string name = i == 0 ? std::string("All threads") : p.by_thread[i - 1].name;
            const u32 c = put(box, font, r, 2, fit_text(name, cols > 2 ? cols - 2 : 1), on ? ink::kStrong : ink::kText);
            if (i > 0 && c + 6 < cols) put(box, font, r, c + 2, percent(p.by_thread[i - 1].weight, p.weight), ink::kMuted);
        }
    }
    // The row chosen (with the keyboard): the nearest one that does something.
    if (u.sel >= rows || u.rows[u.sel].act == ProfUi::kNone) {
        u.sel = 0;
        for (u32 k = 0; k < rows; ++k)
            if (u.rows[k].act != ProfUi::kNone) {
                u.sel = k;
                break;
            }
    }
    if (u.sel < rows && u.rows[u.sel].act != ProfUi::kNone) {
        const ProfUi::Act a = u.rows[u.sel].act;
        for (u32 k = 0; k < rows; ++k)  // (both rows of a field)
            if (u.rows[k].act == a && u.rows[k].index == u.rows[u.sel].index && (k == u.sel || (a == ProfUi::kProgram || a == ProfUi::kArgs || a == ProfUi::kCwd))) light_row(box, k, focused);
    }
}

// What a row of the panel does.
void App::prof_do(u32 row) {
    ProfUi& u = *profui_;
    if (row >= u.rows.size()) return;
    const ProfUi::Row it = u.rows[row];
    u.sel = row;
    ++prof_gen_;
    dirty_ = true;
    switch (it.act) {
        case ProfUi::kProgram: {
            // The box that opens files, choosing a program (on another machine too: ssh://).
            const std::string* root = panel_root();
            std::string start = u.program.empty() ? (root ? *root : default_workdir()) + "/" : u.program;
            if (const char* home = SDL_getenv("HOME"); home && *home && start.starts_with(home)) start = "~" + start.substr(std::strlen(home));
            find_choose(kFindOpen, "Program", start, [this](const std::string& path) {
                ProfUi& v = *profui_;
                v.program = path;
                v.cwd.clear();
                v.error.clear();
                ++prof_gen_;
                dirty_ = true;
            });
            break;
        }
        case ProfUi::kArgs:
            find_choose(kFindInput, "Arguments", u.args, [this](const std::string& typed) {
                profui_->args = typed;
                ++prof_gen_;
                dirty_ = true;
            });
            break;
        case ProfUi::kCwd: {
            std::string start = u.cwd.empty() ? (u.program.empty() ? default_workdir() : parent_of(u.program)) + "/" : u.cwd + "/";
            if (const char* home = SDL_getenv("HOME"); home && *home && start.starts_with(home)) start = "~" + start.substr(std::strlen(home));
            find_choose(kFindFolder, "Working directory", start, [this](const std::string& dir) {
                profui_->cwd = dir;
                ++prof_gen_;
                dirty_ = true;
            });
            break;
        }
        case ProfUi::kRecord: prof_record(); break;
        case ProfUi::kStop: prof_stop(); break;
        case ProfUi::kDwarf: u.dwarf = !u.dwarf; break;
        case ProfUi::kKind: u.kind = static_cast<ProfileRequest::Kind>((u.kind + 1) % 4); break;
        case ProfUi::kAnnotate:
            u.annotate = !u.annotate;
            for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
            break;
        case ProfUi::kOpen:
            if (it.index < u.done.size()) prof_open(u.done[it.index]);
            break;
        case ProfUi::kOpenKept: {  // (the nth of those not read yet, as drawn)
            std::string path;
            {
                std::lock_guard lock(u.kept->mu);
                u32 n = 0;
                for (const std::string& f : u.kept->files) {
                    const std::string full = u.kept->listed + "/" + f;
                    if (std::ranges::find(u.done_files, full) != u.done_files.end()) continue;
                    if (n++ == it.index) path = full;
                }
            }
            if (!path.empty()) prof_open_kept(path);
            break;
        }
        case ProfUi::kThread:  // the graph of all the threads, or of one, in the tab of the profile shown
            for (const std::unique_ptr<Pane>& pane : panes_)
                for (const std::shared_ptr<Doc>& d : pane->docs)
                    if (d->prof && d->prof->orig == u.shown) prof_thread(*d->prof, static_cast<int>(it.index) - 1);
            break;
        case ProfUi::kNone: break;
    }
}

void App::prof_panel_click(int row, int col, const SDL_MouseButtonEvent& b) {
    (void)col;
    if (b.button != SDL_BUTTON_LEFT || row < 0) return;
    prof_do(static_cast<u32>(row));
}

bool App::prof_panel_key(const SDL_KeyboardEvent& k) {
    ProfUi& u = *profui_;
    const u32 n = static_cast<u32>(u.rows.size());
    auto step = [&](int by) {  // to the next row that does something (a field is one stop)
        u32 at = u.sel;
        const ProfUi::Row from = at < n ? u.rows[at] : ProfUi::Row{};
        for (u32 tries = 0; tries < n; ++tries) {
            at = static_cast<u32>((static_cast<int>(at) + by + static_cast<int>(n)) % static_cast<int>(n));
            if (u.rows[at].act != ProfUi::kNone && (u.rows[at].act != from.act || u.rows[at].index != from.index)) break;
        }
        u.sel = at;
    };
    switch (k.key) {
        case SDLK_UP: step(-1); break;
        case SDLK_DOWN: step(1); break;
        case SDLK_HOME:  // the first row that does something, or the last
        case SDLK_END:
            if (!n) return false;
            u.sel = k.key == SDLK_HOME ? n - 1 : 0;
            step(k.key == SDLK_HOME ? 1 : -1);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: prof_do(u.sel); return true;
        case SDLK_R:
            if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT)) return false;
            if (u.job) prof_stop();
            else prof_record();
            break;
        default: return false;
    }
    ++prof_gen_;
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- a profile's tab

// The graph of one thread (or, -1, of all of them): zoomed out to its root,
// its functions by their own time in it.
// The tree shown taken in: each node's depth, the roots (of all threads and
// of each), the deepest; the functions in order.
void App::prof_view_init(ProfView& v) {
    const Profile& p = *v.p;
    v.depth.assign(p.nodes.size(), 0);
    v.is_root.assign(p.nodes.size(), false);
    v.is_root[0] = true;
    v.deepest = 0;
    for (const ProfileThread& t : p.by_thread) v.is_root[t.root] = true;
    for (u32 n = 1; n < p.nodes.size(); ++n) {  // (a node comes after its parent)
        if (v.is_root[n]) continue;
        v.depth[n] = static_cast<u16>(v.depth[p.nodes[n].parent] + 1);
        v.deepest = std::max(v.deepest, v.depth[n]);
    }
    v.order = v.orig->by_self();
    v.whole = v.orig->weight;
}

// The graph turned over (bottom-up), or back; the thread and the function
// chosen stay as they were.
void App::prof_flip(ProfView& v) {
    const u32 focus = v.sel < v.order.size() ? v.order[v.sel] : ~0u;
    v.bottom_up = !v.bottom_up;
    if (v.bottom_up && !v.flipped) v.flipped = std::make_shared<Profile>(invert_profile(*v.orig));
    v.p = v.bottom_up ? v.flipped : v.orig;
    prof_view_init(v);
    prof_thread(v, v.thread);
    for (u32 k = 0; k < v.order.size(); ++k)
        if (v.order[k] == focus) v.sel = k;
}

// The chosen function's callers and callees, with their weights: each place
// it is in the top-down tree (under the thread shown) gives what called it
// there and what it called, as much as they weighed there.
void App::prof_relations(ProfView& v) {
    const Profile& o = *v.orig;
    const u32 focus = v.sel < v.order.size() ? v.order[v.sel] : ~0u;
    v.rel_of = focus;
    v.rel_thread = v.thread;
    v.rel.clear();
    v.rel_sel = v.rel_top = 0;
    if (focus == ~0u) return;
    const u32 root = v.thread < 0 ? 0 : o.by_thread[static_cast<size_t>(v.thread)].root;
    std::vector<bool> roots(o.nodes.size(), false), under(o.nodes.size(), false);
    roots[0] = true;
    for (const ProfileThread& t : o.by_thread) roots[t.root] = true;
    under[root] = true;
    std::map<u32, u64> callers, callees;
    for (u32 n = 1; n < o.nodes.size(); ++n) {
        if (roots[n]) continue;
        under[n] = under[o.nodes[n].parent];
        if (!under[n] || o.nodes[n].func != focus) continue;
        if (const u32 up = o.nodes[n].parent; !roots[up]) callers[o.nodes[up].func] += o.nodes[n].total;
        for (const u32 k : o.nodes[n].kids) callees[o.nodes[k].func] += o.nodes[k].total;
    }
    auto add = [&](const std::map<u32, u64>& m, u8 kind) {
        v.rel.push_back({kind, 0, 0});  // (its heading: which it is, in func)
        std::vector<ProfView::Rel> rows;
        for (const auto& [f, w] : m) rows.push_back({f, w, kind});
        std::ranges::sort(rows, [](const ProfView::Rel& a, const ProfView::Rel& b) { return a.weight > b.weight; });
        v.rel.insert(v.rel.end(), rows.begin(), rows.end());
    };
    add(callers, 1);
    add(callees, 2);
    while (v.rel_sel < v.rel.size() && v.rel[v.rel_sel].kind == 0) ++v.rel_sel;
}

void App::prof_thread(ProfView& v, int i) {
    if (v.diff) i = -1;  // (a comparison is of all of it)
    const Profile& p = *v.p;
    const Profile& o = *v.orig;
    v.thread = i >= 0 && i < static_cast<int>(p.by_thread.size()) ? i : -1;
    v.root = v.zoom = v.thread < 0 ? 0 : p.by_thread[static_cast<size_t>(v.thread)].root;
    v.hover = -1;
    v.sel = v.top = v.shift = 0;
    if (v.thread < 0) {
        v.self_w.clear(), v.total_w.clear();
        v.order = o.by_self();
        v.whole = o.weight;
    } else {
        // (The weights are the top-down tree's, whichever is shown.)
        const u32 oroot = o.by_thread[static_cast<size_t>(v.thread)].root;
        o.weights_under(oroot, &v.self_w, &v.total_w);
        v.whole = o.nodes[oroot].total;
        v.order.clear();
        for (u32 f = 1; f < p.funcs.size(); ++f)
            if (v.total_w[f]) v.order.push_back(f);
        std::ranges::sort(v.order, [&](u32 a, u32 b) { return v.self_w[a] != v.self_w[b] ? v.self_w[a] > v.self_w[b] : v.total_w[a] > v.total_w[b]; });
    }
    v.key = 0;
    ++prof_gen_;
    dirty_ = true;
}

namespace {

// The frames' colours: warm for the program's own code, cool for the
// libraries', each a hue mixed into the background so that the text on it
// reads in a light theme and a dark one.
void make_colours(const Theme& t, float out[8][4]) {
    static const float kHue[8][3] = {
        {0, 0, 0}, {0.93f, 0.40f, 0.28f}, {0.96f, 0.58f, 0.22f}, {0.86f, 0.33f, 0.42f}, {0.92f, 0.70f, 0.30f}, {0.44f, 0.58f, 0.78f}, {0.50f, 0.66f, 0.66f}, {0, 0, 0},
    };
    const float* accent = t.palette[kInkAccent];
    for (u32 i = 1; i < 8; ++i) {
        const float* h = i == kFocusTone ? accent : kHue[i];
        for (int c = 0; c < 3; ++c) out[i][c] = t.bg[c] + (h[c] - t.bg[c]) * (i == kFocusTone ? 0.6f : 0.52f);
        out[i][3] = 1;
    }
}

// A comparison's frames: 1 grown much, 2 grown, 3 about the same, 4 shrunk,
// 5 shrunk much (and new: as grown much); 7 the one chosen.
void make_diff_colours(const Theme& t, float out[8][4]) {
    static const float kHue[8][3] = {
        {0, 0, 0}, {0.90f, 0.25f, 0.22f}, {0.88f, 0.55f, 0.45f}, {0.55f, 0.55f, 0.58f}, {0.48f, 0.62f, 0.86f}, {0.22f, 0.45f, 0.92f}, {0.55f, 0.55f, 0.58f}, {0, 0, 0},
    };
    const float* accent = t.palette[kInkAccent];
    for (u32 i = 1; i < 8; ++i) {
        const float* h = i == kFocusTone ? accent : kHue[i];
        for (int c = 0; c < 3; ++c) out[i][c] = t.bg[c] + (h[c] - t.bg[c]) * (i == kFocusTone ? 0.6f : i == 3 ? 0.35f : 0.55f);
        out[i][3] = 1;
    }
}

// The tone of a frame compared: by how its share of the time moved.
u32 diff_tone(double before, double after) {
    if (before < 0) return 1;  // (a path the earlier profile had not)
    const double d = after - before;
    if (std::abs(d) < 0.005) return 3;
    if (d > 0) return d >= 0.05 ? 1 : 2;
    return d <= -0.05 ? 5 : 4;
}

}  // namespace

// Draws the profile tabs in front in their panes: over the pane's text
// area, a box with the heading, the flame graph and the list of
// functions, and a box for each colour of frame on top of it.
void App::prof_layout(FrameDesc& fd) {
    if (!profui_) return;
    ProfUi& u = *profui_;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    make_colours(theme_, u.tones);
    make_diff_colours(theme_, u.diff_tones);
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->prof || fd.popup_count >= std::size(fd.popups)) continue;
        ProfView& v = *pane->doc->prof;
        const Profile& p = *v.p;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 20 || h < ch * 8) continue;
        const u32 cols = static_cast<u32>(w / cw), rows = static_cast<u32>(h / ch);
        v.x = x, v.y = y, v.cols = cols, v.rows = rows;
        v.pad_x = (w - static_cast<int>(cols) * cw) / 2;
        v.pad_y = (h - static_cast<int>(rows) * ch) / 2;

        if (v.relations && (v.rel_of != (v.sel < v.order.size() ? v.order[v.sel] : ~0u) || v.rel_thread != v.thread)) prof_relations(v);
        u64 key = 1469598103934665603ull;
        auto mix = [&](u64 k) { key = (key ^ k) * 1099511628211ull; };
        mix(cols), mix(rows), mix(font.generation()), mix(v.zoom), mix(static_cast<u64>(v.hover + 1)), mix(v.sel), mix(v.top), mix(v.shift), mix(static_cast<u64>(v.thread + 1));
        mix(v.bottom_up), mix(v.relations), mix(v.rel_sel), mix(v.rel_top), mix(v.rel_of), mix(v.dsel), mix(v.dtop);
        mix(std::bit_cast<u64>(v.t0)), mix(std::bit_cast<u64>(v.t1));
        if (key != v.key) {
            v.key = key;
            v.box.resize(cols, rows);
            v.hits.clear();
            TextBox& box = v.box;

            // The heading: what ran, where, and how much was seen.
            u32 c = box.put(font, 0, 1, short_path(p.program), 0, Font::kBold);
            if (!p.args.empty()) c = box.put(font, 0, c + 1, clip(p.args, cols > c + 30 ? cols - c - 30 : 1), kInkMuted);
            const std::string where = p.backend + (p.host.empty() ? "" : " on " + p.host) + " \xC2\xB7 " + p.when;
            if (c + width_of(where) + 3 < cols) box.put(font, 0, cols - width_of(where) - 1, where, kInkMuted);
            char sum[160];
            if (p.kind == "memory")  // (bytes first touched, from the faults that touched them)
                std::snprintf(sum, sizeof sum, "%.1f MB new \xC2\xB7 %s faults \xC2\xB7 %u thread%s", static_cast<double>(p.weight) / 1048576.0, with_commas(p.samples).c_str(), p.threads, p.threads == 1 ? "" : "s");
            else if (p.kind == "allocations" || p.kind == "held")  // (bytes, from the allocations sampled)
                std::snprintf(sum, sizeof sum, "%.1f MB %s \xC2\xB7 %s sampled", static_cast<double>(p.weight) / 1048576.0, p.kind == "held" ? "held at the end" : "allocated", with_commas(p.samples).c_str());
            else if (p.kind == "waiting")
                std::snprintf(sum, sizeof sum, "%.2f s waited \xC2\xB7 %s waits \xC2\xB7 %u thread%s", static_cast<double>(p.weight) * 1e-9, with_commas(p.samples).c_str(), p.threads, p.threads == 1 ? "" : "s");
            else
                std::snprintf(sum, sizeof sum, "%s samples \xC2\xB7 %.2f s \xC2\xB7 %u thread%s", with_commas(p.samples).c_str(), p.seconds, p.threads, p.threads == 1 ? "" : "s");
            c = box.put(font, 1, 1, sum, kInkMuted);
            if (v.thread >= 0) c = box.put(font, 1, c, " \xC2\xB7 " + v.root_name() + " " + percent(v.whole, p.weight), 0);
            if (v.bottom_up) c = box.put(font, 1, c, " \xC2\xB7 bottom-up", 0);
            if (v.t0 >= 0) {
                char stretch[96];
                std::snprintf(stretch, sizeof stretch, " \xC2\xB7 %.2f\xE2\x80\x93%.2f s of %.2f", v.t0, v.t1, v.full->seconds);
                c = box.put(font, 1, c, stretch, 0);
            }
            if (v.base) c = box.put(font, 1, c, " \xC2\xB7 compared with " + v.base->when + (v.base->program != p.program ? " (" + short_path(v.base->program) + ")" : std::string()), 0);
            // The note after it, or on a row of its own when it would be cut
            // short there (a program not installed, said in full).
            const bool note_row = !p.note.empty() && c + 3 + width_of(p.note) + 1 > cols;
            if (note_row) put(box, font, 2, 1, clip(p.note, cols > 2 ? cols - 2 : 1), ink::kWarning);
            else if (!p.note.empty()) put(box, font, 1, c + 3, clip(p.note, cols > c + 4 ? cols - c - 4 : 1), ink::kWarning);
            const u32 above = note_row ? 4 : 3;  // (the rows above the timeline: the crumbs the last of them)

            // The timeline: a lane for each thread (the heaviest few), how much
            // it ran in each stretch of time, the stretch chosen lit.
            v.tl_rows = 0;
            const Profile& whole = *v.full;
            if (!v.diff && whole.timeline.size() > 1) {
                double span = 0;
                for (const Profile::Sample& s : whole.timeline) span = std::max(span, s.t);
                if (span > 0) {
                    v.tl_span = span;
                    v.lane_ids.clear();
                    for (size_t k = 0; k < whole.by_thread.size() && k < 6; ++k) v.lane_ids.push_back(whole.by_thread[k].id);
                    v.tl_row = above;
                    v.tl_rows = static_cast<u32>(v.lane_ids.size());
                    const u32 label = std::min<u32>(16, cols / 5);
                    v.tl_col = label + 2;
                    v.tl_cols = cols > v.tl_col + 2 ? cols - v.tl_col - 1 : 1;
                    std::vector<std::vector<u64>> lanes(v.tl_rows, std::vector<u64>(v.tl_cols, 0));
                    for (const Profile::Sample& s : whole.timeline) {
                        const auto lane = std::ranges::find(v.lane_ids, s.thread);
                        if (lane == v.lane_ids.end()) continue;
                        const u32 b = std::min<u32>(v.tl_cols - 1, static_cast<u32>(s.t / span * static_cast<double>(v.tl_cols)));
                        lanes[static_cast<size_t>(lane - v.lane_ids.begin())][b] += s.w;
                    }
                    static const char* const kLevel[] = {"\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84", "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"};
                    for (u32 l = 0; l < v.tl_rows; ++l) {
                        const u32 r = v.tl_row + l;
                        const ProfileThread& t = whole.by_thread[l];
                        box.put(font, r, 1, clip(t.name, label), kInkMuted);
                        const u64 most = *std::ranges::max_element(lanes[l]);
                        for (u32 b = 0; b < v.tl_cols; ++b) {
                            const u64 ran = lanes[l][b];
                            if (!ran) box.put(font, r, v.tl_col + b, "\xC2\xB7", kInkMuted);  // (not running)
                            else box.put(font, r, v.tl_col + b, kLevel[std::min<u64>(7, (ran * 8 + most - 1) / most - 1)], kInkAccent);
                            const double b0 = static_cast<double>(b) / static_cast<double>(v.tl_cols) * span, b1 = static_cast<double>(b + 1) / static_cast<double>(v.tl_cols) * span;
                            if (v.t0 >= 0 && b1 > v.t0 && b0 <= v.t1) box.cells[static_cast<size_t>(r) * cols + v.tl_col + b] |= kCellSelected;
                        }
                    }
                }
            }

            // Where in the graph the view is: the frames above the one zoomed into.
            const u32 top_rows = above + (v.tl_rows ? v.tl_rows + 1 : 0);
            {
                std::vector<u32> path;
                for (u32 n = v.zoom; n != v.root; n = p.nodes[n].parent) path.push_back(n);
                std::string crumbs = v.root_name();
                for (size_t k = path.size(); k-- > 0;) crumbs += " \xE2\x80\xBA " + std::string(short_name(p.funcs[p.nodes[path[k]].func].name));
                c = box.put(font, above - 1, 1, clip(crumbs, cols / 2), v.zoom != v.root ? 0 : kInkMuted);
                if (v.zoom != v.root) box.put(font, above - 1, c + 2, "Esc: zoom out", kInkMuted);
            }

            // The flame graph: the root at the bottom, each function above
            // what called it, as wide as its share of the time.
            // (The list gives up rows before the graph has fewer than four.)
            u32 list_min = std::min<u32>(8, rows / 3);
            while (list_min > 2 && rows < top_rows + list_min + 3 + 4) --list_min;
            const u32 room = rows > top_rows + list_min + 3 ? rows - top_rows - list_min - 3 : 3;
            // The frames a cell wide or more, by depth: as many rows as the deepest of them needs.
            struct Item {
                u32 node;
                double x0, x1;
            };
            std::vector<Item> placed;
            u32 levels = static_cast<u32>(v.depth[v.zoom]) + 1;
            {
                std::vector<Item> todo{{v.zoom, 0.0, static_cast<double>(cols)}};
                while (!todo.empty()) {
                    const Item it = todo.back();
                    todo.pop_back();
                    if (it.x1 - it.x0 < 0.999 || std::lround(it.x1) <= std::lround(it.x0)) continue;  // (narrower than a cell: so is all it called)
                    placed.push_back(it);
                    levels = std::max<u32>(levels, static_cast<u32>(v.depth[it.node]) + 1);
                    const ProfileNode& n = p.nodes[it.node];
                    double at = it.x0;
                    for (const u32 k : n.kids) {
                        const double wk = n.total ? (it.x1 - it.x0) * static_cast<double>(p.nodes[k].total) / static_cast<double>(n.total) : 0;
                        todo.push_back({k, at, at + wk});
                        at += wk;
                    }
                }
            }
            v.flame_rows = std::min(levels, room);
            v.flame_row = top_rows;
            v.shift = std::min<u32>(v.shift, levels > v.flame_rows ? levels - v.flame_rows : 0);
            const u32 bottom = v.flame_row + v.flame_rows - 1;  // the row of depth 0 (scrolled: shift levels pushed off the top)
            const u32 zoom_depth = v.depth[v.zoom];
            const u32 focus_func = v.sel < v.order.size() ? v.order[v.sel] : ~0u;
            std::vector<u32> last_end(rows, ~0u), last_layer(rows, ~0u);
            auto put_frame = [&](u32 node, u32 depth, u32 c0, u32 c1, bool ancestor) {
                const i64 row = static_cast<i64>(bottom) - static_cast<i64>(depth) + static_cast<i64>(v.shift);
                if (row < static_cast<i64>(v.flame_row) || row > static_cast<i64>(bottom) || c1 <= c0) return;
                const u32 r = static_cast<u32>(row);
                const ProfileFunc& f = p.funcs[p.nodes[node].func];
                u32 tone;
                // (Compared, colour says how a frame moved: only the one under the mouse is lit.)
                if (static_cast<int>(node) == v.hover || (!v.diff && node != 0 && p.nodes[node].func == focus_func)) tone = kFocusTone;
                else if (v.diff) tone = diff_tone(v.diff->node_before[node], p.weight ? static_cast<double>(p.nodes[node].total) / static_cast<double>(p.weight) : 0);
                else if (ancestor || !f.own) tone = 1 + kOwnTones + hash_of(f.name) % kLibTones;
                else tone = 1 + hash_of(f.name) % kOwnTones;
                const bool touching = last_end[r] == c0 && last_layer[r] == tone;
                if (v.diff && touching && c1 - c0 >= 2) ++c0;  // (compared, the tones mean something: a gap between two of one tone instead)
                if (!v.diff && tone != kFocusTone && touching)  // (two side by side of one tone would run into one)
                    tone = tone <= kOwnTones ? 1 + tone % kOwnTones : 1 + kOwnTones + (tone - kOwnTones) % kLibTones;
                last_end[r] = c1;
                last_layer[r] = tone;
                TextBox& l = v.box;
                if (c1 - c0 >= 4) {
                    const std::string label = node == v.root ? v.root_name() : std::string(short_name(f.name));
                    l.put(font, r, c0 + 1, clip(label, c1 - c0 - 2), 0);
                }
                for (u32 k = c0; k < c1 && k < cols; ++k) l.cells[static_cast<size_t>(r) * cols + k] |= kCellSelected | tone << kCellDecoShift;
                v.hits.push_back({r, c0, c1, node});
            };
            // Above the frame zoomed into: what led to it, across the whole width.
            for (u32 n = v.zoom, d = zoom_depth; n != v.root;) {
                n = p.nodes[n].parent;
                --d;
                put_frame(n, d, 0, cols, true);
            }
            // It and what it called, by their share of its time.
            std::ranges::sort(placed, [](const Item& a, const Item& b) { return a.x0 < b.x0; });  // (left to right: neighbours of one tone are told apart)
            for (const Item& it : placed) put_frame(it.node, v.depth[it.node], static_cast<u32>(std::lround(it.x0)), static_cast<u32>(std::lround(it.x1)), false);

            // The functions, the heaviest by their own time first.
            v.list_row = bottom + 2;
            const u32 head = v.list_row;
            v.list_row = head + 1;
            v.list_rows = rows > v.list_row + 1 ? rows - v.list_row - 1 : 0;
            const u32 n = static_cast<u32>(v.order.size());
            v.sel = n ? std::min(v.sel, n - 1) : 0;
            if (v.sel < v.top) v.top = v.sel;
            if (v.list_rows && v.sel >= v.top + v.list_rows) v.top = v.sel - v.list_rows + 1;
            v.top = std::min(v.top, n > v.list_rows ? n - v.list_rows : 0);
            const u32 src_w = std::min<u32>(36, cols / 4), name_col = 16, src_col = cols > src_w + 2 ? cols - src_w - 1 : cols;
            if (v.diff) {  // compared: each function's own share before and after, the most changed first
                const std::vector<ProfileDiff::Row>& dr = v.diff->rows;
                const u32 nd = static_cast<u32>(dr.size());
                if (head < rows) {
                    box.put(font, head, 1, "Before", kInkMuted);
                    box.put(font, head, 9, "After", kInkMuted);
                    box.put(font, head, 17, "Change", kInkMuted);
                    box.put(font, head, 26, "Function", kInkMuted);
                }
                v.dsel = nd ? std::min(v.dsel, nd - 1) : 0;
                if (v.dsel < v.dtop) v.dtop = v.dsel;
                if (v.list_rows && v.dsel >= v.dtop + v.list_rows) v.dtop = v.dsel - v.list_rows + 1;
                v.dtop = std::min(v.dtop, nd > v.list_rows ? nd - v.list_rows : 0);
                for (u32 k = 0; k < v.list_rows && v.dtop + k < nd; ++k) {
                    const u32 r = v.list_row + k;
                    const ProfileDiff::Row& row = dr[v.dtop + k];
                    const std::string b = share_text(row.self_before), a = share_text(row.self_after), moved = change_text(row.self_after - row.self_before);
                    box.put(font, r, 7 - std::min<u32>(7, width_of(b)), b, kInkMuted);
                    box.put(font, r, 15 - std::min<u32>(15, width_of(a)), a, 0);
                    const u32 ink = row.self_after > row.self_before + 0.0005 ? kInkDiagnostic : row.self_after + 0.0005 < row.self_before ? kInkAccent : kInkMuted;
                    box.put(font, r, 23 - std::min<u32>(23, width_of(moved)), moved, ink);
                    const std::string name = row.func == ~0u ? row.name + "  (gone)" : row.name;
                    box.put(font, r, 26, clip(name, cols > 28 ? cols - 28 : 1), row.func == ~0u ? kInkMuted : 0);
                    if (v.dtop + k == v.dsel) box.select_row(r);
                }
            } else if (v.relations) {  // the chosen function's callers, then its callees, in place of the list
                const u32 nr = static_cast<u32>(v.rel.size());
                const std::string of = v.rel_of < p.funcs.size() ? std::string(short_name(p.funcs[v.rel_of].name)) : std::string();
                if (head < rows) box.put(font, head, 2, clip("Callers and callees of " + of, cols > 4 ? cols - 4 : 1), kInkMuted);
                if (v.rel_sel < v.rel_top) v.rel_top = v.rel_sel;
                if (v.list_rows && v.rel_sel >= v.rel_top + v.list_rows) v.rel_top = v.rel_sel - v.list_rows + 1;
                v.rel_top = std::min(v.rel_top, nr > v.list_rows ? nr - v.list_rows : 0);
                for (u32 k = 0; k < v.list_rows && v.rel_top + k < nr; ++k) {
                    const u32 r = v.list_row + k;
                    const ProfView::Rel& rx = v.rel[v.rel_top + k];
                    if (rx.kind == 0) {
                        box.put(font, r, 2, rx.func == 1 ? "Called by" : "Calls", kInkMuted, Font::kBold);
                        continue;
                    }
                    const ProfileFunc& f = p.funcs[rx.func];
                    const std::string wt = percent(rx.weight, v.whole);
                    box.put(font, r, 7 - std::min<u32>(7, width_of(wt)), wt, f.own ? kInkAccent : 0);
                    box.put(font, r, name_col, clip(f.name, src_col > name_col + 2 ? src_col - name_col - 2 : 1), f.own ? 0 : kInkMuted);
                    if (src_col < cols && !f.file.empty()) box.put(font, r, src_col, clip(short_path(f.file) + ":" + std::to_string(f.line), src_w), kInkMuted);
                    if (v.rel_top + k == v.rel_sel) box.select_row(r);
                }
            } else if (head < rows) {
                box.put(font, head, 2, "Self", kInkMuted);
                box.put(font, head, 9, "Total", kInkMuted);
                box.put(font, head, name_col, "Function", kInkMuted);
                if (src_col < cols) box.put(font, head, src_col, "Source", kInkMuted);
            }
            for (u32 k = 0; !v.relations && !v.diff && k < v.list_rows && v.top + k < n; ++k) {
                const u32 r = v.list_row + k;
                const u32 fi = v.order[v.top + k];
                const ProfileFunc& f = p.funcs[fi];
                const u64 f_self = v.self(fi), f_total = v.total(fi);
                const std::string self = percent(f_self, v.whole), total = percent(f_total, v.whole);
                box.put(font, r, 7 - std::min<u32>(7, width_of(self)), self, f.own && f_self ? kInkAccent : f_self ? 0 : kInkMuted, f.own && f_self ? Font::kBold : 0);
                box.put(font, r, 14 - std::min<u32>(14, width_of(total)), total, kInkMuted);
                box.put(font, r, name_col, clip(f.name, src_col > name_col + 2 ? src_col - name_col - 2 : 1), f.own ? 0 : kInkMuted);
                if (src_col < cols && !f.file.empty()) box.put(font, r, src_col, clip(short_path(f.file) + ":" + std::to_string(f.line), src_w), kInkMuted);
                if (v.top + k == v.sel) box.select_row(r);
            }
            // The last line: what the mouse is over, or the function chosen.
            if (rows > 1) {
                std::string about;
                const ProfileFunc* f = nullptr;
                u64 total = 0, self = 0;
                if (v.hover >= 0) {
                    const ProfileNode& hn = p.nodes[static_cast<u32>(v.hover)];
                    f = &p.funcs[hn.func];
                    total = hn.total, self = hn.self;
                } else if (focus_func != ~0u) {
                    f = &p.funcs[focus_func];
                    total = v.total(focus_func), self = v.self(focus_func);
                }
                if (f) {
                    about = f->name + "  \xC2\xB7  " + percent(total, v.whole) + " total, " + percent(self, v.whole) + " self";
                    if (!f->file.empty()) about += "  \xC2\xB7  " + short_path(f->file) + ":" + std::to_string(f->line) + "  \xC2\xB7  Enter: open source";
                }
                box.put(font, rows - 1, 1, clip(about, cols - 2), kInkMuted);
            }
        }

        // One box over the pane's text area: the frames are its lit cells,
        // each in its tone, as rounded boxes.
        const Look lk = look();
        Popup& q = fd.popups[fd.popup_count++];
        q = Popup{};
        q.x = x, q.y = y, q.w = w, q.h = h;
        q.pad_x = v.pad_x, q.pad_y = v.pad_y;
        q.cols = v.box.cols, q.rows = v.box.rows;
        q.cells = v.box.cells.data();
        q.border_px = 0;
        q.bg = theme_.bg;
        q.tones = v.diff ? &u.diff_tones[0][0] : &u.tones[0][0];
        q.pill = std::max(2.0f, lk.pill * 0.5f);
        q.pill_dx = std::max(1.0f, std::round(scale_));
        q.pill_dy = std::max(1.0f, std::round(scale_));
    }
}

namespace {

// The profile tab in front of the pane under a point, and the cell the point is in.
}  // namespace

bool App::prof_hit(int px, int py, Pane** pane, u32* row, u32* col) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->prof) continue;
        const ProfView& v = *p->doc->prof;
        if (px < v.x || py < v.y || px >= p->x1 || py >= p->y1 || !v.cols) continue;
        const int c = (px - v.x - v.pad_x) / font_->cell_w(), r = (py - v.y - v.pad_y) / font_->cell_h();
        if (c < 0 || r < 0 || static_cast<u32>(c) >= v.cols || static_cast<u32>(r) >= v.rows) return false;
        *pane = p.get();
        *row = static_cast<u32>(r);
        *col = static_cast<u32>(c);
        return true;
    }
    return false;
}

bool App::prof_mouse_motion(const SDL_MouseMotionEvent& m) {
    Pane* pane = nullptr;
    u32 row = 0, col = 0;
    int hover = -1;
    const bool over = prof_hit(static_cast<int>(m.x * density_), static_cast<int>(m.y * density_), &pane, &row, &col);
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->prof) continue;
        ProfView& v = *p->doc->prof;
        if (v.tl_drag) {  // (the stretch follows the mouse, anywhere across the tab)
            const int cw = font_->cell_w();
            const int cx = (static_cast<int>(m.x * density_) - v.x - v.pad_x) / std::max(1, cw);
            const double t = v.time_at(static_cast<u32>(std::max(0, cx)));
            v.t0 = std::min(v.tl_anchor, t), v.t1 = std::max(v.tl_anchor, t);
            v.key = 0;
            dirty_ = true;
            continue;
        }
        hover = -1;
        if (over && pane == p.get())
            for (const ProfView::Hit& h : v.hits)
                if (h.row == row && col >= h.c0 && col < h.c1) hover = static_cast<int>(h.node);
        if (v.hover != hover) {
            v.hover = hover;
            dirty_ = true;
        }
    }
    return over;
}

// Opens a function's source at its first line with time on it, or at its
// definition if that can be found above that line.
void App::prof_go_to(const Profile& p, u32 func) {
    const ProfileFunc& f = p.funcs[func];
    if (f.file.empty()) return;
    remember_place();
    if (!open_path(f.file.c_str())) return;
    Doc& d = *pane_->doc;
    const u64 line = f.line ? f.line - 1 : 0;
    if (d.loading) {  // (on its way from another machine)
        d.goto_line = line;
        d.goto_col = 0;
        d.goto_utf16 = false;
        return;
    }
    const u64 at = prof_definition(d, f);
    d.ed.move_to(d.ed.buf.line_start(std::min<u64>(at, d.ed.buf.line_count() - 1)), false);
    scroll_to_cursor();
    const double cell = font_->cell_h();
    pane_->target_y = std::clamp(static_cast<double>(at) * cell - (view_h() - cell) / 3, 0.0, std::max(max_scroll_y(), 0.0));
}

bool App::prof_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    u32 row = 0, col = 0;
    if (!prof_hit(static_cast<int>(b.x * density_), static_cast<int>(b.y * density_), &pane, &row, &col)) return false;
    focus_pane(pane);
    ProfView& v = *pane->doc->prof;
    const Profile& p = *v.p;
    dirty_ = true;
    if (b.button != SDL_BUTTON_LEFT) return true;
    if (v.tl_rows && row >= v.tl_row && row < v.tl_row + v.tl_rows && col >= v.tl_col) {  // a stretch of the timeline, dragged out from here
        v.tl_drag = true;
        v.tl_anchor = v.t0 = v.t1 = v.time_at(col);
        v.key = 0;
        return true;
    }
    for (const ProfView::Hit& h : v.hits)
        if (h.row == row && col >= h.c0 && col < h.c1) {
            // A frame: the graph zooms into it (or back out to it), and
            // its function is chosen in the list; twice opens its source.
            v.zoom = h.node;
            const u32 func = p.nodes[h.node].func;
            for (u32 k = 0; k < v.order.size(); ++k)
                if (v.order[k] == func) v.sel = k;
            if (b.clicks >= 2 && h.node != 0) prof_go_to(p, func);
            return true;
        }
    if (v.relations && row >= v.list_row && row < v.list_row + v.list_rows) {  // a caller or a callee: chosen; twice, it is the function shown
        const u32 k = v.rel_top + (row - v.list_row);
        if (k < v.rel.size() && v.rel[k].kind != 0) {
            v.rel_sel = k;
            if (b.clicks >= 2)
                for (u32 j = 0; j < v.order.size(); ++j)
                    if (v.order[j] == v.rel[k].func) v.sel = j;
        }
        v.key = 0;
        return true;
    }
    if (row >= v.list_row && row < v.list_row + v.list_rows) {
        const u32 k = v.top + (row - v.list_row);
        if (k < v.order.size()) {
            v.sel = k;
            if (b.clicks >= 2) prof_go_to(p, v.order[k]);
        }
    }
    return true;
}

bool App::prof_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    u32 row = 0, col = 0;
    if (!prof_hit(static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_), &pane, &row, &col)) return false;
    ProfView& v = *pane->doc->prof;
    const int by = static_cast<int>(std::lround(w.y * 3));
    if (row < v.list_row) {  // over the graph: a deep one scrolls
        v.shift = static_cast<u32>(std::max(0, static_cast<int>(v.shift) + by));
    } else {
        const int n = static_cast<int>(v.order.size()), room = static_cast<int>(v.list_rows);
        v.top = static_cast<u32>(std::clamp(static_cast<int>(v.top) - by, 0, std::max(0, n - room)));
        if (v.sel < v.top) v.sel = v.top;
        if (room > 0 && v.sel >= v.top + static_cast<u32>(room)) v.sel = v.top + static_cast<u32>(room) - 1;
    }
    dirty_ = true;
    return true;
}

bool App::prof_key(const SDL_KeyboardEvent& k) {
    if (!pane_->doc->prof || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT))) return false;
    ProfView& v = *pane_->doc->prof;
    const Profile& p = *v.p;
    const u32 n = static_cast<u32>(v.order.size());
    if (v.diff) {  // compared: the list of changes
        const u32 nd = static_cast<u32>(v.diff->rows.size());
        auto pick = [&] {  // (the graph's chosen function follows the row)
            if (v.dsel < nd)
                for (u32 j = 0; j < n; ++j)
                    if (v.order[j] == v.diff->rows[v.dsel].func) v.sel = j;
        };
        switch (k.key) {
            case SDLK_UP: v.dsel = v.dsel ? v.dsel - 1 : 0; pick(); break;
            case SDLK_DOWN: v.dsel = nd ? std::min(v.dsel + 1, nd - 1) : 0; pick(); break;
            case SDLK_PAGEUP: v.dsel = v.dsel > v.list_rows ? v.dsel - v.list_rows : 0; pick(); break;
            case SDLK_PAGEDOWN: v.dsel = nd ? std::min(v.dsel + std::max(1u, v.list_rows), nd - 1) : 0; pick(); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (v.dsel < nd && v.diff->rows[v.dsel].func != ~0u) prof_go_to(p, v.diff->rows[v.dsel].func);
                break;
            case SDLK_ESCAPE:
                if (v.zoom == v.root) return false;
                v.zoom = p.nodes[v.zoom].parent;
                break;
            case SDLK_SPACE: pick(); goto zoom;  // (below)
            default: return false;
        }
        dirty_ = true;
        return true;
    }
    if (k.key == SDLK_B && !(k.mod & SDL_KMOD_SHIFT)) {  // the graph bottom-up, or top-down again
        prof_flip(v);
        dirty_ = true;
        return true;
    }
    if (k.key == SDLK_C && !(k.mod & SDL_KMOD_SHIFT)) {  // the chosen function's callers and callees, or the functions again
        v.relations = !v.relations;
        if (v.relations) prof_relations(v);
        v.key = 0;
        dirty_ = true;
        return true;
    }
    if (v.relations) {
        const u32 nr = static_cast<u32>(v.rel.size());
        auto step = [&](int by) {  // (over the headings)
            for (u32 at = v.rel_sel; nr;) {
                if ((by < 0 && at == 0) || (by > 0 && at + 1 >= nr)) break;
                at = static_cast<u32>(static_cast<int>(at) + by);
                if (v.rel[at].kind != 0) {
                    v.rel_sel = at;
                    break;
                }
            }
        };
        switch (k.key) {
            case SDLK_UP: step(-1); break;
            case SDLK_DOWN: step(1); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:  // that one chosen: its callers and callees
                if (v.rel_sel < nr && v.rel[v.rel_sel].kind != 0)
                    for (u32 j = 0; j < n; ++j)
                        if (v.order[j] == v.rel[v.rel_sel].func) v.sel = j;
                break;
            case SDLK_ESCAPE:
                if (v.zoom != v.root) {
                    v.zoom = p.nodes[v.zoom].parent;
                    break;
                }
                v.relations = false;
                break;
            case SDLK_SPACE: break;  // (below)
            default: return false;
        }
        if (k.key != SDLK_SPACE) {
            v.key = 0;
            dirty_ = true;
            return true;
        }
    }
    switch (k.key) {
        case SDLK_UP: v.sel = v.sel ? v.sel - 1 : 0; break;
        case SDLK_DOWN: v.sel = n ? std::min(v.sel + 1, n - 1) : 0; break;
        case SDLK_PAGEUP: v.sel = v.sel > v.list_rows ? v.sel - v.list_rows : 0; break;
        case SDLK_PAGEDOWN: v.sel = n ? std::min(v.sel + std::max(1u, v.list_rows), n - 1) : 0; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (v.sel < n) prof_go_to(p, v.order[v.sel]);
            break;
        case SDLK_ESCAPE:
            if (v.zoom == v.root) {
                if (v.t0 < 0) return false;
                v.t0 = v.t1 = -1;  // (the stretch of time chosen let go of: all of it again)
                prof_narrow(v);
                break;
            }
            v.zoom = p.nodes[v.zoom].parent;
            break;
        case SDLK_SPACE:
        zoom: {  // the graph zooms into the chosen function: where it took the most time (in the graph shown)
            if (v.sel >= n) break;
            const u32 func = v.order[v.sel];
            u32 best = 0;
            for (u32 node = 1; node < p.nodes.size(); ++node)
                if (p.nodes[node].func == func && !v.is_root[node] && (best == 0 || p.nodes[node].total > p.nodes[best].total) && v.under_root(node)) best = node;
            if (best) v.zoom = v.zoom == best ? p.nodes[best].parent : best;  // (again: back out)
            break;
        }
        default: return false;
    }
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- inline performance labels

// The line (from 0) a function's definition starts on: looking up from
// the first line with time on it for its name and a bracket after it;
// that line itself if there is none.
u64 App::prof_definition(const Doc& d, const ProfileFunc& f) const {
    const u64 lines = d.ed.buf.line_count();
    const u64 first = std::min<u64>(f.line ? f.line - 1 : 0, lines ? lines - 1 : 0);
    const std::string name = bare_name(f.name);
    if (name.empty() || name.find_first_of(" ()[]") != std::string::npos) return first;
    for (u64 l = first + 1, looked = 0; l-- > 0 && looked < 400; ++looked) {
        const u64 a = d.ed.buf.line_start(l), b = d.ed.line_end(l);
        if (b - a > 4096) continue;
        const std::string text = d.ed.buf.text(a, b - a);
        for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
            const bool word_start = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
            size_t after = at + name.size();
            if (after < text.size() && text[after] == '<') {  // (a template's arguments)
                int depth = 0;
                for (; after < text.size(); ++after) {
                    if (text[after] == '<') ++depth;
                    else if (text[after] == '>' && --depth == 0) {
                        ++after;
                        break;
                    }
                }
            }
            while (after < text.size() && text[after] == ' ') ++after;
            if (word_start && after < text.size() && text[after] == '(') return l;
        }
    }
    return first;
}

// Adds the times to each view of a file the profile shown has times in:
// after each function that took time, its share of it all and of its own;
// after a line that took much, its share. (One note to a line: a language
// server's or a script's goes first.)
void App::prof_marks(FrameDesc& fd) {
    if (!profui_) return;
    ProfUi& u = *profui_;
    if (!u.annotate || !u.shown) return;
    const Profile& p = *u.shown;
    Font& font = *font_;
    for (u32 vi = 0; vi < fd.view_count && vi < kMaxViews; ++vi) {
        GridSpec& g = fd.views[vi].grid;
        Doc* d = find_doc(g.doc);
        if (!d || d->ed.path.empty() || d->loading) continue;
        ProfUi::Where& at = u.where[d->id];
        if (!at.known || at.raw != d->ed.path) {
            at.raw = d->ed.path;
            at.full = full_path(d->ed.path);
            const auto found = p.lines.find(at.full);
            at.lines = found == p.lines.end() ? nullptr : &found->second;
            at.known = true;
        }
        if (!at.lines) continue;
        const std::string& path = at.full;
        ProfUi::Notes& nt = u.notes[d->id];
        if (nt.rev != d->ed.revision() || nt.p != &p || nt.font != font.generation()) {
            nt.rev = d->ed.revision();
            nt.p = &p;
            nt.font = font.generation();
            nt.lines.clear();
            nt.kinds.clear();
            std::map<u64, std::pair<std::string, u32>> by_line;  // line -> text, ink
            const u64 min_func = p.weight / 500, min_line = p.weight / 200;  // (0.2% and 0.5%)
            for (u32 fi = 1; fi < p.funcs.size(); ++fi) {
                const ProfileFunc& f = p.funcs[fi];
                if (f.file != path || f.total <= min_func) continue;
                const u64 l = prof_definition(*d, f);
                std::string text = "\xE2\x97\x86 " + percent(f.total, p.weight) + " total";
                if (f.self) text += ", " + percent(f.self, p.weight) + " self";
                if (by_line.contains(l)) by_line[l].first = text + " \xC2\xB7 " + by_line[l].first;
                else by_line[l] = {text, kInkAccent};
            }
            for (const auto& [line, w] : *at.lines) {
                if (w.self <= min_line || line == 0) continue;
                const u64 l = line - 1;
                const std::string text = "\xE2\x97\x8F " + percent(w.self, p.weight);
                if (by_line.contains(l)) by_line[l].first += "  " + text;
                else by_line[l] = {text, static_cast<u32>(w.self * 20 > p.weight ? kInkDiagnostic + 1 : kInkMuted)};
            }
            for (const auto& [l, t] : by_line) {
                TextBox box;
                box.resize(std::min<u32>(width_of(t.first), 160), 1);
                box.put(font, 0, 0, t.first, t.second);  // (upright: the font's italic may be a script)
                nt.lines.push_back({l, std::move(box.cells)});
                nt.kinds.push_back(t.first.starts_with("\xE2\x97\x86") ? 'F' : 'L');
            }
        }
        if (nt.lines.empty()) continue;
        std::vector<GridNote>& notes = u.view_notes[vi];
        notes.assign(g.notes.begin(), g.notes.end());
        const size_t had = notes.size();
        const u64 lines = d->ed.buf.line_count();
        for (const auto& [l, cells] : nt.lines) {
            if (l < g.top_line || l >= g.bottom() || l >= lines) continue;
            bool taken = false;
            for (size_t k = 0; k < had && !taken; ++k) taken = notes[k].line == l;
            if (taken) continue;
            const u64 start = d->ed.buf.line_start(l), end = d->ed.line_end(l);
            notes.push_back({l, d->ed.col_between(start, end) + 3, cells.data(), static_cast<u32>(cells.size())});
        }
        g.notes = notes;
    }
}

// ---------------------------------------------------------------- for the tests

void App::prof_set(const std::string& what, const std::string& value) {
    ProfUi& u = *profui_;
    if (what == "program") u.program = full_path(value);
    else if (what == "args") u.args = value;
    else if (what == "cwd") u.cwd = full_path(value);
    else if (what == "dwarf") u.dwarf = value == "on";
    else if (what == "annotate") u.annotate = value != "off";
    else if (what == "kind") u.kind = value == "memory" ? ProfileRequest::kMemory : value == "waiting" ? ProfileRequest::kWaiting : value == "allocations" ? ProfileRequest::kAllocations : ProfileRequest::kTime;
    ++prof_gen_;
}

bool App::prof_busy() const {
    if (!profui_) return false;
    std::lock_guard lock(profui_->kept->mu);
    return profui_->job || profui_->kept->busy > 0 || !profui_->kept->read.empty();
}

// What choosing a row of the Profile tab's sections does (profiles: those
// made and kept, as listed; threads: from 1, All threads first), for the tests.
bool App::prof_pick(const std::string& section, u32 n) {
    ProfUi& u = *profui_;
    for (u32 i = 0, seen = 0; i < u.rows.size(); ++i) {
        const ProfUi::Act a = u.rows[i].act;
        const bool in = section == "threads" ? a == ProfUi::kThread : section == "profiles" && (a == ProfUi::kOpen || a == ProfUi::kOpenKept);
        if (in && ++seen == std::max(1u, n)) {
            prof_do(i);
            return true;
        }
    }
    return false;
}

void App::prof_toggle_labels() {
    profui_->annotate = !profui_->annotate;
    for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
    ++prof_gen_;
    dirty_ = true;
}

// What the profiling shows, on one line: the recording's state, the
// profile in front (its heaviest functions, alphabetically, and which of
// them have their source), and the lines with times in the document in front.
std::string App::prof_dump() {
    ProfUi& u = *profui_;
    std::string out;
    if (u.job) out += "recording; ";
    if (!u.error.empty()) out += "failed: " + u.error + "; ";
    out += std::to_string(u.done.size()) + " profiles";
    if (u.shown) {
        const Profile& p = *u.shown;
        out += "; " + short_path(p.program) + (p.kind == "time" || p.kind.empty() ? "" : " (" + p.kind + ")") + " by " + p.backend + (p.host.empty() ? "" : " on " + p.host) + ", " + std::to_string(p.threads) + " thread";
        std::vector<std::string> hot;
        for (const u32 f : p.by_self())
            if (p.funcs[f].self * 10 >= p.weight && hot.size() < 4) hot.push_back(std::string(short_name(p.funcs[f].name)) + (p.funcs[f].file.empty() ? "" : "@" + short_path(p.funcs[f].file)));
        std::ranges::sort(hot);
        out += ", over 10%:";
        for (const std::string& h : hot) out += " " + h;
    }
    if (pane_->doc->prof) {
        const ProfView& v = *pane_->doc->prof;
        out += "; tab: zoom " + (v.zoom != v.root ? std::string(short_name(v.p->funcs[v.p->nodes[v.zoom].func].name)) : v.root_name());
        if (v.thread >= 0 && !v.order.empty()) out += ", heaviest " + std::string(short_name(v.p->funcs[v.order[0]].name));
        if (v.t0 >= 0) {
            char stretch[64];
            std::snprintf(stretch, sizeof stretch, ", stretch %.2f-%.2f: %llu samples", v.t0, v.t1, static_cast<unsigned long long>(v.orig->samples));
            out += stretch;
            if (!v.order.empty()) out += ", heaviest " + std::string(short_name(v.p->funcs[v.order[0]].name));
        }
        if (v.diff) {  // (the functions that moved most, their own share before and after)
            out += ", compared with " + v.base->when + ":";
            for (size_t k = 0; k < v.diff->rows.size() && k < 4; ++k) {
                const ProfileDiff::Row& row = v.diff->rows[k];
                out += " " + std::string(short_name(row.name)) + " " + share_text(row.self_before) + "\xE2\x86\x92" + share_text(row.self_after) + (row.func == ~0u ? " (gone)" : "");
            }
            out += ", frames:";  // (the bottom row's, each with its tone)
            for (const u32 kid : v.p->nodes[v.root].kids) out += " " + std::string(short_name(v.p->funcs[v.p->nodes[kid].func].name)) + "=" + std::to_string(diff_tone(v.diff->node_before[kid], v.p->weight ? static_cast<double>(v.p->nodes[kid].total) / static_cast<double>(v.p->weight) : 0));
        }
        if (v.bottom_up) {  // (the bottom row: where time was spent, each with its share)
            out += ", bottom-up:";
            for (const u32 kid : v.p->nodes[v.root].kids) out += " " + std::string(short_name(v.p->funcs[v.p->nodes[kid].func].name)) + "=" + percent(v.p->nodes[kid].total, v.whole);
        }
        if (v.relations) {
            out += ", of " + (v.rel_of < v.p->funcs.size() ? std::string(short_name(v.p->funcs[v.rel_of].name)) : std::string("?")) + ":";
            for (const ProfView::Rel& rx : v.rel)
                out += rx.kind == 0 ? (rx.func == 1 ? " callers" : " | callees") : " " + std::string(short_name(v.p->funcs[rx.func].name)) + "=" + percent(rx.weight, v.whole);
        }
    } else if (auto it = u.notes.find(pane_->doc->id); it != u.notes.end()) {
        out += "; times on lines";
        for (size_t k = 0; k < it->second.lines.size(); ++k) out += " " + std::to_string(it->second.lines[k].first + 1) + it->second.kinds[k];
    }
    return out;
}

std::string App::prof_note() {
    if (pane_->doc->prof) return pane_->doc->prof->p->note;
    return profui_->shown ? profui_->shown->note : std::string();
}

}  // namespace blit
