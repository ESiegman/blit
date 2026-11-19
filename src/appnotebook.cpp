// Jupyter notebooks: an .ipynb opened as the script of its cells (each
// after a "# %%" line, notebook.hpp), saved back as the notebook, its saved
// outputs shown after the cells they are of, in a line, and in full in the
// notebook's view beside it (appmarkdown.cpp, nbview.cpp); and a cell run
// (Shift+Return in a notebook; "Run the cell" in any script with "# %%"
// cells), the cursor to the next: a notebook's in its kernel, any other
// script's in the Python console.
//
// The cells as read (or last saved, or given outputs by whatever ran them:
// nb_set_outputs, the kernel) are the notebook's model, shared with the
// view's worker: it is never changed, only replaced.
//
// A notebook's kernel (kernel.hpp) is started the first time one of its
// cells runs, and ends with its tab. Cells wait their turn, and run one at
// a time, with the code they had when they were run; the model's cell for
// one is known by a key, however the script changes meanwhile. When it
// starts its outputs go, as Jupyter's do, and what it gives comes into the
// model as it comes; the notebook has changed. An error (an interrupt is
// one) and the cells still waiting do not run, as in Jupyter.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <deque>

#include "app.hpp"
#include "fsio.hpp"
#include "kernel.hpp"
#include "look.hpp"
#include "notebook.hpp"
#include "remote.hpp"
#include "textbox.hpp"

namespace blit {

namespace {
std::atomic<u64> models{0};  // (each model made, counted: what the view has drawn is known by it)

// A kernel has said something (nb_sync takes it).
void kernel_wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 26;
    SDL_PushEvent(&ev);
}

// The model as the script's cells are now: each cell's own (as
// notebook_match pairs them), or a new one's with nothing; one taken away
// is let go of.
std::vector<NotebookCell> aligned(const std::vector<NotebookCell>& now, const std::vector<NotebookCell>& model) {
    const std::vector<CellMatch> match = notebook_match(now, model);
    std::vector<NotebookCell> out;
    out.reserve(now.size());
    for (size_t i = 0; i < now.size(); ++i) {
        if (match[i].at >= 0) {
            out.push_back(model[static_cast<size_t>(match[i].at)]);
        } else {
            out.emplace_back();
            out.back().kind = now[i].kind;
            out.back().source = now[i].source;
        }
    }
    return out;
}
}  // namespace

struct NotebookTab {
    std::string original;  // the notebook as last read or written
    std::shared_ptr<const std::vector<NotebookCell>> cells = std::make_shared<const std::vector<NotebookCell>>();  // its cells and their outputs
    u64 gen = ++models;    // (counts up as they change)
    std::string lang = "python";  // its code's language
    // The notes after the cells on show, as of the text's revision and the
    // model's (and the font's generation: the cells hold its glyphs).
    u64 noted_rev = ~0ull, noted_gen = ~0ull;
    u32 noted_font = ~0u;
    std::vector<GridNote> notes;
    std::vector<std::vector<u32>> cells_of_notes;
    std::vector<std::string> texts;  // (for the tests: "line: what")
    // Its kernel (null: none yet, or it has ended), what it is (IPython or
    // Python, its version); the cells waiting their turn (their keys, and
    // the code they were run with); the one running, and what it has given
    // so far (its count, its outputs, and that they are to go when the next
    // comes: clear_output(wait=True)).
    std::unique_ptr<Kernel> kernel;
    bool ipython = false;
    std::string version;
    struct Run {
        u64 key = 0;
        std::string code;
    };
    std::deque<Run> waiting;
    Run running;
    bool started = false;
    i64 count = -1;
    std::vector<std::string> outs;
    bool clear_next = false;
    u64 keys = 0;
};

namespace {

// The model made again, as the script's cells are now, each cell then as
// `each` makes it.
template <class F>
void remodel(Doc& d, F each) {
    NotebookTab& nb = *d.nb;
    std::vector<NotebookCell> model = aligned(notebook_cells(d.ed.buf.text(0, d.ed.buf.size())), *nb.cells);
    for (NotebookCell& c : model) each(c);
    nb.cells = std::make_shared<const std::vector<NotebookCell>>(std::move(model));
    nb.gen = ++models;
}

// The running cell's outputs so far, and its count, put in its cell of the
// model (with the code they are of); the notebook has changed.
void put_running(Doc& d, bool busy) {
    NotebookTab& nb = *d.nb;
    std::string list = "[";
    for (const std::string& o : nb.outs) list += (list.size() > 1 ? "," : "") + o;
    list += "]";
    remodel(d, [&](NotebookCell& c) {
        if (c.key != nb.running.key) return;
        c.source = nb.running.code;
        notebook_set_outputs(&c, list, nb.count);
        c.busy = busy;
    });
    d.ed.mark_saved_at(~0ull);  // (never the saved state again, until it is saved)
}

// The cells waiting their turn let go of (an error, an interrupt, the kernel gone).
void drop_waiting(Doc& d) {
    NotebookTab& nb = *d.nb;
    if (nb.waiting.empty()) return;
    nb.waiting.clear();
    remodel(d, [&](NotebookCell& c) {
        if (c.key != nb.running.key) c.busy = false;
    });
}

}  // namespace

// A notebook's text (as read from its file) put in its tab as the script
// of its cells. False: it is not a notebook (it stays as it is).
bool App::nb_load(Doc& d) {
    const std::string text = d.ed.buf.text(0, d.ed.buf.size());
    auto nb = std::make_shared<NotebookTab>();
    std::string script;
    std::vector<NotebookCell> cells;
    if (!notebook_read(text, &cells, &script)) return false;
    nb->cells = std::make_shared<const std::vector<NotebookCell>>(std::move(cells));
    nb->original = text;
    nb->lang = notebook_language(text);
    if (d.nb) {  // (read again: its kernel goes on, what it is running with it)
        NotebookTab& was = *d.nb;
        nb->kernel = std::move(was.kernel);
        nb->ipython = was.ipython, nb->version = std::move(was.version);
        nb->waiting = std::move(was.waiting), nb->running = std::move(was.running);
        nb->started = was.started, nb->count = was.count, nb->outs = std::move(was.outs), nb->clear_next = was.clear_next, nb->keys = was.keys;
    }
    d.ed.open_bytes(d.ed.path, script);
    d.ed.mark_saved();
    d.nb = std::move(nb);
    return true;
}

// The notebook's model, for its view: how many times it has changed, and
// (unless null) its cells and its language. False: not a notebook.
bool App::nb_model(const Doc& d, u64* gen, std::shared_ptr<const std::vector<NotebookCell>>* cells, std::string* lang) const {
    if (!d.nb) return false;
    *gen = d.nb->gen;
    if (cells) *cells = d.nb->cells;
    if (lang) *lang = d.nb->lang;
    return true;
}

// A code cell of the script (by its place, from 0) given outputs (Jupyter's
// JSON list of them) and its count, as something that ran it says: the
// model's cell for it now has its text as it is, and they are what the
// view shows and what is saved. False: no such code cell, or that is not a
// list of outputs.
bool App::nb_set_outputs(Doc& d, size_t cell, std::string_view outputs, i64 count) {
    if (!d.nb) return false;
    NotebookTab& nb = *d.nb;
    const std::vector<NotebookCell> now = notebook_cells(d.ed.buf.text(0, d.ed.buf.size()));
    if (cell >= now.size() || now[cell].kind != "code") return false;
    std::vector<NotebookCell> model = aligned(now, *nb.cells);
    model[cell].source = now[cell].source;
    if (!notebook_set_outputs(&model[cell], outputs, count)) return false;
    nb.cells = std::make_shared<const std::vector<NotebookCell>>(std::move(model));
    nb.gen = ++models;
    dirty_ = true;
    return true;
}

// A new notebook (an .ipynb that is not JSON yet: being made here) saved
// as one from now on.
bool App::nb_new(Doc& d) {
    const std::string head = d.ed.buf.text(0, std::min<u64>(d.ed.buf.size(), 4096));
    const size_t at = head.find_first_not_of(" \t\r\n");
    if (at != std::string::npos && head[at] == '{') return false;
    d.nb = std::make_shared<NotebookTab>();
    return true;
}

// The script written to the file as the notebook: each cell with its
// outputs (one changed since it ran too, as Jupyter keeps them). The model
// stays as it is (as the script's cells are now): what was run keeps its
// keys, and a cell changed since it ran is still said to be.
bool App::nb_save(Doc& d, std::string* err) {
    NotebookTab& nb = *d.nb;
    const std::string json = notebook_write(nb.original, d.ed.buf.text(0, d.ed.buf.size()), nb.cells.get());
    if (!fs_write(d.ed.path, json, err)) return false;
    nb.original = json;
    remodel(d, [](NotebookCell&) {});
    d.ed.mark_saved();
    return true;
}

// The notebook as it is to be written (to another machine, on a thread:
// appremote.cpp), and from now on what it was.
std::string App::nb_json(Doc& d) {
    NotebookTab& nb = *d.nb;
    nb.original = notebook_write(nb.original, d.ed.buf.text(0, d.ed.buf.size()), nb.cells.get());
    remodel(d, [](NotebookCell&) {});
    return nb.original;
}

// The cell the cursor is in to the Python console (a Markdown cell's
// text is not code: only the cursor moves), and the cursor to the next
// cell's first line. False: the text has no "# %%" cells.
bool App::nb_run_cell() {
    Doc& d = *pane_->doc;
    if (d.term || d.read_only) return false;
    Editor& ed = d.ed;
    const std::string text = ed.buf.text(0, ed.buf.size());
    const std::vector<CellLines> cells = notebook_cell_lines(text);
    if (cells.empty() || (cells.size() == 1 && cells[0].head == cells[0].first)) return false;
    const u64 line = ed.buf.line_of(ed.cursor());
    size_t k = 0;
    while (k + 1 < cells.size() && cells[k + 1].head <= line) ++k;
    const CellLines& c = cells[k];
    if (c.code && c.end > c.first) {
        if (d.nb) nb_queue(d, {k});  // (a notebook's: in its kernel)
        else py_send(ed.buf.text(ed.buf.line_start(c.first), ed.line_end(c.end - 1) - ed.buf.line_start(c.first)));
    }
    if (k + 1 < cells.size()) {
        const u64 next = std::min<u64>(cells[k + 1].first, ed.buf.line_count() - 1);
        if (cfg_.modal) vim_attach();
        ed.move_to(ed.buf.line_start(next), false);
        scroll_to_cursor();
    }
    dirty_ = true;
    return true;
}

// Shift+Return in a notebook: its cell run.
bool App::nb_key(const SDL_KeyboardEvent& k) {
    if (k.key != SDLK_RETURN && k.key != SDLK_KP_ENTER) return false;
    if ((k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) || !(k.mod & SDL_KMOD_SHIFT)) return false;
    if (!pane_->doc->nb || find_open_ || panel_has_keyboard()) return false;
    if (cfg_.modal && vim_.mode() == Vim::kCommand) return false;
    return nb_run_cell();
}

// ---------------------------------------------------------------- the kernel

// Code cells of a notebook's script (by their places) to run, after those
// waiting: each said to be busy (In [*]), with its code as it is now.
void App::nb_queue(Doc& d, const std::vector<size_t>& cells) {
    NotebookTab& nb = *d.nb;
    const std::vector<NotebookCell> now = notebook_cells(d.ed.buf.text(0, d.ed.buf.size()));
    std::vector<NotebookCell> model = aligned(now, *nb.cells);
    bool any = false;
    for (const size_t i : cells) {
        if (i >= now.size() || now[i].kind != "code" || now[i].source.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        NotebookCell& c = model[i];
        if (!c.key) c.key = ++nb.keys;
        c.busy = true;
        nb.waiting.push_back({c.key, now[i].source});
        any = true;
    }
    if (!any) return;
    nb.cells = std::make_shared<const std::vector<NotebookCell>>(std::move(model));
    nb.gen = ++models;
    dirty_ = true;
    nb_next(d);
}

// A notebook's kernel started: with the Python the console would have, in
// the folder being worked in if the notebook is in it, else the notebook's
// own; working in the notebook's folder. False: it could not be (said).
bool App::nb_start(Doc& d) {
    NotebookTab& nb = *d.nb;
    const std::string path = d.ed.path.empty() ? std::string() : is_remote(d.ed.path) ? d.ed.path : full_path(d.ed.path);
    const std::string folder = path.empty() ? default_workdir() : parent_of(path);
    const std::string* root = panel_root();
    const std::string dir = root && path.starts_with(*root + "/") ? *root : folder;
    auto k = std::make_unique<Kernel>();
    std::string err;
    // (On another machine the Python is chosen there.)
    if (!k->start(is_remote(dir) ? std::string() : python_for(dir, false), dir, folder, kernel_wake, &err)) {
        notify("The kernel could not start: " + err, 2);
        return false;
    }
    nb.kernel = std::move(k);
    nb.ipython = false;
    nb.version.clear();
    return true;
}

// The next cell waiting sent to the kernel, if none is running (the kernel
// started, if it has not been).
void App::nb_next(Doc& d) {
    NotebookTab& nb = *d.nb;
    if (nb.running.key || nb.waiting.empty()) return;
    if (!nb.kernel && !nb_start(d)) return drop_waiting(d);
    nb.running = std::move(nb.waiting.front());
    nb.waiting.pop_front();
    nb.started = false;
    nb.outs.clear();
    nb.clear_next = false;
    nb.kernel->run(nb.running.key, nb.running.code);
}

// What the kernels have said, put in their notebooks: a cell started (its
// outputs gone), its outputs as they come, and its end (and the next sent);
// a kernel that ended without being asked to, said.
void App::nb_sync() {
    for (Doc* dp : open_docs()) {
        if (!dp->nb || !dp->nb->kernel) continue;
        Doc& d = *dp;
        NotebookTab& nb = *d.nb;
        const std::vector<Kernel::Event> events = nb.kernel->poll();
        if (events.empty()) continue;
        bool touched = false;  // (the running cell's outputs, to put in the model)
        for (const Kernel::Event& e : events) {
            const bool its = nb.running.key && e.cell == nb.running.key;
            switch (e.kind) {
                case Kernel::Event::kReady:
                    nb.ipython = e.flag;
                    nb.version = e.text;
                    break;
                case Kernel::Event::kStart:
                    if (!its) break;
                    nb.started = true;
                    nb.count = e.count;
                    nb.outs.clear();
                    touched = true;
                    break;
                case Kernel::Event::kOutput:
                    if (!its) break;
                    if (nb.clear_next) nb.outs.clear(), nb.clear_next = false;
                    touched |= notebook_add_output(&nb.outs, e.text);
                    break;
                case Kernel::Event::kClear:
                    if (!its) break;
                    if (e.flag) nb.clear_next = true;
                    else nb.outs.clear(), touched = true;
                    break;
                case Kernel::Event::kDone:
                    if (!its) break;
                    nb.count = e.count;
                    put_running(d, false);
                    touched = false;
                    nb.running = {};
                    if (!e.flag) drop_waiting(d);  // (as Jupyter: after an error, nothing more)
                    break;
                case Kernel::Event::kEnded: {
                    // (Said with the last line of what it wrote as errors; all of it in the messages.)
                    std::string why = e.text;
                    while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
                    const size_t nl = why.rfind('\n');
                    const std::string said = "The kernel has stopped" + (why.empty() ? std::string() : ": " + why.substr(nl == std::string::npos ? 0 : nl + 1));
                    notify(said, 2, nl == std::string::npos ? std::string() : said + "\n" + why);
                    if (nb.running.key) put_running(d, false), touched = false;
                    nb.running = {};
                    drop_waiting(d);
                    break;
                }
            }
        }
        if (touched) put_running(d, true);
        if (nb.kernel->ended()) nb.kernel.reset();  // (started again when a cell next runs)
        dirty_ = true;
        nb_next(d);
    }
}

// "Run all the cells": each code cell of the notebook in turn.
void App::nb_run_all() {
    Doc* d = nb_of(*pane_->doc);
    if (!d) return notify("Not a notebook");
    std::vector<size_t> all;
    const std::vector<NotebookCell> now = notebook_cells(d->ed.buf.text(0, d->ed.buf.size()));
    for (size_t i = 0; i < now.size(); ++i) all.push_back(i);
    nb_queue(*d, all);
}

// "Interrupt the kernel": Ctrl+C in the cell running (the cells waiting do
// not run: it ends in an error).
void App::nb_interrupt() {
    Doc* d = nb_of(*pane_->doc);
    if (!d) return notify("Not a notebook");
    if (d->nb->kernel && d->nb->running.key) d->nb->kernel->interrupt();
}

// "Restart the kernel": it ends (what it was running and what was waiting
// with it; their outputs so far stay) and starts again, its counts from 1.
void App::nb_restart() {
    Doc* d = nb_of(*pane_->doc);
    if (!d) return notify("Not a notebook");
    NotebookTab& nb = *d->nb;
    if (nb.kernel) nb.kernel->stop();
    nb.kernel.reset();
    if (nb.running.key) put_running(*d, false);
    nb.running = {};
    drop_waiting(*d);
    nb_start(*d);
    dirty_ = true;
}

// A notebook's tab closed: its kernel ends.
void App::nb_forget(const Doc& d) {
    if (!d.nb || !d.nb->kernel) return;
    d.nb->kernel->stop();
    d.nb->kernel.reset();
    d.nb->running = {};
    d.nb->waiting.clear();
}

// For the tests: until no notebook's kernel is running a cell or has one
// waiting, and each has said it is ready (or, `started`, until a cell is
// running and has given an output); 30 s at most. False: it did not come to that.
bool App::nb_kernel_wait(bool started) {
    const u64 until = now_ns() + 30'000'000'000ull;
    for (;;) {
        nb_sync();
        bool done = !started;
        for (Doc* d : open_docs()) {
            if (!d->nb) continue;
            const NotebookTab& nb = *d->nb;
            if (started && nb.running.key && nb.started && !nb.outs.empty()) done = true;
            if (!started && (nb.running.key || !nb.waiting.empty() || (nb.kernel && nb.kernel->pid() < 0))) done = false;
        }
        if (done) break;
        if (now_ns() >= until) return false;
        SDL_Delay(5);
    }
    frame();
    return true;
}

// For the tests: the kernel of the notebook in front: what it is, its
// process, whether a cell is running, and how many wait.
std::string App::nb_kernel_dump() {
    Doc* d = nb_of(*pane_->doc);
    if (!d) return "not a notebook";
    const NotebookTab& nb = *d->nb;
    if (!nb.kernel) return "no kernel";
    std::string out = std::string(nb.ipython ? "ipython" : "python") + " pid " + std::to_string(nb.kernel->pid());
    out += nb.running.key ? std::string(nb.started ? " running" : " sent") : std::string(" idle");
    if (!nb.waiting.empty()) out += " " + std::to_string(nb.waiting.size()) + " waiting";
    return out;
}

// The saved outputs of the cells on show (those whose text is as it was
// saved), each after its cell's last line.
void App::nb_marks(FrameDesc& fd) {
    for (u32 vi = 0; vi < fd.view_count && vi < kMaxViews; ++vi) {
        GridSpec& g = fd.views[vi].grid;
        Doc* d = find_doc(g.doc);
        if (!d || !d->nb) continue;
        NotebookTab& nb = *d->nb;
        if (nb.noted_rev != d->ed.revision() || nb.noted_gen != nb.gen || nb.noted_font != font_->generation()) {
            nb.noted_rev = d->ed.revision();
            nb.noted_gen = nb.gen;
            nb.noted_font = font_->generation();
            nb.notes.clear();
            nb.cells_of_notes.clear();
            nb.texts.clear();
            const std::string text = d->ed.buf.text(0, d->ed.buf.size());
            const std::vector<CellLines> lines = notebook_cell_lines(text);
            const std::vector<NotebookCell> now = notebook_cells(text);
            const std::vector<CellMatch> match = notebook_match(now, *nb.cells);
            Font& font = *font_;
            for (size_t i = 0; i < now.size() && i < lines.size(); ++i) {
                if (now[i].kind != "code" || !match[i].same) continue;
                std::string said = (*nb.cells)[static_cast<size_t>(match[i].at)].output;
                if (said.empty()) continue;
                const u64 at = lines[i].end > lines[i].first ? lines[i].end - 1 : lines[i].head;
                said = "\xE2\x86\x92 " + said;  // →
                TextBox box;
                box.resize(std::min<u32>(width_of(said), 120), 1);
                put(box, font, 0, 0, said, ink::kMuted);
                nb.cells_of_notes.push_back(std::move(box.cells));
                nb.texts.push_back(std::to_string(at + 1) + ": " + said.substr(4));
                const u64 start = d->ed.buf.line_start(at);
                nb.notes.push_back({at, d->ed.col_between(start, d->ed.line_end(at)) + 3, nullptr, static_cast<u32>(nb.cells_of_notes.back().size())});
            }
            for (size_t k = 0; k < nb.notes.size(); ++k) nb.notes[k].cells = nb.cells_of_notes[k].data();
        }
        if (nb.notes.empty()) continue;
        std::vector<GridNote>& notes = nb_notes_[vi];
        notes.assign(g.notes.begin(), g.notes.end());
        for (const GridNote& n : nb.notes)
            if (std::ranges::none_of(g.notes, [&](const GridNote& o) { return o.line == n.line; })) notes.push_back(n);
        g.notes = notes;
    }
}

// For the tests: the notebook in front's notes, or that it is not one.
std::string App::nb_dump() {
    const Doc& d = *pane_->doc;
    if (!d.nb) return "not a notebook";
    std::string out;
    for (const std::string& t : d.nb->texts) out += (out.empty() ? "" : " | ") + t;
    return out.empty() ? "no outputs" : out;
}

// For the tests: the cells of the notebook in front, as its model has them,
// a line each: its kind, its count, whether its text is the one its outputs
// are of (same), was changed since (changed), or is new, and its outputs'
// kinds.
std::string App::nb_cells_dump() {
    const Doc& d = *pane_->doc;
    if (!d.nb) return "not a notebook";
    const std::vector<NotebookCell> now = notebook_cells(d.ed.buf.text(0, d.ed.buf.size()));
    const std::vector<CellMatch> match = notebook_match(now, *d.nb->cells);
    std::string out;
    for (size_t i = 0; i < now.size(); ++i) {
        const NotebookCell* m = match[i].at >= 0 ? &(*d.nb->cells)[static_cast<size_t>(match[i].at)] : nullptr;
        out += std::to_string(i + 1) + " " + now[i].kind;
        if (now[i].kind == "code") out += " [" + (m && m->busy ? std::string("*") : m && m->count >= 0 ? std::to_string(m->count) : std::string(" ")) + "]";
        out += !m ? " new" : match[i].same ? " same" : " changed";
        if (m && m->outputs)
            for (size_t at = 0; (at = m->outputs->find("\"output_type\": \"", at)) != std::string::npos;) {
                at += 16;
                out += " " + m->outputs->substr(at, m->outputs->find('"', at) - at);
            }
        out += "\n";
    }
    return out;
}

}  // namespace blit
