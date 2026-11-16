// Profiling in the editor: the side panel's Profile tab (the program, its
// arguments and folder, recording, the profiles made), a tab for each
// profile (a flame graph, and its functions by their own time), and the
// inline performance labels: at each function that took time, and on lines that
// took much (profile.hpp records and reads).
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#include "app.hpp"
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
    std::shared_ptr<Profile> p;
    std::vector<u16> depth;  // of each node
    u16 deepest = 0;
    std::vector<u32> order;  // the functions, the heaviest by their own time first
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
    u32 frequency = 999;
    bool dwarf = false;
    bool annotate = true;
    std::unique_ptr<ProfileJob> job;
    SDL_TimerID timer = 0;
    std::string error;
    std::vector<std::shared_ptr<Profile>> done;  // the latest first
    std::shared_ptr<Profile> shown;               // whose times are in the code
    // The panel: what each row does, and the row chosen.
    enum Act : u8 { kNone, kProgram, kArgs, kCwd, kRecord, kStop, kDwarf, kAnnotate, kOpen };
    struct Row {
        Act act = kNone;
        u32 index = 0;
    };
    std::vector<Row> rows;
    u32 sel = 0;
    float tones[8][4] = {};  // the frames' colours (Popup::tones)
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

// ---------------------------------------------------------------- recording

void App::prof_record() {
    ProfUi& u = *profui_;
    if (u.job || u.program.empty()) return;
    ProfileRequest r;
    r.program = u.program;
    r.cwd = u.cwd;
    r.frequency = u.frequency;
    r.dwarf = u.dwarf;
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

// The recording has news: it has finished (its tab opens), or failed.
void App::prof_sync() {
    ProfUi& u = *profui_;
    ++prof_gen_;
    dirty_ = true;
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
    u.job.reset();
    if (!p) return;
    u.done.insert(u.done.begin(), p);
    if (u.done.size() > 20) u.done.resize(20);
    prof_open(p);
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
            if (pane->docs[i]->prof && pane->docs[i]->prof->p == p) {
                focus_pane(pane.get());
                tab_show(i);
                return;
            }
    Doc* d = add_doc(pane_->tab + 1);
    d->title = "Profile: " + short_path(p->program) + " " + p->when;
    d->read_only = true;
    auto v = std::make_shared<ProfView>();
    v->p = p;
    v->depth.assign(p->nodes.size(), 0);
    for (u32 n = 1; n < p->nodes.size(); ++n) {  // (a node comes after its parent)
        v->depth[n] = static_cast<u16>(v->depth[p->nodes[n].parent] + 1);
        v->deepest = std::max(v->deepest, v->depth[n]);
    }
    v->order = p->by_self();
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
    box.put(font, 1, 0, (tool.empty() ? std::string("Profiler") : tool == "perf" ? "perf" : "xctrace") + (remote ? " on " + host : ", local"), kInkMuted);
    u32 r = 2;
    auto field = [&](const char* label, const std::string& value, const char* none, ProfUi::Act act) {
        if (r + 1 >= rows) return;
        box.put(font, r, 0, label, kInkMuted);
        u.rows[r] = {act, 0};
        u.rows[r + 1] = {act, 0};
        std::string shown = value;
        if (const char* home = SDL_getenv("HOME"); home && *home && shown.starts_with(home)) shown = "~" + shown.substr(std::strlen(home));
        // (a long path loses its start)
        if (width_of(shown) + 2 > cols && cols > 4) {
            size_t at = 0;
            for (u32 cut = width_of(shown) + 3 - cols; cut > 0 && at < shown.size(); --cut) at += decode_utf8(reinterpret_cast<const u8*>(shown.data()) + at, shown.size() - at).len;
            shown = "\xE2\x80\xA6" + shown.substr(at);
        }
        box.put(font, r + 1, 2, value.empty() ? none : shown, value.empty() ? kInkMuted : 0, value.empty() ? Font::kItalic : 0);
        r += 2;
    };
    field("Program", u.program, "Choose a program\xE2\x80\xA6", ProfUi::kProgram);
    field("Arguments", u.args, "None", ProfUi::kArgs);
    field("Working directory", u.cwd, "The program's folder", ProfUi::kCwd);
    ++r;
    if (r < rows) {  // record, or stop
        const bool running = u.job != nullptr;
        u.rows[r] = {running ? ProfUi::kStop : ProfUi::kRecord, 0};
        u32 c = box.put(font, r, 1, running ? "\xE2\x96\xA0 Stop" : "\xE2\x97\x8F Record", running ? kInkDiagnostic : kInkAccent, Font::kBold);
        if (running) {
            char t[64];
            std::snprintf(t, sizeof t, "  %.1f s", u.job->elapsed());
            const ProfileJob::State st = u.job->state();
            box.put(font, r, c, st == ProfileJob::kRecording ? std::string(t) : "  " + u.job->status() + "\xE2\x80\xA6", kInkMuted);
        } else if (u.program.empty()) {
            box.put(font, r, c, "  (choose a program first)", kInkMuted);
        }
        ++r;
    }
    if (!u.error.empty() && r < rows) box.put(font, r++, 1, clip(u.error, cols - 1), kInkDiagnostic, Font::kItalic);
    ++r;
    if (r < rows) {
        u.rows[r] = {ProfUi::kDwarf, 0};
        // (How perf walks the stack: frame pointers, or DWARF for code built without them.)
        const u32 c = box.put(font, r, 1, "Call stack", 0);
        box.put(font, r++, c + 2, u.dwarf ? "DWARF" : "Frame pointers", kInkMuted);
    }
    if (r < rows) {
        u.rows[r] = {ProfUi::kAnnotate, 0};
        box.put(font, r++, 1, std::string(u.annotate ? "\xE2\x97\x89" : "\xE2\x97\x8B") + " Inline performance labels", u.annotate ? 0 : kInkMuted);
    }
    ++r;
    if (r < rows && !u.done.empty()) box.put(font, r++, 0, "Profiles", kInkMuted);
    for (u32 i = 0; i < u.done.size() && r < rows; ++i, ++r) {
        const Profile& p = *u.done[i];
        u.rows[r] = {ProfUi::kOpen, i};
        const bool on = u.shown == u.done[i];
        u32 c = box.put(font, r, 1, on ? "\xE2\x96\xB8 " : "  ", kInkAccent);
        c = box.put(font, r, c, short_path(p.program), 0, on ? Font::kBold : 0);
        char t[96];
        std::snprintf(t, sizeof t, "  %s  %.1f s", p.when.c_str(), p.seconds);
        box.put(font, r, c, t, kInkMuted);
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
    if (focused && u.sel < rows && u.rows[u.sel].act != ProfUi::kNone) {
        const ProfUi::Act a = u.rows[u.sel].act;
        for (u32 k = 0; k < rows; ++k)  // (both rows of a field)
            if (u.rows[k].act == a && u.rows[k].index == u.rows[u.sel].index && (k == u.sel || (a == ProfUi::kProgram || a == ProfUi::kArgs || a == ProfUi::kCwd))) box.select_row(k);
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
        case ProfUi::kAnnotate:
            u.annotate = !u.annotate;
            for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
            break;
        case ProfUi::kOpen:
            if (it.index < u.done.size()) prof_open(u.done[it.index]);
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

        u64 key = 1469598103934665603ull;
        auto mix = [&](u64 k) { key = (key ^ k) * 1099511628211ull; };
        mix(cols), mix(rows), mix(font.generation()), mix(v.zoom), mix(static_cast<u64>(v.hover + 1)), mix(v.sel), mix(v.top), mix(v.shift);
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
            std::snprintf(sum, sizeof sum, "%s samples \xC2\xB7 %.2f s \xC2\xB7 %u thread%s", with_commas(p.samples).c_str(), p.seconds, p.threads, p.threads == 1 ? "" : "s");
            c = box.put(font, 1, 1, sum, kInkMuted);
            if (!p.note.empty()) box.put(font, 1, c + 3, clip(p.note, cols > c + 4 ? cols - c - 4 : 1), kInkDiagnostic + 1, Font::kItalic);

            // Where in the graph the view is: the frames above the one zoomed into.
            const u32 top_rows = 3;
            {
                std::vector<u32> path;
                for (u32 n = v.zoom; n != 0; n = p.nodes[n].parent) path.push_back(n);
                std::string crumbs = "all";
                for (size_t k = path.size(); k-- > 0;) crumbs += " \xE2\x80\xBA " + std::string(short_name(p.funcs[p.nodes[path[k]].func].name));
                c = box.put(font, 2, 1, clip(crumbs, cols / 2), v.zoom ? 0 : kInkMuted);
                if (v.zoom) box.put(font, 2, c + 2, "Esc: zoom out", kInkMuted);
            }

            // The flame graph: the root at the bottom, each function above
            // what called it, as wide as its share of the time.
            const u32 list_min = std::min<u32>(8, rows / 3);
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
                if (static_cast<int>(node) == v.hover || (node != 0 && p.nodes[node].func == focus_func)) tone = kFocusTone;
                else if (ancestor || !f.own) tone = 1 + kOwnTones + hash_of(f.name) % kLibTones;
                else tone = 1 + hash_of(f.name) % kOwnTones;
                if (tone != kFocusTone && last_end[r] == c0 && last_layer[r] == tone)  // (two side by side of one tone would run into one)
                    tone = tone <= kOwnTones ? 1 + tone % kOwnTones : 1 + kOwnTones + (tone - kOwnTones) % kLibTones;
                last_end[r] = c1;
                last_layer[r] = tone;
                TextBox& l = v.box;
                if (c1 - c0 >= 4) {
                    const std::string label = node == 0 ? "all" : std::string(short_name(f.name));
                    l.put(font, r, c0 + 1, clip(label, c1 - c0 - 2), 0);
                }
                for (u32 k = c0; k < c1 && k < cols; ++k) l.cells[static_cast<size_t>(r) * cols + k] |= kCellSelected | tone << kCellDecoShift;
                v.hits.push_back({r, c0, c1, node});
            };
            // Above the frame zoomed into: what led to it, across the whole width.
            for (u32 n = v.zoom, d = zoom_depth; n != 0;) {
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
            if (head < rows) {
                box.put(font, head, 2, "Self", kInkMuted);
                box.put(font, head, 9, "Total", kInkMuted);
                box.put(font, head, name_col, "Function", kInkMuted);
                if (src_col < cols) box.put(font, head, src_col, "Source", kInkMuted);
            }
            for (u32 k = 0; k < v.list_rows && v.top + k < n; ++k) {
                const u32 r = v.list_row + k;
                const ProfileFunc& f = p.funcs[v.order[v.top + k]];
                const std::string self = percent(f.self, p.weight), total = percent(f.total, p.weight);
                box.put(font, r, 7 - std::min<u32>(7, width_of(self)), self, f.own && f.self ? kInkAccent : f.self ? 0 : kInkMuted, f.own && f.self ? Font::kBold : 0);
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
                    total = f->total, self = f->self;
                }
                if (f) {
                    about = f->name + "  \xC2\xB7  " + percent(total, p.weight) + " total, " + percent(self, p.weight) + " self";
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
        q.tones = &u.tones[0][0];
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
            if (v.zoom == 0) return false;
            v.zoom = p.nodes[v.zoom].parent;
            break;
        case SDLK_SPACE: {  // the graph zooms into the chosen function: where it took the most time
            if (v.sel >= n) break;
            const u32 func = v.order[v.sel];
            u32 best = 0;
            for (u32 node = 1; node < p.nodes.size(); ++node)
                if (p.nodes[node].func == func && (best == 0 || p.nodes[node].total > p.nodes[best].total)) best = node;
            v.zoom = v.zoom == best ? p.nodes[best].parent : best;  // (again: back out)
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
                box.put(font, 0, 0, t.first, t.second, Font::kItalic);
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
            if (l < g.top_line || l >= g.top_line + g.rows || l >= lines) continue;
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
    ++prof_gen_;
}

bool App::prof_busy() const { return profui_ && profui_->job; }

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
        out += "; " + short_path(p.program) + " by " + p.backend + (p.host.empty() ? "" : " on " + p.host) + ", " + std::to_string(p.threads) + " thread";
        std::vector<std::string> hot;
        for (const u32 f : p.by_self())
            if (p.funcs[f].self * 10 >= p.weight && hot.size() < 4) hot.push_back(std::string(short_name(p.funcs[f].name)) + (p.funcs[f].file.empty() ? "" : "@" + short_path(p.funcs[f].file)));
        std::ranges::sort(hot);
        out += ", over 10%:";
        for (const std::string& h : hot) out += " " + h;
    }
    if (pane_->doc->prof) {
        const ProfView& v = *pane_->doc->prof;
        out += "; tab: zoom " + std::string(v.zoom ? short_name(v.p->funcs[v.p->nodes[v.zoom].func].name) : "all");
    } else if (auto it = u.notes.find(pane_->doc->id); it != u.notes.end()) {
        out += "; times on lines";
        for (size_t k = 0; k < it->second.lines.size(); ++k) out += " " + std::to_string(it->second.lines[k].first + 1) + it->second.kinds[k];
    }
    return out;
}

}  // namespace blit
