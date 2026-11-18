#pragma once
// A Jupyter notebook (.ipynb) as a script to edit, and back: each cell
// after a "# %%" line ("# %% [markdown]" and its lines commented for a
// Markdown cell, "# %% [raw]" for a raw one), as the percent format has it.
// Written back, a cell whose text is as it was keeps its outputs and the
// rest of what the notebook says of it; the notebook's own metadata stays.
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

bool is_notebook_path(std::string_view path);

struct NotebookCell {
    std::string kind;    // "code", "markdown" or "raw"
    std::string source;  // its text, without the newline at its end
    std::string output;  // a code cell's saved outputs, in a line (the last line of text, "[image/png]", the error)
};

// The notebook's cells, and the script that shows them. False: not a notebook.
bool notebook_read(std::string_view json, std::vector<NotebookCell>* cells, std::string* script);
// A script's cells (the text before the first "# %%", if any, a code cell).
std::vector<NotebookCell> notebook_cells(std::string_view script);
// The script written as the notebook `original` (its text) with its cells.
std::string notebook_write(std::string_view original, std::string_view script);
// The line (from 0) of each cell's "# %%" in a script, and where each ends
// (the line after its last), for the cells notebook_cells finds; and so the
// cell a line is in.
struct CellLines {
    u64 head = 0, first = 0, end = 0;  // its "# %%" line (== first: none), its first line, the line after its last
    bool code = true;
};
std::vector<CellLines> notebook_cell_lines(std::string_view script);

}  // namespace blit
