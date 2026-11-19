// Jupyter notebooks: an .ipynb opened as the script of its cells (each
// after a "# %%" line, notebook.hpp), saved back as the notebook, its saved
// outputs shown after the cells they are of, in a line, and in full in the
// notebook's view beside it (appmarkdown.cpp, nbview.cpp); and a cell run
// in the Python console (Shift+Return in a notebook; "Run the cell" in any
// script with "# %%" cells), the cursor to the next.
//
// The cells as read (or last saved, or given outputs by whatever ran them:
// nb_set_outputs) are the notebook's model, shared with the view's worker:
// it is never changed, only replaced.
#include <algorithm>
#include <atomic>

#include "app.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "notebook.hpp"
#include "textbox.hpp"

namespace blit {

namespace {
std::atomic<u64> models{0};  // (each model made, counted: what the view has drawn is known by it)
}

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
};

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
    // (The model as the script's cells are now: each cell's own, or a new
    // one's with nothing; one taken away is let go of.)
    const std::vector<CellMatch> match = notebook_match(now, *nb.cells);
    std::vector<NotebookCell> model;
    for (size_t i = 0; i < now.size(); ++i) {
        if (match[i].at >= 0) {
            model.push_back((*nb.cells)[static_cast<size_t>(match[i].at)]);
        } else {
            model.emplace_back();
            model.back().kind = now[i].kind;
            model.back().source = now[i].source;
        }
    }
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

// The script written to the file as the notebook: a cell whose text is as
// it was keeps its outputs.
bool App::nb_save(Doc& d, std::string* err) {
    NotebookTab& nb = *d.nb;
    const std::string json = notebook_write(nb.original, d.ed.buf.text(0, d.ed.buf.size()), nb.cells.get());
    if (!fs_write(d.ed.path, json, err)) return false;
    nb.original = json;
    std::string script;
    std::vector<NotebookCell> cells;
    notebook_read(json, &cells, &script);
    nb.cells = std::make_shared<const std::vector<NotebookCell>>(std::move(cells));
    nb.gen = ++models;
    d.ed.mark_saved();
    return true;
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
    if (c.code && c.end > c.first) py_send(ed.buf.text(ed.buf.line_start(c.first), ed.line_end(c.end - 1) - ed.buf.line_start(c.first)));
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
        if (now[i].kind == "code") out += " [" + (m && m->count >= 0 ? std::to_string(m->count) : std::string(" ")) + "]";
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
