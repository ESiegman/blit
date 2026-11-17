#include "folds.hpp"

#include <algorithm>

namespace blit {

bool Folds::add(const Buffer& b, u64 header, u64 last) {
    const u64 lines = b.line_count();
    if (header >= lines || last <= header) return false;
    last = std::min(last, lines - 1);
    const u64 end = last + 1 < lines ? b.line_start(last + 1) - 1 : b.size();
    for (const Fold& f : folds_)
        if (f.head == b.line_start(header) && f.end == end) return false;
    folds_.push_back({b.line_start(header), end});
    ++gen_;
    return true;
}

bool Folds::open_header(const Buffer& b, u64 line) {
    const size_t before = folds_.size();
    std::erase_if(folds_, [&](const Fold& f) { return b.line_of(f.head) == line; });
    if (folds_.size() == before) return false;
    ++gen_;
    return true;
}

bool Folds::open_around(const Buffer& b, u64 line) {
    const size_t before = folds_.size();
    std::erase_if(folds_, [&](const Fold& f) { return line > b.line_of(f.head) && line <= b.line_of(f.end); });
    if (folds_.size() == before) return false;
    ++gen_;
    return true;
}

void Folds::clear() {
    if (folds_.empty()) return;
    folds_.clear();
    ++gen_;
}

void Folds::on_edit(const TextEdit& e) {
    const u64 d = e.new_end - e.old_end;  // (as two's complement: added, it moves either way)
    const size_t before = folds_.size();
    std::erase_if(folds_, [&](Fold& f) {
        if (e.start > f.end) return false;  // after it
        if ((e.old_end <= f.head && e.start < f.head) || (e.start == e.old_end && e.start == f.head)) {  // before it, or typed at its start
            f.head += d, f.end += d;
            return false;
        }
        if (e.start >= f.head && e.old_end <= f.end) {  // in it
            f.end += d;
            return f.end <= f.head;
        }
        return true;  // across an edge: it opens
    });
    if (folds_.size() != before) ++gen_;
    synced_rev_ = ~0ull;
}

void Folds::sync(const Buffer& b, u64 revision) const {
    if (revision == synced_rev_ && gen_ == synced_gen_) return;
    synced_rev_ = revision;
    synced_gen_ = gen_;
    runs_.clear();
    for (const Fold& f : folds_) {
        const u64 h = b.line_of(std::min(f.head, b.size())), l = b.line_of(std::min(f.end, b.size()));
        if (l > h) runs_.emplace_back(h + 1, l);
    }
    std::ranges::sort(runs_);
    size_t n = 0;
    for (const auto& r : runs_) {
        if (n > 0 && r.first <= runs_[n - 1].second + 1) runs_[n - 1].second = std::max(runs_[n - 1].second, r.second);
        else runs_[n++] = r;
    }
    runs_.resize(n);
    before_.resize(n);
    hidden_ = 0;
    for (size_t i = 0; i < n; ++i) {
        before_[i] = hidden_;
        hidden_ += runs_[i].second - runs_[i].first + 1;
    }
}

bool Folds::hidden(u64 line) const {
    const auto it = std::ranges::upper_bound(runs_, line, {}, &std::pair<u64, u64>::first);
    return it != runs_.begin() && line <= std::prev(it)->second;
}

bool Folds::header(u64 line) const {
    const auto it = std::ranges::lower_bound(runs_, line + 1, {}, &std::pair<u64, u64>::first);
    return it != runs_.end() && it->first == line + 1;
}

u64 Folds::hidden_under(u64 line) const {
    const auto it = std::ranges::lower_bound(runs_, line + 1, {}, &std::pair<u64, u64>::first);
    return it != runs_.end() && it->first == line + 1 ? it->second - it->first + 1 : 0;
}

u64 Folds::row_of(u64 line) const {
    const size_t k = static_cast<size_t>(std::ranges::upper_bound(runs_, line, {}, &std::pair<u64, u64>::first) - runs_.begin());
    if (k == 0) return line;
    const auto& r = runs_[k - 1];
    if (line <= r.second) return r.first - 1 - before_[k - 1];
    return line - before_[k - 1] - (r.second - r.first + 1);
}

u64 Folds::line_at(u64 row) const {
    // Run i's header is on row first - 1 - before; rows past it are later lines.
    size_t lo = 0, hi = runs_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (runs_[mid].first - 1 - before_[mid] < row) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return row;
    return row + before_[lo - 1] + (runs_[lo - 1].second - runs_[lo - 1].first + 1);
}

u64 Folds::visible(u64 line, int dir, u64 lines) const {
    const auto it = std::ranges::upper_bound(runs_, line, {}, &std::pair<u64, u64>::first);
    if (it == runs_.begin() || line > std::prev(it)->second) return line;
    const auto& r = *std::prev(it);
    if (dir < 0 || r.second + 1 >= lines) return r.first - 1;
    return r.second + 1;
}

u64 Folds::skip_row(u64 line) const {
    sync(ed_->buf, ed_->revision());
    return row_of(line);
}

u64 Folds::skip_line(u64 row) const {
    sync(ed_->buf, ed_->revision());
    return line_at(row);
}

u64 Folds::skip_rows() const {
    sync(ed_->buf, ed_->revision());
    return rows(ed_->buf.line_count());
}

}  // namespace blit
