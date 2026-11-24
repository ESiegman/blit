#include "streamtab.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <utility>

#include "term.hpp"

namespace blit {

namespace {

constexpr size_t kKeepMost = 8u << 20, kKeepTrimmed = 6u << 20;  // what is kept of what came, at most; and after the oldest goes
constexpr size_t kChunksMost = 1u << 18;                         // the times kept, at most
constexpr u64 kViewLines = 10000;                                // (the Term's history: what drawing again starts from)

// "14:02:03.120", by the clock here; with the date first, "2026-11-23 14:02:03.120".
std::string clock_text(u64 ns, bool date) {
    const time_t s = static_cast<time_t>(ns / 1'000'000'000ull);
    tm t{};
    ::localtime_r(&s, &t);
    char buf[48];
    const unsigned ms = static_cast<unsigned>(ns / 1'000'000ull % 1000);
    if (date)
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03u", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ms);
    else
        std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03u", t.tm_hour, t.tm_min, t.tm_sec, ms);
    return buf;
}

std::string span_text(u64 ns) {
    char buf[48];
    const double s = static_cast<double>(ns) * 1e-9;
    if (s < 60) std::snprintf(buf, sizeof buf, "%.1f s", s);
    else std::snprintf(buf, sizeof buf, "%u min %u s", static_cast<unsigned>(s / 60), static_cast<unsigned>(s) % 60);
    return buf;
}

}  // namespace

const char* serial_return_name(SerialReturn r) { return r == SerialReturn::kLF ? "LF" : r == SerialReturn::kCRLF ? "CR LF" : "CR"; }

bool serial_return_parse(std::string_view s, SerialReturn* r) {
    std::string w;
    for (const char c : s)
        if (c != ' ' && c != '-' && c != '+' && c != '/') w += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (w == "cr") *r = SerialReturn::kCR;
    else if (w == "lf") *r = SerialReturn::kLF;
    else if (w == "crlf") *r = SerialReturn::kCRLF;
    else return false;
    return true;
}

// ---------------------------------------------------------------- the tab

StreamTab::StreamTab(SerialReturn ret, std::mutex& mu, Term& term) : mu_(mu), term_(term), ret_(ret) {
    int p[2];
    if (::pipe(p) == 0) {
        wake_r_ = p[0], wake_w_ = p[1];
        for (const int fd : p) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
}

StreamTab::~StreamTab() {
    if (log_) std::fclose(log_);
    if (wake_r_ >= 0) ::close(wake_r_);
    if (wake_w_ >= 0) ::close(wake_w_);
}

u64 StreamTab::wall_ns() {
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
}

void StreamTab::poke() {
    if (wake_w_ >= 0) {
        const char c = 1;
        [[maybe_unused]] const ssize_t k = ::write(wake_w_, &c, 1);
    }
}

void StreamTab::drain() {
    char buf[64];
    while (wake_r_ >= 0 && ::read(wake_r_, buf, sizeof buf) > 0) {
    }
}

// ---------------------------------------------------------------- asked by the main thread

SerialReturn StreamTab::return_mode() {
    std::lock_guard lock(mu_);
    return ret_;
}

void StreamTab::set_return(SerialReturn r) {
    std::lock_guard lock(mu_);
    ret_ = r;
}

std::string StreamTab::return_bytes() {
    std::lock_guard lock(mu_);
    return ret_ == SerialReturn::kLF ? "\n" : ret_ == SerialReturn::kCRLF ? "\r\n" : "\r";
}

bool StreamTab::stamps() {
    std::lock_guard lock(mu_);
    return stamps_;
}

void StreamTab::set_stamps(bool on) {
    std::lock_guard lock(mu_);
    if (stamps_ == on) return;
    stamps_ = on;
    rebuild();
}

bool StreamTab::hex() {
    std::lock_guard lock(mu_);
    return hex_;
}

void StreamTab::set_hex(bool on) {
    std::lock_guard lock(mu_);
    if (hex_ == on) return;
    hex_ = on;
    rebuild();
}

bool StreamTab::log_to(const std::string& path, std::string* why) {
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) {
        *why = "Cannot write " + path + ": " + std::strerror(errno);
        return false;
    }
    std::lock_guard lock(mu_);
    if (log_) std::fclose(log_);
    log_ = f;
    log_path_ = path;
    log_line_start_ = true;
    return true;
}

void StreamTab::log_stop() {
    std::lock_guard lock(mu_);
    if (!log_) return;
    if (!log_line_start_) std::fputc('\n', log_);
    std::fclose(log_);
    log_ = nullptr;
    log_path_.clear();
}

std::string StreamTab::log_path() {
    std::lock_guard lock(mu_);
    return log_path_;
}

void StreamTab::clear() {
    std::lock_guard lock(mu_);
    forget();
}

void StreamTab::forget() {
    base_ += bytes_.size();
    bytes_.clear();
    chunks_.clear();
    if (gap_open()) {
        Event e = events_.back();
        e.at = base_;
        events_.assign(1, e);
    } else {
        events_.clear();
    }
    rebuild();
}

std::vector<std::pair<std::string, int>> StreamTab::take_news() {
    std::lock_guard lock(mu_);
    return std::exchange(news_, {});
}

void StreamTab::set_scan(std::unique_ptr<StreamScan> scan) {
    std::lock_guard lock(scan_mu_);
    scan_ = std::move(scan);
    scan_line_.clear();
}

// ---------------------------------------------------------------- the reader's

std::string StreamTab::take(const char* p, size_t n, u64 ns) {
    // The lines looked at first, with the tab's mutex free (the drawing is
    // not held up while a crash dump's ELF is read); what they added goes
    // in after each, as the bytes are drawn and logged.
    notes_.clear();
    {
        std::lock_guard lock(scan_mu_);
        if (scan_) scan_lines(p, n, ns);
    }
    std::lock_guard lock(mu_);
    keep(p, n, ns);
    size_t at = 0;
    for (auto& [end, text] : notes_) {
        log_bytes(p + at, end - at, ns);
        render(p + at, end - at, ns);
        at = end;
        Event e;
        e.at = render_at_;
        e.ns = ns;
        e.kind = kNote;
        e.text = std::move(text);
        end_line();
        dim_line(e.text, false);
        log_line(e.text, ns);
        events_.push_back(std::move(e));
    }
    log_bytes(p + at, n - at, ns);
    render(p + at, n - at, ns);
    return term_.take_reply();
}

void StreamTab::take_quiet(const char* p, size_t n, u64 ns) {
    keep(p, n, ns);
    render(p, n, ns);
    (void)term_.take_reply();
}

void StreamTab::scan_gone() {
    std::lock_guard lock(scan_mu_);
    scan_line_.clear();
    if (scan_) scan_->gone();
}

void StreamTab::gap_begin(const std::string& why, u64 ns, const std::string& waiting) {
    Event e;
    e.at = base_ + bytes_.size();
    e.ns = ns;
    e.kind = kGap;
    e.text = why;
    events_.push_back(e);
    status(gap_text(e, waiting));
    log_line(gap_text(e), ns);
}

bool StreamTab::gap_end(u64 ns) {
    if (!events_.empty() && events_.back().kind == kGap && !events_.back().back) {
        Event& e = events_.back();
        e.back = ns;
        status_end(gap_text(e));
        log_line(gap_text(e), e.back);
        return true;
    }
    status_end({});
    return false;
}

size_t StreamTab::gap_count() const { return static_cast<size_t>(std::ranges::count(events_, kGap, &Event::kind)); }

std::string StreamTab::last_gap_text(const std::string& waiting) const {
    if (events_.empty() || events_.back().kind != kGap) return {};
    return gap_text(events_.back(), waiting);
}

// ---------------------------------------------------------------- what came, kept

void StreamTab::keep(const char* p, size_t n, u64 ns) {
    const u64 at = base_ + bytes_.size();
    if (chunks_.empty() || ns / 1'000'000ull != chunks_.back().ns / 1'000'000ull) chunks_.push_back({at, ns});  // (a time to the millisecond, as it is shown)
    bytes_.append(p, n);
    if (bytes_.size() > kKeepMost || chunks_.size() > kChunksMost) trim();
}

// The oldest goes (at a line's start, where one is near), down to what is
// kept after a trim, or to half the times kept.
void StreamTab::trim() {
    size_t drop = bytes_.size() > kKeepTrimmed ? bytes_.size() - kKeepTrimmed : 0;
    if (chunks_.size() > kChunksMost / 2) drop = std::max<size_t>(drop, static_cast<size_t>(chunks_[chunks_.size() / 2].at - base_));
    if (const void* nl = std::memchr(bytes_.data() + drop, '\n', std::min<size_t>(4096, bytes_.size() - drop))) drop = static_cast<size_t>(static_cast<const char*>(nl) - bytes_.data()) + 1;
    bytes_.erase(0, drop);
    base_ += drop;
    while (chunks_.size() > 1 && chunks_[1].at <= base_) chunks_.pop_front();
    if (!chunks_.empty() && chunks_.front().at < base_) chunks_.front().at = base_;
    while (!events_.empty() && events_.front().at < base_) events_.pop_front();
}

// ---------------------------------------------------------------- drawn

void StreamTab::render(const char* p, size_t n, u64 ns) {
    if (hex_) render_hex(p, n, ns);
    else render_text(p, n, ns);
    render_at_ += n;
}

// Each line from the device a line: LF alone is taken as CR LF (as a
// terminal's ONLCR would), CR alone goes back along the line (a progress
// count written over itself).
void StreamTab::render_text(const char* p, size_t n, u64 ns) {
    std::string& out = scratch_;
    out.clear();
    size_t i = 0;
    while (i < n) {
        if (line_start_ && stamps_) {
            if (!out.empty()) term_.feed(out), out.clear();
            stamp(ns);
        }
        line_start_ = false;
        const void* nl = std::memchr(p + i, '\n', n - i);
        const size_t end = nl ? static_cast<size_t>(static_cast<const char*>(nl) - p) : n;
        if (end > i) {
            out.append(p + i, end - i);
            after_cr_ = p[end - 1] == '\r';
        }
        if (!nl) break;
        if (!after_cr_) out += '\r';
        out += '\n';
        after_cr_ = false;
        line_start_ = true;
        i = end + 1;
    }
    if (!out.empty()) term_.feed(out);
}

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

// A row of n bytes in hex takes n * 3 columns, and one more for the space
// after the eighth of sixteen: k of them written, the rest padded, then a
// space before the text.
void hex_pad(std::string& out, size_t k, size_t n) {
    const size_t used = k * 3 + (n == 16 && k >= 8 ? 1 : 0), all = n * 3 + (n == 16 ? 1 : 0);
    out.append(all - used + 1, ' ');
}

// The row's bytes as text: printable ASCII as it is, the rest dots.
void hex_text(std::string& out, const std::string& row) {
    for (const char c : row) out += c >= 0x20 && c < 0x7f ? c : '.';
    out += "\r\n";
}

}  // namespace

// Rows of 16 bytes (8 in a tab too narrow for 16; a row ends early at a
// line's LF): the place, the bytes in hex, then as text, once the row is
// whole.
void StreamTab::render_hex(const char* p, size_t n, u64 ns) {
    std::string& out = scratch_;
    out.clear();
    for (size_t i = 0; i < n; ++i) {
        const u8 b = static_cast<u8>(p[i]);
        if (!hex_open_) {
            if (stamps_) {
                if (!out.empty()) term_.feed(out), out.clear();
                stamp(ns);
            }
            char at[24];
            std::snprintf(at, sizeof at, "%08llx  ", static_cast<unsigned long long>(render_at_ + i));
            out += at;
            hex_open_ = true;
            hex_row_.clear();
            const u32 wide = 10 + 16 * 3 + 2 + 16 + (stamps_ ? 13 : 0);  // (the place, the bytes, the text, the time)
            hex_n_ = term_.cols() >= wide ? 16 : 8;
        }
        out += kHexDigits[b >> 4];
        out += kHexDigits[b & 15];
        out += ' ';
        hex_row_ += static_cast<char>(b);
        if (hex_n_ == 16 && hex_row_.size() == 8) out += ' ';
        if (hex_row_.size() == hex_n_ || b == '\n') {
            hex_pad(out, hex_row_.size(), hex_n_);
            hex_text(out, hex_row_);
            hex_open_ = false;
        }
    }
    if (!out.empty()) term_.feed(out);
}

void StreamTab::hex_end_row() {
    if (!hex_open_) return;
    std::string out;
    hex_pad(out, hex_row_.size(), hex_n_);
    hex_text(out, hex_row_);
    term_.feed(out);
    hex_open_ = false;
}

void StreamTab::end_line() {
    if (hex_open_) {
        hex_end_row();
    } else if (!line_start_) {
        term_.feed("\r\n");
    }
    line_start_ = true;
    after_cr_ = false;
}

// The host's time before a line, dim, the device's own pen put back after it.
void StreamTab::stamp(u64 ns) {
    const TermCell pen = term_.pen();
    TermCell dim;
    dim.attr = TermCell::kDim;
    term_.set_pen(dim);
    term_.feed(clock_text(ns, false) + " ");
    term_.set_pen(pen);
}

void StreamTab::dim_line(const std::string& text, bool open) {
    const TermCell pen = term_.pen();
    TermCell dim;
    dim.attr = TermCell::kDim;
    term_.set_pen(dim);
    term_.feed(text);
    term_.set_pen(pen);
    if (!open) term_.feed("\r\n");
}

void StreamTab::status(const std::string& text) {
    const u64 here = term_.lines_gone() + term_.history() + term_.cursor_row();
    if (status_row_ != ~0ull && here >= status_row_ && here - status_row_ <= 16) {
        if (here > status_row_) term_.feed("\x1b[" + std::to_string(here - status_row_) + "A");
        term_.feed("\r\x1b[J");
    } else {
        end_line();
        status_row_ = term_.lines_gone() + term_.history() + term_.cursor_row();
    }
    dim_line(text, true);
    line_start_ = false;
}

void StreamTab::status_end(const std::string& last) {
    if (status_row_ == ~0ull) return;
    const u64 here = term_.lines_gone() + term_.history() + term_.cursor_row();
    if (here >= status_row_ && here - status_row_ <= 16) {
        if (here > status_row_) term_.feed("\x1b[" + std::to_string(here - status_row_) + "A");
        term_.feed("\r\x1b[J");
    } else {
        term_.feed("\r\n");
    }
    status_row_ = ~0ull;
    if (!last.empty()) dim_line(last, false);
    line_start_ = true;
    after_cr_ = false;
}

std::string StreamTab::gap_text(const Event& e, const std::string& waiting) const {
    const bool rel = e.text == kReleasedWhy;
    if (!e.back) {
        std::string t = rel ? std::string("[released for flashing at ") + clock_text(e.ns, false) : "[gone at " + clock_text(e.ns, false) + " (" + e.text + ")";
        if (!waiting.empty()) t += "; " + waiting;
        return t + "]";
    }
    return (rel ? std::string("[released for flashing from ") : std::string("[gone from ")) + clock_text(e.ns, false) + " to " + clock_text(e.back, false) + ", " +
           span_text(e.back > e.ns ? e.back - e.ns : 0) + (rel ? std::string("]") : " (" + e.text + ")]");
}

// Drawn again from what is kept: the Term emptied, then the last of what
// came (as much as its history holds), with its times, gaps and notes.
void StreamTab::rebuild() {
    term_.feed("\x1b" "c");
    term_.clear_history();
    line_start_ = true, after_cr_ = false, hex_open_ = false;
    status_row_ = ~0ull;
    const u64 end = base_ + bytes_.size();
    const u64 want = kViewLines + term_.rows();
    const size_t lowest = bytes_.size() > (4u << 20) ? bytes_.size() - (4u << 20) : 0;
    size_t i = bytes_.size();
    if (hex_) {
        i = std::max<size_t>(lowest, bytes_.size() > want * 16 ? bytes_.size() - static_cast<size_t>(want * 16) : 0);
    } else {
        u64 lines = 0;
        while (i > lowest && !(bytes_[i - 1] == '\n' && ++lines > want)) --i;
    }
    u64 pos = base_ + i;
    render_at_ = pos;
    u64 ns = 0;
    size_t ci = 0;
    while (ci < chunks_.size() && chunks_[ci].at <= pos) ns = chunks_[ci++].ns;
    size_t ei = 0;
    while (ei < events_.size() && events_[ei].at < pos) ++ei;
    for (;;) {
        u64 next = end;
        if (ci < chunks_.size()) next = std::min(next, chunks_[ci].at);
        if (ei < events_.size()) next = std::min(next, events_[ei].at);
        if (next > pos) {
            render(bytes_.data() + (pos - base_), static_cast<size_t>(next - pos), ns);
            pos = next;
        }
        for (; ei < events_.size() && events_[ei].at <= pos; ++ei) {
            const Event& e = events_[ei];
            if (e.kind == kNote) {
                end_line();
                dim_line(e.text, false);
            } else if (e.back) {
                end_line();
                dim_line(gap_text(e), false);
            } else {
                status(gap_text(e));
            }
        }
        for (; ci < chunks_.size() && chunks_[ci].at <= pos; ++ci) ns = chunks_[ci].ns;
        if (pos >= end && ei >= events_.size()) break;
    }
    (void)term_.take_reply();  // (what the device asked was answered when it asked)
}

// ---------------------------------------------------------------- the log

void StreamTab::log_bytes(const char* p, size_t n, u64 ns) {
    if (!log_) return;
    size_t i = 0;
    while (i < n) {
        if (log_line_start_) {
            const std::string t = clock_text(ns, true) + " ";
            std::fwrite(t.data(), 1, t.size(), log_);
            log_line_start_ = false;
        }
        const void* nl = std::memchr(p + i, '\n', n - i);
        const size_t end = nl ? static_cast<size_t>(static_cast<const char*>(nl) - p) + 1 : n;
        std::fwrite(p + i, 1, end - i, log_);
        if (nl) log_line_start_ = true;
        i = end;
    }
    std::fflush(log_);
}

void StreamTab::log_line(const std::string& text, u64 ns) {
    if (!log_) return;
    if (!log_line_start_) std::fputc('\n', log_);
    std::fprintf(log_, "%s %s\n", clock_text(ns, true).c_str(), text.c_str());
    log_line_start_ = true;
    std::fflush(log_);
}

// ---------------------------------------------------------------- a recogniser of lines

// Each line that ends in what came told to the recogniser (one begun in
// what came before put together with it); what it adds kept in notes_,
// after the line's LF.
void StreamTab::scan_lines(const char* p, size_t n, u64 ns) {
    size_t i = 0;
    while (i < n) {
        const void* nl = std::memchr(p + i, '\n', n - i);
        if (!nl) {
            scan_line_.append(p + i, n - i);
            break;
        }
        const size_t end = static_cast<size_t>(static_cast<const char*>(nl) - p) + 1;
        std::string_view line(p + i, end - i);  // (whole in what came, as most are: not copied)
        if (!scan_line_.empty()) line = scan_line_.append(line);
        i = end;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        scan_add_.clear();
        scan_->line(line, ns, &scan_add_);
        scan_line_.clear();
        for (std::string& a : scan_add_) notes_.emplace_back(end, std::move(a));
    }
    if (scan_line_.size() > 64 * 1024) scan_line_.erase(0, scan_line_.size() - 64 * 1024);  // (a "line" that never ends is not kept whole)
}

}  // namespace blit
