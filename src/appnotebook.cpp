// Jupyter notebooks: an .ipynb opened as the script of its cells (each
// after a "# %%" line, notebook.hpp), saved back as the notebook, its saved
// outputs shown after the cells they are of; and a cell run in the Python
// console (Shift+Return in a notebook; "Run the cell" in any script with
// "# %%" cells), the cursor to the next.
#include <algorithm>

#include "app.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "notebook.hpp"
#include "textbox.hpp"

namespace blit {

struct NotebookTab {
    std::string original;            // the notebook as last read or written
    std::vector<NotebookCell> cells;  // its cells (their saved outputs)
    // The notes after the cells on show, as of the text's revision.
    u64 noted_rev = ~0ull;
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
    if (!notebook_read(text, &nb->cells, &script)) return false;
    nb->original = text;
    d.ed.open_bytes(d.ed.path, script);
    d.ed.mark_saved();
    d.nb = std::move(nb);
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
    const std::string json = notebook_write(nb.original, d.ed.buf.text(0, d.ed.buf.size()));
    if (!fs_write(d.ed.path, json, err)) return false;
    nb.original = json;
    std::string script;
    notebook_read(json, &nb.cells, &script);
    nb.noted_rev = ~0ull;
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
        if (nb.noted_rev != d->ed.revision()) {
            nb.noted_rev = d->ed.revision();
            nb.notes.clear();
            nb.cells_of_notes.clear();
            nb.texts.clear();
            const std::string text = d->ed.buf.text(0, d->ed.buf.size());
            const std::vector<CellLines> lines = notebook_cell_lines(text);
            const std::vector<NotebookCell> now = notebook_cells(text);
            std::vector<bool> taken(nb.cells.size(), false);
            Font& font = *font_;
            for (size_t i = 0; i < now.size() && i < lines.size(); ++i) {
                if (now[i].kind != "code") continue;
                std::string said;
                for (size_t j = 0; j < nb.cells.size(); ++j)
                    if (!taken[j] && nb.cells[j].kind == "code" && nb.cells[j].source == now[i].source) {
                        taken[j] = true;
                        said = nb.cells[j].output;
                        break;
                    }
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

}  // namespace blit
