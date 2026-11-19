#pragma once
// Cursor, selection, movement, edits and undo on top of a Buffer.
// Knows nothing about windows, pixels or the GPU.
#include <algorithm>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.hpp"

namespace blit {

// One change to the text, in the terms an incremental parser wants: byte
// offsets, plus (row, column) with the column counted in bytes.
struct TextEdit {
    u64 start = 0, old_end = 0, new_end = 0;
    u32 start_row = 0, start_col = 0;
    u32 old_end_row = 0, old_end_col = 0;
    u32 new_end_row = 0, new_end_col = 0;
};

// Rows of a view that are not one line each (lines folded away under the
// line before them; long lines wrapped onto the rows under them): what
// moves up and down count in, set while there are any.
struct LineSkip {
    virtual u64 skip_row(u64 line) const = 0;   // the row a line is on (a hidden one: its fold's header's)
    virtual u64 skip_line(u64 row) const = 0;   // the line a row shows
    virtual u64 skip_rows() const = 0;          // how many rows the text takes
    // With rows that are parts of lines: the row a place is in, the column a
    // row starts at, and where the next row of its line starts (~0u: none).
    virtual u64 skip_row_at(u64 line, u32 /*col*/) const { return skip_row(line); }
    virtual u32 skip_col(u64 /*row*/) const { return 0; }
    virtual u32 skip_end(u64 /*row*/) const { return ~0u; }

protected:
    ~LineSkip() = default;
};

// Told about every edit: before it is applied (while the text to be removed
// can still be read) if it cares to know, and after.
struct EditListener {
    virtual void before_edit(u64 /*off*/, u64 /*remove_len*/) {}
    virtual void on_edit(const TextEdit& e) = 0;

protected:
    ~EditListener() = default;
};

// Told where the text changed, after every edit and every undo: the bytes
// [start, old_end) became [start, new_end). For whoever keeps places in
// the text (marks) that are to move with it.
struct EditWatch {
    virtual void edited(u64 start, u64 old_end, u64 new_end) = 0;

protected:
    ~EditWatch() = default;
};

class Editor {
public:
    Buffer buf;
    std::string path;
    u32 tab_width = 4;  // columns between tab stops
    EditWatch* watch = nullptr;
    // A second one, for the other splits that show this text: each keeps its
    // own cursor while one of them has the editor (see App::place_view()).
    EditWatch* views = nullptr;
    const LineSkip* skip = nullptr;  // (what is folded: up and down go by rows)

    // Listeners hear about edits (syntax highlighting, the language
    // server, the debugger's breakpoints, what is folded, and the soft wrap
    // of each split that shows the text), as many as there are, in the
    // order they started listening; one that asks to be first goes before
    // them all (what is folded: what else hears of an edit, the soft wrap,
    // goes by what the folds hide once the edit has moved them).
    void listen(EditListener* l, bool first = false) {
        unlisten(l);
        listeners_.insert(first ? listeners_.begin() : listeners_.end(), l);
    }
    void unlisten(EditListener* l) { std::erase(listeners_, l); }

    bool open(const char* file, std::string* err);
    // The file's bytes, read already (from another machine: see remote.hpp).
    void open_bytes(const std::string& file, std::string_view bytes);
    // (*modified_ns: when the file was written, for one on another machine.)
    bool save(std::string* err, i64* modified_ns = nullptr);
    bool modified() const { return state_id() != saved_id_; }
    bool crlf() const { return crlf_; }  // its lines end in "\r\n" (as its first line did)
    void set_crlf(bool on) { crlf_ = on; }  // (once they have all been made to: Enter puts that in)
    // Calls the text as it is now the saved state (for text that was put
    // there to be shown, not typed).
    void mark_saved() {
        if (!undo_.empty()) undo_.back().sealed = true;
        saved_id_ = state_id();
    }
    // A save made elsewhere (a copy of the text written on another thread,
    // while typing goes on): where the history is as the copy is taken (what
    // is typed next is a step of its own), and, once that copy has been
    // written, the text as it was there is the saved state.
    u64 save_point() {
        if (!undo_.empty()) undo_.back().sealed = true;
        return state_id();
    }
    void mark_saved_at(u64 point) { saved_id_ = point; }
    // Changes whenever the text changes; the renderer keys its caches on it.
    u64 revision() const { return revision_; }
    // Where the text changed, revision by revision (the last 64).
    const EditLog& edit_log() const { return edits_; }

    // The cursor, the selection's other end and the column vertical moves
    // keep: a view's own, for a text two splits show.
    struct Caret {
        u64 cursor = 0, anchor = 0;
        u32 want_col = ~0u;
    };
    Caret caret() const { return {cursor_, anchor_, want_col_}; }
    void set_caret(const Caret& c) {
        const u64 n = buf.size();
        cursor_ = c.cursor < n ? c.cursor : n;
        anchor_ = c.anchor < n ? c.anchor : n;
        want_col_ = c.want_col;
    }

    // Other carets than the one above (the primary, which the view follows
    // and the rest of the editor speaks of): each with its own selection.
    // They move with every edit as marks do; each_caret() runs a command at
    // every caret in turn, the primary among them, as one undo step, and
    // then those that meet are merged. An undo or a redo leaves only the
    // primary, as does opening a file.
    const std::vector<Caret>& carets() const { return carets_; }
    bool multi() const { return !carets_.empty(); }
    void add_caret(const Caret& c) {
        carets_.push_back(c);
        merge_carets();
    }
    // The caret given becomes the primary, the primary one of the others.
    void add_primary(const Caret& c) {
        carets_.push_back(caret());
        set_caret(c);
        merge_carets();
    }
    void clear_carets() { carets_.clear(); }

    // Ranges kept in step with the text (a snippet's tab stops): an insert
    // before one moves it, one at its end or inside it makes it longer, one
    // at its start moves it on (unless it is `sticky`, the stop being
    // filled in, which then takes it); what is taken out of one is gone from
    // it. An undo or a redo, or opening a file, drops them all.
    struct Span {
        u64 a = 0, b = 0;
        bool sticky = false;
    };
    std::vector<Span> spans;
    // Every caret, the primary among them, in the order of the text; *primary: which it is.
    std::vector<Caret> all_carets(size_t* primary = nullptr) const;
    template <class F>
    void each_caret(F&& f) {
        if (carets_.empty()) return f();
        begin_group();
        carets_.push_back(caret());
        const size_t primary = carets_.size() - 1;
        std::vector<size_t> order(carets_.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::sort(order, [&](size_t x, size_t y) { return carets_[x].cursor < carets_[y].cursor; });
        for (const size_t i : order) {
            active_ = static_cast<i64>(i);
            set_caret(carets_[i]);
            f();
            carets_[i] = caret();
        }
        active_ = -1;
        const Caret p = carets_[primary];
        carets_.erase(carets_.begin() + static_cast<std::ptrdiff_t>(primary));
        set_caret(p);
        merge_carets();
        end_group();
    }

    u64 cursor() const { return cursor_; }
    u64 anchor() const { return anchor_; }
    bool has_selection() const { return cursor_ != anchor_; }
    u64 sel_min() const { return cursor_ < anchor_ ? cursor_ : anchor_; }
    u64 sel_max() const { return cursor_ < anchor_ ? anchor_ : cursor_; }

    // Geometry in display columns (tabs expand, wide characters take two).
    u64 line_end(u64 line) const;  // offset of the line terminator, or size()
    u32 col_between(u64 line_start, u64 off) const;
    // The widest line of a text given as its pieces, in display columns.
    // Safe on another thread (the text behind a piece never changes); it
    // gives up early, with what it has, when asked to stop.
    static u64 widest_columns(std::span<const Piece> pieces, u32 tab_width, std::stop_token stop = {});
    // Columns per level of indentation, guessed from the first few thousand
    // lines: the tab width if lines start with tabs, else the commonest
    // step by which the leading spaces of one line exceed the last one's.
    // *tabs: whether lines are indented with tabs (also said of a text
    // with no indentation to go by: a tab is what the Tab key puts in).
    u32 guess_indent(bool* tabs = nullptr, bool* known = nullptr) const;  // (known: an indented line was seen)
    u32 col_of(u64 off) const;
    u64 off_at(u64 line, u32 col, bool nearest = false) const;
    // The next and the previous character: a grapheme cluster, so a letter
    // and the marks on it, or an emoji and what is joined to it, are one.
    u64 next_char(u64 off) const;
    u64 prev_char(u64 off) const;
    bool joins_back(u64 from, u32 before, u32 cp) const;  // (prev_char's rules that look back past one character)

    // Movement. `select` keeps the anchor in place.
    void move_to(u64 off, bool select);
    void left(bool select);
    void right(bool select);
    void up(bool select) { vertical(-1, select); }
    void down(bool select) { vertical(1, select); }
    void vertical(i64 lines, bool select);
    void word_left(bool select);
    void word_right(bool select);
    void home(bool select);
    void end(bool select);
    void doc_start(bool select) { move_to(0, select); }
    void doc_end(bool select) { move_to(buf.size(), select); }
    void select_all();
    void select_word(u64 off);
    void select_line(u64 off);

    // Edits. All of them replace the selection when there is one.
    void insert(std::string_view text);
    // Replaces [start, end) with text, as one undo step. (Undone, the
    // cursor goes back to where it was before, not to the range.)
    void replace(u64 start, u64 end, std::string_view text);
    void newline();
    void backspace();
    void del();
    void backspace_word();
    void delete_word();
    std::string selection_text() const { return buf.text(sel_min(), sel_max() - sel_min()); }
    bool undo();
    bool redo();
    // Where the text changed in the last undo() or redo(): the first place, if there were several.
    u64 changed_at() const { return changed_at_; }
    // Where the cursor was when the last step there is to undo began (what
    // undo() puts back); 0 if there is none.
    u64 step_cursor() const {
        size_t i = undo_.size();
        if (i == 0) return 0;
        const u64 group = undo_[--i].group;
        while (group && i > 0 && undo_[i - 1].group == group) --i;
        return undo_[i].cursor0;
    }
    // Edits made between begin_group() and end_group() are undone and
    // redone together, as one step (a command that changes several places,
    // everything typed in one go). The calls nest. `from`, if given, is
    // where the cursor counts as having been before them (where the command
    // they are began, before it moved the cursor): undone, they put it there.
    void begin_group(u64 from = ~0ull) {
        if (group_depth_++ > 0) return;
        group_from_ = undo_.size();
        group_cursor_ = from == ~0ull ? from : std::min(from, buf.size());
        if (!undo_.empty()) undo_.back().sealed = true;
    }
    void end_group() {
        if (group_depth_ == 0 || --group_depth_ > 0) return;
        if (undo_.size() > group_from_ + 1) {
            const u64 g = next_group_++;
            for (size_t i = group_from_; i < undo_.size(); ++i) undo_[i].group = g;
        }
        if (!undo_.empty()) undo_.back().sealed = true;
    }

private:
    void opened(const std::string& file);  // (after loading: the cursor and history start again)
    enum Kind : u8 { kOther, kTyping, kBackspace, kDelete };
    // One undo step: `cur_len` bytes at `off` are what this step put in the
    // document; `stash` is what it took out. Undo and redo both swap the two.
    struct Edit {
        u64 off = 0;
        u64 cur_len = 0;
        std::vector<Piece> stash;
        u64 cursor0 = 0, anchor0 = 0, cursor1 = 0;
        u64 id = 0;
        u64 group = 0;  // edits with the same number (not 0) are one undo step
        Kind kind = kOther;
        bool sealed = false;
        bool reversed = false;  // stash holds its pieces last-to-first
    };

    void apply(u64 off, u64 remove_len, std::string_view text, Kind kind);
    void swap_edit(Edit& e);
    TextEdit begin_edit(u64 off, u64 remove_len) const;
    void end_edit(TextEdit& e, u64 insert_len);
    u64 word_edge(u64 off, int dir) const;
    u64 cluster_edge(u64 off, u64 limit) const;
    u64 state_id() const { return undo_.empty() ? 0 : undo_.back().id; }
    u32 tab_stop(u32 col) const { return (col / tab_width + 1) * tab_width; }

    bool listening() const { return !listeners_.empty(); }
    void merge_carets();
    void shift_carets(u64 off, u64 remove_len, u64 insert_len);
    void shift_spans(u64 off, u64 remove_len, u64 insert_len);

    std::vector<EditListener*> listeners_;
    u64 cursor_ = 0;
    u64 anchor_ = 0;
    std::vector<Caret> carets_;
    i64 active_ = -1;  // (each_caret: the one of carets_ in cursor_ now, which edits do not move)
    u32 want_col_ = ~0u;  // column kept across vertical moves
    u64 revision_ = 0;
    EditLog edits_;
    // Where col_between() last stopped on a long line, to go on from there
    // (the cursor's column is asked for every key and every frame; on a line
    // of millions of columns, walking it from its start each time was most
    // of a key's cost), and an older place at least 4 KB before that one,
    // which Backspace leaves standing. Each holds through edits after it.
    struct ColMemo {
        u64 line_start = ~0ull, off = 0, revision = 0;
        u32 col = 0, tab_width = 0;
    };
    mutable ColMemo col_near_, col_far_;
    u64 next_id_ = 1;
    u64 saved_id_ = 0;
    bool crlf_ = false;
    std::vector<Edit> undo_;
    std::vector<Edit> redo_;
    u64 changed_at_ = 0;
    size_t group_from_ = 0;
    u64 group_cursor_ = ~0ull;  // begin_group()'s `from` (~0: none)
    u64 next_group_ = 1;
    int group_depth_ = 0;
};

}  // namespace blit
