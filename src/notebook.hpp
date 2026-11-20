#pragma once
// A Jupyter notebook (.ipynb) as a script to edit, and back: each cell
// after a "# %%" line ("# %% [markdown]" and its lines commented for a
// Markdown cell, "# %% [raw]" for a raw one), as the percent format has it.
// Written back, a cell whose text is as it was keeps its outputs and the
// rest of what the notebook says of it; the notebook's own metadata stays.
//
// The cells read from a notebook are its model: each code cell's outputs,
// as Jupyter keeps them, and its count. A script's cells are matched to
// the model's (notebook_match): one with the same text has its outputs;
// one changed at the same place had them, but they are of other code (and
// are kept, as Jupyter keeps them, until it runs again). Whatever runs a
// cell gives the model its new outputs (notebook_set_outputs), and those
// are what is written back.
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

bool is_notebook_path(std::string_view path);

struct NotebookCell {
    std::string kind;    // "code", "markdown" or "raw"
    std::string source;  // its text, without the newline at its end (a code cell's: the text its outputs are of)
    // A code cell's outputs as the notebook keeps them: the JSON of their
    // list, as the file has it (shared: a copy of the model does not copy
    // its pictures). Null: none.
    std::shared_ptr<const std::string> outputs;
    i64 count = -1;      // its execution count (-1: none)
    std::string output;  // its outputs in a line (the last line of text, "[image/png]", the error)
    // (Whatever runs it: which cell it is, however the script changes
    // around it, 0 until it is run; and that it is waiting to run, or
    // running: In [*].)
    u64 key = 0;
    bool busy = false;
};

// The notebook's cells, and the script that shows them. False: not a notebook.
bool notebook_read(std::string_view json, std::vector<NotebookCell>* cells, std::string* script);
// The language its code is in, as its metadata says ("python" if it says none).
std::string notebook_language(std::string_view json);
// A code cell's outputs (Jupyter's JSON list of them) and its count put in
// it, its line made again. False: that is not a list (nothing changes).
bool notebook_set_outputs(NotebookCell* cell, std::string_view outputs, i64 count);
// A running cell's outputs, as a kernel sends them: each put after the
// others as Jupyter does (text printed to the same stream as the last
// joins it, and what a carriage return goes back over is gone from it).
// A stream's text is kept as text, so that each piece costs what it is,
// not what came before it.
class NotebookOutputs {
public:
    // False: that is not an output (nothing changes).
    bool add(std::string_view output);
    void clear() { outs_.clear(); }
    bool empty() const { return outs_.empty(); }
    // Jupyter's JSON list of them. A stream's text longer than `most`
    // bytes is cut to its first lines and its last, and says how many
    // lines it has ("blit_lines": the view's, never saved).
    std::string list(size_t most = SIZE_MAX) const;

private:
    struct Out {
        std::string json;          // an output that is not a stream's
        std::string stream, text;  // a stream's name and its text, as a terminal leaves it
        size_t line = 0;           // where its last line starts
        u64 lines = 0;             // and its newlines
    };
    std::vector<Out> outs_;
};
// A script's cells (the text before the first "# %%", if any, a code cell).
std::vector<NotebookCell> notebook_cells(std::string_view script);
// Each of a script's cells (`now`) matched to one of the model's: the first
// not taken of its kind with its text (`same`), else the one at its place if
// that is of its kind and not taken; -1: none (a new cell).
struct CellMatch {
    int at = -1;
    bool same = false;
};
std::vector<CellMatch> notebook_match(const std::vector<NotebookCell>& now, const std::vector<NotebookCell>& model);
// The script written as the notebook `original` (its text) with its cells:
// each with the outputs and count of its cell in `model`, its text the one
// they are of or changed since (no model: the cells of `original`).
std::string notebook_write(std::string_view original, std::string_view script, const std::vector<NotebookCell>* model = nullptr);
// The line (from 0) of each cell's "# %%" in a script, and where each ends
// (the line after its last), for the cells notebook_cells finds; and so the
// cell a line is in.
struct CellLines {
    u64 head = 0, first = 0, end = 0;  // its "# %%" line (== first: none), its first line, the line after its last
    bool code = true;
};
std::vector<CellLines> notebook_cell_lines(std::string_view script);

}  // namespace blit
