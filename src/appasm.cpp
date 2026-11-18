// A file's assembly in a tab beside it ("Show the assembly"): made by its
// compiler (with the file's command from compile_commands.json, or plainly
// with cc or c++), made again each time the file is saved, and linked to it
// line by line: the lines of one that the other's cursor line is of are
// marked, and brought into view.
#include <algorithm>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "asm.hpp"
#include "fsio.hpp"
#include "remote.hpp"

namespace blit {

struct AsmTab {
    u64 source = 0;          // the file's document
    std::string path;        // its full path
    std::vector<u32> line;   // each of the tab's lines: the file's line it is of (from 1; 0: none)
    bool asking = false;     // being made
    bool again = false;      // (saved while it was: made again once it is)
};

// The assemblies being read, on threads of their own (a large file's is
// many megabytes): each tab's when it is read, for the main thread.
struct AsmWork {
    std::mutex mu;
    std::vector<std::pair<u64, AsmView>> done;
    std::vector<std::jthread> threads;  // (last: they end before what they write to goes)
    size_t reading = 0;  // (on the main thread)
};

namespace {

// The command that makes `file`'s assembly: its own from a
// compile_commands.json in its folder or one above it (or in a build folder
// of one), else the plain compiler for its kind. False: not a file that has one.
bool command_for(const std::string& file, const std::string& root, std::vector<std::string>* argv, std::string* dir) {
    for (std::string at = parent_of(file); !at.empty();) {
        std::vector<std::string> dbs{at + "/compile_commands.json"};
        for (const FsEntry& e : fs_list(at))
            if (e.dir && e.name.starts_with("build")) dbs.push_back(at + "/" + e.name + "/compile_commands.json");
        for (const std::string& db : dbs) {
            std::string text;
            if (fs_read(db, &text, 256u << 20) && asm_command(text, file, argv, dir)) return true;
        }
        if (at == root || at == "/") break;
        const std::string up = parent_of(at);
        if (up == at) break;
        at = up;
    }
    const std::string ext = file.substr(std::min(file.size(), file.find_last_of('.') + 1));
    const bool c = ext == "c", cpp = ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c++" || ext == "mm";
    if (!c && !cpp && ext != "m") return false;
    *argv = {c || ext == "m" ? "cc" : "c++", "-S", "-g", "-O2", "-fno-asynchronous-unwind-tables", "-o", "-", file};
    if (cpp) argv->insert(argv->begin() + 1, "-std=c++20");
    *dir = parent_of(file);
    return true;
}

}  // namespace

// The front file's assembly, in a tab in a split to its right (or that tab
// brought to the front, if there is one); from the assembly, back to its file.
void App::asm_open() {
    Doc& src = *pane_->doc;
    if (src.asmv) {
        Pane* p = nullptr;
        size_t at = 0;
        if (find_doc(src.asmv->source, &p, &at)) {
            focus_pane(p);
            tab_show(at);
        }
        return;
    }
    if (src.ed.path.empty() || is_remote(src.ed.path) || src.read_only) return;
    const std::string path = full_path(src.ed.path);
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->asmv && p->docs[i]->asmv->source == src.id) {
                Pane* const editing = pane_;
                pane_ = p.get();
                tab_show(i);
                focus_pane(editing);
                asm_ask(*p->docs[i]);
                return;
            }
    const u64 id = src.id;
    Pane* const editing = pane_;
    Pane* into = split_pane(editing, kEdgeRight);
    if (!into) into = editing;
    pane_ = into;
    Doc* d = add_doc(pane_->tab + 1);
    d->title = "Assembly: " + path.substr(path.find_last_of('/') + 1);
    d->read_only = true;
    d->asmv = std::make_shared<AsmTab>();
    d->asmv->source = id;
    d->asmv->path = path;
    for (size_t i = 0; i < pane_->docs.size(); ++i)
        if (pane_->docs[i].get() == d) tab_show(i);
    focus_pane(editing);
    asm_ask(*d);
}

// An assembly tab's text asked for (again), from its file as it is saved.
void App::asm_ask(Doc& d) {
    AsmTab& t = *d.asmv;
    if (t.asking) {
        t.again = true;
        return;
    }
    std::vector<std::string> argv;
    std::string dir;
    if (!command_for(t.path, build_root(), &argv, &dir)) {
        debug_tab_fill(d, "No compiler command for this file.", {0}, {4}, 0, 0);
        return;
    }
    t.asking = true;
    const u64 id = d.id;
    script_run(std::move(argv), dir, [this, id](int code, const std::string& out, const std::string& err) {
        Doc* const tab = find_doc(id);
        if (!tab || !tab->asmv) return;
        AsmTab& a = *tab->asmv;
        if (code != 0) {
            std::string why = err.empty() ? out : err;
            if (why.empty()) why = code < 0 ? "The compiler could not be run." : "The compiler failed.";
            a.asking = false;
            a.line.clear();
            debug_tab_fill(*tab, why, {0}, {4}, 0, 0);
            asm_seen_ = {};
            if (a.again) {
                a.again = false;
                asm_ask(*tab);
            }
            return;
        }
        if (!asm_work_) asm_work_ = std::make_shared<AsmWork>();
        ++asm_work_->reading;
        asm_work_->threads.emplace_back([w = asm_work_.get(), id, out, path = a.path] {
            AsmView v = read_asm(out, path);
            {
                std::lock_guard lock(w->mu);
                w->done.emplace_back(id, std::move(v));
            }
            SDL_Event ev{};
            ev.type = SDL_EVENT_USER;
            ev.user.code = 23;
            SDL_PushEvent(&ev);
        });
    });
}

// The assemblies read since last time: in their tabs.
void App::asm_take() {
    if (!asm_work_) return;
    AsmWork& w = *asm_work_;
    std::vector<std::pair<u64, AsmView>> done;
    {
        std::lock_guard lock(w.mu);
        done.swap(w.done);
    }
    w.reading -= std::min(w.reading, done.size());
    if (!w.reading) w.threads.clear();  // (each has ended, or is about to)
    for (auto& [id, v] : done) {
        Doc* const tab = find_doc(id);
        if (!tab || !tab->asmv) continue;
        AsmTab& a = *tab->asmv;
        a.asking = false;
        if (v.text.empty()) v.text = "(no code)", v.line = {0}, v.off = {0}, v.style = {4};
        a.line = std::move(v.line);
        debug_tab_fill(*tab, std::move(v.text), std::move(v.off), std::move(v.style), -1, 0);
        asm_seen_ = {};  // (the marks, and what is brought into view, worked out again)
        if (a.again) {
            a.again = false;
            asm_ask(*tab);
        }
    }
}

// A file saved here: its assembly tabs made again.
void App::asm_saved(const std::string& path) {
    if (path.empty()) return;
    const std::string full = full_path(path);
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (d->asmv && d->asmv->path == full) asm_ask(*d);
}

// The lines that go with the cursor's: in an assembly tab on show, those of
// its file's cursor line (when the file is the front tab); in the file,
// the line of the assembly's cursor line (when the assembly is). The first
// of them brought into view when the cursor goes to another line.
void App::asm_marks(FrameDesc& fd) {
    if (!pane_ || !pane_->doc || focus_view_ >= fd.view_count) return;
    Doc& front = *pane_->doc;
    const u64 cur = front.ed.buf.line_of(front.ed.cursor());
    Doc* tab = nullptr;  // the assembly tab, and the line it is about (from 1)
    u32 line = 0;
    if (front.asmv) {
        tab = &front;
        if (cur < front.asmv->line.size()) line = front.asmv->line[cur];
    } else if (!front.ed.path.empty()) {
        for (const std::unique_ptr<Pane>& p : panes_)
            if (p->doc && p->doc->asmv && p->doc->asmv->source == front.id) tab = p->doc;
        line = static_cast<u32>(cur + 1);
    }
    if (!tab) return;
    const AsmTab& t = *tab->asmv;
    const std::array<u64, 4> seen{front.id, cur, tab->ed.revision(), t.line.size()};
    const bool moved = seen != asm_seen_;
    asm_seen_ = seen;
    for (u32 vi = 0; vi < fd.view_count && vi < kMaxViews; ++vi) {
        GridSpec& g = fd.views[vi].grid;
        Doc* d = find_doc(g.doc);
        if (!d || !line) continue;
        std::vector<GridDeco>& out = asm_decos_[vi];
        out.assign(g.decos.begin(), g.decos.end());
        u64 first = ~0ull;
        if (d == tab) {
            for (size_t k = 0; k < t.line.size() && k < d->ed.buf.line_count(); ++k)
                if (t.line[k] == line) {
                    if (first == ~0ull) first = k;
                    if (k >= g.top_line && k < g.bottom()) out.push_back({k, 0, std::max<u32>(1, d->ed.col_between(d->ed.buf.line_start(k), d->ed.line_end(k))), kDecoMatch});
                }
        } else if (d->id == t.source && front.asmv) {
            const u64 k = line - 1;
            if (k < d->ed.buf.line_count()) {
                first = k;
                if (k >= g.top_line && k < g.bottom()) out.push_back({k, 0, std::max<u32>(1, d->ed.col_between(d->ed.buf.line_start(k), d->ed.line_end(k))), kDecoMatch});
            }
        } else {
            continue;
        }
        g.decos = out;
        fd.views[vi].match_radius = look().sel;
        // (Brought into view in the other split, a third of the way down.)
        if (!moved || first == ~0ull || vi == focus_view_ || (first >= g.top_line && first < g.bottom())) continue;
        for (const std::unique_ptr<Pane>& p : panes_) {
            if (p->doc != d || p.get() == pane_) continue;
            Pane* const keep = pane_;
            pane_ = p.get();
            const double ch = font_->cell_h(), vh = view_h();
            pane_->target_y = std::clamp(static_cast<double>(row_of_line(first)) * ch - vh / 3, 0.0, std::max(0.0, max_scroll_y()));
            if (pane_->scroll_y != pane_->target_y) {
                if (!animating_) last_step_ns_ = now_ns() - 8'000'000;
                animating_ = true;
            }
            pane_ = keep;
        }
        dirty_ = true;
    }
}

// For the tests: the assembly tab's lines that go with the cursor's line,
// as "file line: assembly lines (from 1)", or (none made) what the tab says.
std::string App::asm_dump() {
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs) {
            if (!d->asmv) continue;
            const AsmTab& t = *d->asmv;
            if (t.asking) return "asking";
            Doc* src = find_doc(t.source);
            if (!src) return "no file";
            if (t.line.empty()) return d->ed.buf.text(0, d->ed.line_end(0));  // (what was said instead)
            const u32 line = static_cast<u32>(src->ed.buf.line_of(src->ed.cursor()) + 1);
            std::string out = std::to_string(line) + ":";
            for (size_t k = 0; k < t.line.size(); ++k)
                if (t.line[k] == line) out += " " + std::to_string(k + 1);
            return out;
        }
    return "no assembly";
}

}  // namespace blit
