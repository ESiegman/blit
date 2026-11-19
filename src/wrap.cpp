#include "wrap.hpp"

#include <algorithm>

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
}

void Wrap::breaks(std::string_view t, u32 width, u32 tab, std::vector<u32>* out) {
    out->clear();
    if (width < 2) return;
    u32 col = 0, row_start = 0, after_space = 0;  // (after_space: where a row may start, past spaces; 0: nowhere yet)
    Joiner join;
    const u8* p = reinterpret_cast<const u8*>(t.data());
    for (size_t i = 0; i < t.size();) {
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
        if (w && !space && col + w - row_start > width && col > row_start) {  // (it does not fit: a new row; a space hangs past the end)
            row_start = after_space > row_start ? after_space : col;
            out->push_back(row_start);
        }
        col += w;
        if (space) after_space = col;
        i += len;
    }
}

void Wrap::set(Editor* ed, u64 doc, const Folds* folds, u64 folds_gen, u32 width, u32 tab) {
    width = std::max<u32>(width, 8);
    if (ed == ed_ && doc == doc_ && width == width_ && tab == tab_ && folds_gen == folds_gen_ && folds == folds_) return;
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
    const Buffer& b = ed_->buf;
    if (folds_ && folds_->hidden(line)) {
        count_[line] = 0;
        starts_[line].clear();
        return;
    }
    const u64 s = b.line_start(line), e = ed_->line_end(line);
    if (e - s <= width_ / 2) {  // (short enough whatever it holds: a character is two columns at most, a tab no wider than the width)
        bool tabbed = false;
        if (e - s > 0 && tab_ > 2) {
            char buf[256];
            const u64 n = std::min<u64>(e - s, sizeof buf);
            b.read(s, buf, n);
            tabbed = std::find(buf, buf + n, '\t') != buf + n;
        }
        if (!tabbed) {
            count_[line] = 1;
            starts_[line].clear();
            return;
        }
    }
    breaks(b.text(s, e - s), width_, tab_, &starts_[line]);
    count_[line] = 1 + static_cast<u32>(starts_[line].size());
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
