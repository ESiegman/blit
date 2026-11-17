#pragma once
// What is folded in a document: runs of lines hidden under the line before
// them (the fold's header), and from that the map between the rows a view
// shows and the document's lines. With nothing folded the map is the lines
// themselves, and nothing here is looked at.
#include <utility>
#include <vector>

#include "buffer.hpp"
#include "editor.hpp"

namespace blit {

class Folds final : public EditListener, public LineSkip {
public:
    // The editor whose text it folds, for the moves up and down (LineSkip).
    void attach(const Editor* ed) { ed_ = ed; }
    u64 skip_row(u64 line) const override;
    u64 skip_line(u64 row) const override;
    u64 skip_rows() const override;

    // A fold, by where its header line starts and where its last hidden line
    // ends (its newline, or the end of the text). An edit inside it (its
    // header line or what it hides) moves its end; one across its edges
    // opens it.
    struct Fold {
        u64 head = 0, end = 0;
    };

    bool empty() const { return folds_.empty(); }
    const std::vector<Fold>& all() const { return folds_; }
    u64 generation() const { return gen_; }  // (changes whenever the folds do)

    // Hides lines header + 1 to last. False if there is nothing to hide.
    bool add(const Buffer& b, u64 header, u64 last);
    // Opens the folds whose header is this line; true if there were any.
    bool open_header(const Buffer& b, u64 line);
    // Opens every fold that hides this line; true if there was one.
    bool open_around(const Buffer& b, u64 line);
    void clear();
    void on_edit(const TextEdit& e) override;

    // The map, made again when the text or the folds have changed since.
    void sync(const Buffer& b, u64 revision) const;
    bool hidden(u64 line) const;
    bool header(u64 line) const;  // a fold's header (its lines hidden)
    u64 hidden_under(u64 line) const;  // how many lines a header hides (0: none)
    u64 row_of(u64 line) const;        // the row a line is on; a hidden one, its header's
    u64 line_at(u64 row) const;        // the line a row shows
    u64 rows(u64 lines) const { return lines - hidden_; }
    // The next line on show from `line` on in a direction (+1 or -1): a
    // hidden one gives way to the first line after its fold, or its header.
    u64 visible(u64 line, int dir, u64 lines) const;

private:
    std::vector<Fold> folds_;
    // The map, made from the folds as the text is now.
    mutable std::vector<std::pair<u64, u64>> runs_;  // hidden lines [first, last], merged, in order
    mutable std::vector<u64> before_;                // hidden lines before each run
    mutable u64 hidden_ = 0;
    mutable u64 synced_rev_ = ~0ull, synced_gen_ = ~0ull;
    u64 gen_ = 1;
    const Editor* ed_ = nullptr;
};

}  // namespace blit
