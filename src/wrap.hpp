#pragma once
// Soft wrap: a view's map between its rows and the document's lines when
// lines longer than the view's width go on in the rows under them, broken
// after the last space that fits (or, with none, where the width ends).
// Each row is a part of a line: the line, and the display column it starts
// at. Lines folded away (folds.hpp) take no rows. Kept for a width; made
// again line by line as the text changes (it hears each edit).
#include <vector>

#include "buffer.hpp"
#include "editor.hpp"

namespace blit {

class Folds;

class Wrap final : public EditListener, public LineSkip {
public:
    Wrap() = default;
    Wrap(const Wrap&) = delete;
    Wrap& operator=(const Wrap&) = delete;
    ~Wrap();

    // For an editor's text, `width` columns wide: made (again) when any of
    // these is other than what it was made for. It then hears the editor's
    // edits until it is let go of or made for another.
    void set(Editor* ed, u64 doc, const Folds* folds, u64 folds_gen, u32 width, u32 tab);
    void detach();
    u64 doc() const { return doc_; }
    u32 width() const { return width_; }

    u64 rows() const;                          // how many the text takes
    u64 row_of(u64 line) const;                // a line's first (a hidden one's: its fold header's)
    u64 line_at(u64 row) const;                // the line a row shows (past the last: the last)
    u32 start_col(u64 row) const;              // the display column a row starts at
    u32 end_col(u64 row) const;                // where the next row of its line starts (~0u: its line ends in it)
    u64 row_at(u64 line, u32 col) const;       // the row a place is shown in
    u32 rows_of(u64 line) const { return line < count_.size() ? count_[line] : 1; }
    // The rows from `top` on, n of them, walked once (for a view): each
    // one's line (~0 past the end), start column and end column.
    void walk(u64 top, u32 n, u64* lines, u32* cols, u32* ends) const;

    // Where a line's rows start (after its first, which starts at 0): the
    // breaks for `text` in `width` columns. (Public for the tests.)
    static void breaks(std::string_view text, u32 width, u32 tab, std::vector<u32>* out);

    // LineSkip: the editor's moves up and down go by rows, at the column
    // along the row.
    u64 skip_row(u64 line) const override { return row_of(line); }
    u64 skip_line(u64 row) const override { return line_at(row); }
    u64 skip_rows() const override { return rows(); }
    u64 skip_row_at(u64 line, u32 col) const override { return row_at(line, col); }
    u32 skip_col(u64 row) const override { return start_col(row); }
    u32 skip_end(u64 row) const override { return end_col(row); }

    void on_edit(const TextEdit& e) override;

private:
    void measure(u64 line);   // one line's rows and breaks, from the text
    void tree();              // the sums made again from the counts
    void add(u64 line, i64 by);
    u64 sum_before(u64 line) const;  // rows of the lines before it

    Editor* ed_ = nullptr;
    const Folds* folds_ = nullptr;
    u64 doc_ = 0, folds_gen_ = ~0ull;
    u32 width_ = 0, tab_ = 4;
    std::vector<u32> count_;                // each line's rows (0: folded away)
    std::vector<std::vector<u32>> starts_;  // each line's breaks (empty: one row)
    std::vector<u64> fen_;                  // a Fenwick tree of the counts (1-based)
};

}  // namespace blit
