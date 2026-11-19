#include "wrap.hpp"

#include <algorithm>
#include <cmath>

#include "folds.hpp"
#include "utf8.hpp"

namespace blit {

Wrap::~Wrap() { detach(); }

void Wrap::detach() {
    if (ed_) ed_->unlisten(this);
    ed_ = nullptr;
    doc_ = 0;
    count_.clear();
    starts_.clear();
    fen_.clear();
    notes_.clear();
    hints_key_ = ~0ull;
}

namespace {

// Wrap::breaks(), with notes or without (the loop as plain as it can be
// then: a long line is measured again on every key typed in it).
template <bool kNotes>
void line_breaks(std::string_view t, u32 width, u32 tab, std::vector<u32>* out, std::span<const Wrap::Hint> notes) {
    out->clear();
    if (width < 2) return;
    u32 col = 0, row_start = 0, after_space = 0;  // (after_space: where a row may start, past spaces; 0: nowhere yet)
    // With notes: the columns they have taken so far, and as that was past
    // the last space; and what the row's room is counted from, where it
    // starts less the notes in it before here (what was taken at its start,
    // less what the notes have taken).
    u64 extra = 0, space_extra = 0;
    i64 base = 0;
    size_t k = 0;  // (the next note)
    u64 next = notes.empty() ? ~0ull : notes[0].off;
    auto fits = [&](u64 w) { return static_cast<i64>(col + w) - base <= static_cast<i64>(width); };
    auto new_row = [&](u64 here) {  // (here: what the notes had taken before this column's)
        if (after_space > row_start) row_start = after_space, base = static_cast<i64>(after_space + space_extra) - static_cast<i64>(extra);
        else row_start = col, base = static_cast<i64>(col + here) - static_cast<i64>(extra);
        out->push_back(row_start);
    };
    Joiner join;
    const u8* p = reinterpret_cast<const u8*>(t.data());
    for (size_t i = 0; i < t.size();) {
        const u64 here = extra;
        if (kNotes && i >= next) [[unlikely]] {  // (the notes before this character go on its row: one that does not fit starts a row)
            for (; k < notes.size() && notes[k].off <= i; ++k) {
                while (!fits(notes[k].width) && col > row_start) new_row(here);
                extra += notes[k].width, base -= notes[k].width;
            }
            next = k < notes.size() ? notes[k].off : ~0ull;
        }
        u32 w;
        bool space = false;
        size_t len = 1;
        if (p[i] == '\t') {
            w = tab - col % std::max<u32>(1, tab);
            space = true;
            join.reset();
        } else if (p[i] < 0x80) {
            w = p[i] == '\r' ? 0 : 1;
            space = p[i] == ' ';
            join.reset();
        } else {
            const Decoded d = decode_utf8(p + i, t.size() - i);
            w = join.width(d.cp);
            len = d.len;
        }
        if (w && !space && !fits(w) && col > row_start) {  // (it does not fit: a new row; a space hangs past the end)
            new_row(here);
            if (kNotes && !fits(w) && col > row_start) new_row(here);  // (past the last space, the notes there still in the way)
        }
        col += w;
        if (space) after_space = col, space_extra = extra;
        i += len;
    }
    // Notes at the line's end: one that does not fit takes the word before
    // it to the next row, if there is a space before that (no row holds a
    // note alone).
    const u64 here = extra;
    for (; kNotes && k < notes.size(); ++k) {
        if (!fits(notes[k].width) && after_space > row_start) new_row(here);
        extra += notes[k].width, base -= notes[k].width;
    }
}

}  // namespace

void Wrap::breaks(std::string_view t, u32 width, u32 tab, std::vector<u32>* out, std::span<const Hint> notes) {
    if (notes.empty()) line_breaks<false>(t, width, tab, out, notes);
    else line_breaks<true>(t, width, tab, out, notes);
}

void Wrap::set(Editor* ed, u64 doc, const Folds* folds, u64 folds_gen, u32 width, u32 tab) {
    width = std::max<u32>(width, 8);
    if (ed == ed_ && doc == doc_ && width == width_ && tab == tab_ && folds_gen == folds_gen_ && folds == folds_) return;
    if (ed != ed_ || doc != doc_) {  // (another text: its notes are not these)
        notes_.clear();
        hints_key_ = ~0ull;
    }
    if (ed != ed_) {
        if (ed_) ed_->unlisten(this);
        ed_ = ed;
        if (ed_) ed_->listen(this);
    }
    doc_ = doc, folds_ = folds, folds_gen_ = folds_gen, width_ = width, tab_ = tab;
    if (folds_ && ed_) folds_->sync(ed_->buf, ed_->revision());  // (what is hidden, as the text is now)
    const u64 lines = ed_ ? ed_->buf.line_count() : 0;
    count_.assign(lines, 1);
    starts_.assign(lines, {});
    for (u64 l = 0; l < lines; ++l) measure(l);
    tree();
}

void Wrap::measure(u64 line) {
    if (folds_ && folds_->hidden(line)) {
        count_[line] = 0;
        starts_[line].clear();
        return;
    }
    std::span<const Hint> notes;
    if (!notes_.empty() && line >= notes_.front().line && line <= notes_.back().line) {
        line_hints_.clear();
        for (auto it = std::ranges::lower_bound(notes_, line, {}, &Note::line); it != notes_.end() && it->line == line; ++it) line_hints_.push_back({it->at, it->width});
        notes = line_hints_;
    }
    count_[line] = line_rows(*ed_, line, width_, tab_, &starts_[line], notes);
}

u32 Wrap::line_rows(const Editor& ed, u64 line, u32 width, u32 tab, std::vector<u32>* starts, std::span<const Hint> notes) {
    const Buffer& b = ed.buf;
    const u64 s = b.line_start(line), e = ed.line_end(line);
    if (e - s <= width / 2 && notes.empty()) {  // (short enough whatever it holds: a character is two columns at most, a tab no wider than the width)
        bool tabbed = false;
        if (e - s > 0 && tab > 2) {
            char buf[256];
            const u64 n = std::min<u64>(e - s, sizeof buf);
            b.read(s, buf, n);
            tabbed = std::find(buf, buf + n, '\t') != buf + n;
        }
        if (!tabbed) {
            starts->clear();
            return 1;
        }
    }
    breaks(b.text(s, e - s), width, tab, starts, notes);
    return 1 + static_cast<u32>(starts->size());
}

void Wrap::set_hints(std::vector<Hint> hints, u64 key) {
    hints_key_ = key;
    std::vector<Note> notes;
    if (ed_) {
        const Buffer& b = ed_->buf;
        notes.reserve(hints.size());
        for (const Hint& h : hints) {
            const u64 off = std::min(h.off, b.size()), line = b.line_of(off);
            notes.push_back({line, static_cast<u32>(off - b.line_start(line)), h.width});
        }
    }
    if (notes == notes_) return;
    std::vector<u64> lines;  // (those with notes, before or now: measured again)
    for (const Note& n : notes_) lines.push_back(n.line);
    for (const Note& n : notes) lines.push_back(n.line);
    notes_ = std::move(notes);
    std::ranges::sort(lines);
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    for (const u64 l : lines) {
        if (l >= count_.size()) continue;
        const u32 was = count_[l];
        measure(l);
        if (count_[l] != was) add(l, static_cast<i64>(count_[l]) - static_cast<i64>(was));
    }
}

double Wrap::top_line(double rows) const {
    if (count_.empty()) return 0;
    rows = std::max(0.0, rows);
    const u64 line = line_at(static_cast<u64>(rows));  // (never one folded away)
    const double part = (rows - static_cast<double>(sum_before(line))) / std::max<u32>(1, count_[line]);
    return static_cast<double>(line) + std::clamp(part, 0.0, 0.9999);
}

double Wrap::top_rows(double line) const {
    if (count_.empty()) return 0;
    const double whole = std::floor(std::max(0.0, line));
    const u64 l = std::min<u64>(static_cast<u64>(whole), count_.size() - 1);
    return static_cast<double>(row_of(l)) + (std::max(0.0, line) - whole) * count_[l];  // (one folded away: its header's row)
}

double Wrap::walk_line(const Editor& ed, const Folds* folds, u32 width, u32 tab, double rows) {
    width = std::max<u32>(width, 8);
    rows = std::max(0.0, rows);
    const u64 lines = ed.buf.line_count();
    std::vector<u32> starts;
    double above = 0;  // (rows of the lines before this one)
    for (u64 l = 0; l < lines; ++l) {
        const u32 n = folds && folds->hidden(l) ? 0 : line_rows(ed, l, width, tab, &starts);
        if (n && above + n > rows) return static_cast<double>(l) + (rows - above) / n;
        above += n;
    }
    return lines > 0 ? static_cast<double>(lines - 1) : 0;
}

double Wrap::walk_rows(const Editor& ed, const Folds* folds, u32 width, u32 tab, double line) {
    width = std::max<u32>(width, 8);
    const u64 lines = ed.buf.line_count();
    const double whole = std::floor(std::max(0.0, line));
    const u64 to = std::min<u64>(static_cast<u64>(whole), lines > 0 ? lines - 1 : 0);
    std::vector<u32> starts;
    double above = 0, shown = 0;  // (rows of the lines before it; where the last line on show began)
    for (u64 l = 0; l < to; ++l) {
        if (folds && folds->hidden(l)) continue;
        shown = above;
        above += line_rows(ed, l, width, tab, &starts);
    }
    if (folds && folds->hidden(to)) return shown;  // (folded away: its header's row)
    return above + (std::max(0.0, line) - whole) * line_rows(ed, to, width, tab, &starts);
}

void Wrap::tree() {
    const size_t n = count_.size();
    fen_.assign(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        fen_[i + 1] += count_[i];
        const size_t up = (i + 1) + ((i + 1) & (~(i + 1) + 1));
        if (up <= n) fen_[up] += fen_[i + 1];
    }
}

void Wrap::add(u64 line, i64 by) {
    for (size_t i = line + 1; i < fen_.size(); i += i & (~i + 1)) fen_[i] = static_cast<u64>(static_cast<i64>(fen_[i]) + by);
}

u64 Wrap::sum_before(u64 line) const {
    u64 s = 0;
    for (size_t i = std::min<size_t>(line, count_.size()); i > 0; i -= i & (~i + 1)) s += fen_[i];
    return s;
}

u64 Wrap::rows() const { return sum_before(count_.size()); }

u64 Wrap::row_of(u64 line) const {
    if (line >= count_.size()) return rows();
    while (line > 0 && count_[line] == 0) --line;  // (folded away: its header's)
    return sum_before(line);
}

u64 Wrap::line_at(u64 row) const {
    if (count_.empty()) return 0;
    // The first line whose rows reach past `row`.
    size_t at = 0;
    u64 left = row;
    size_t step = 1;
    while (step * 2 < fen_.size()) step *= 2;
    for (; step; step /= 2)
        if (at + step < fen_.size() && fen_[at + step] <= left) {
            at += step;
            left -= fen_[at];
        }
    return std::min<u64>(at, count_.size() - 1);
}

void Wrap::walk(u64 top, u32 n, u64* lines, u32* cols, u32* ends) const {
    const u64 total = rows();
    u64 line = top < total ? line_at(top) : count_.size();
    u64 k = line < count_.size() ? top - sum_before(line) : 0;  // (which of its line's rows)
    for (u32 r = 0; r < n; ++r) {
        while (line < count_.size() && k >= count_[line]) ++line, k = 0;  // (on to the next line with rows: past folded ones)
        if (line >= count_.size()) {
            lines[r] = ~0ull, cols[r] = 0, ends[r] = ~0u;
            continue;
        }
        const std::vector<u32>& st = starts_[line];
        lines[r] = line;
        cols[r] = k == 0 ? 0 : st[k - 1];
        ends[r] = k < st.size() ? st[k] : ~0u;
        ++k;
    }
}

u32 Wrap::start_col(u64 row) const {
    const u64 line = line_at(row);
    const u64 k = row - std::min(row, sum_before(line));
    return k == 0 || k > starts_[line].size() ? 0 : starts_[line][k - 1];
}

u32 Wrap::end_col(u64 row) const {
    const u64 line = line_at(row);
    const u64 k = row - std::min(row, sum_before(line));
    return k < starts_[line].size() ? starts_[line][k] : ~0u;
}

u64 Wrap::row_at(u64 line, u32 col) const {
    if (line >= count_.size()) return rows();
    const u64 first = row_of(line);
    if (count_[line] == 0) return first;
    const std::vector<u32>& st = starts_[line];
    return first + static_cast<u64>(std::upper_bound(st.begin(), st.end(), col) - st.begin());
}

void Wrap::on_edit(const TextEdit& e) {
    if (!ed_ || count_.empty()) return;
    // (What is hidden now: lines the edit made inside a fold are. The folds
    // have heard of the edit already, being the editor's first listener.)
    if (folds_) folds_->sync(ed_->buf, ed_->revision());
    if (!notes_.empty()) {
        // The notes after it move with the text (one inside what was
        // replaced goes with it): those on the line it ended in, along it to
        // where it ends now; the lines after, by the lines it made or took.
        const Note from{e.start_row, e.start_col}, to = std::max(Note{e.old_end_row, e.old_end_col}, Note{e.start_row, e.start_col + 1});
        auto it = notes_.erase(std::upper_bound(notes_.begin(), notes_.end(), from), std::lower_bound(notes_.begin(), notes_.end(), to));
        for (; it != notes_.end() && it->line == e.old_end_row; ++it) it->line = e.new_end_row, it->at = it->at - e.old_end_col + e.new_end_col;
        if (e.new_end_row != e.old_end_row)
            for (; it != notes_.end(); ++it) it->line = it->line - e.old_end_row + e.new_end_row;
    }
    const u64 first = e.start_row, old_last = e.old_end_row, new_last = e.new_end_row;
    // The lines it replaced make way for the ones it made; those are measured.
    if (new_last != old_last) {
        if (new_last > old_last) {
            count_.insert(count_.begin() + static_cast<std::ptrdiff_t>(old_last + 1), new_last - old_last, 1);
            starts_.insert(starts_.begin() + static_cast<std::ptrdiff_t>(old_last + 1), new_last - old_last, {});
        } else {
            count_.erase(count_.begin() + static_cast<std::ptrdiff_t>(new_last + 1), count_.begin() + static_cast<std::ptrdiff_t>(old_last + 1));
            starts_.erase(starts_.begin() + static_cast<std::ptrdiff_t>(new_last + 1), starts_.begin() + static_cast<std::ptrdiff_t>(old_last + 1));
        }
        for (u64 l = first; l <= new_last && l < count_.size(); ++l) measure(l);
        tree();
        return;
    }
    for (u64 l = first; l <= new_last && l < count_.size(); ++l) {
        const u32 was = count_[l];
        measure(l);
        if (count_[l] != was) add(l, static_cast<i64>(count_[l]) - static_cast<i64>(was));
    }
}

}  // namespace blit
