#include "serialtab.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <utility>

#include "term.hpp"
#include "usb.hpp"

namespace blit {

namespace {

constexpr size_t kKeepMost = 8u << 20, kKeepTrimmed = 6u << 20;  // what is kept of what came, at most; and after the oldest goes
constexpr size_t kChunksMost = 1u << 18;                         // the times kept, at most
constexpr u64 kViewLines = 10000;                                // (the Term's history: what drawing again starts from)

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// What a port is called: its node's name ("cu.usbmodem1101", "ttyACM0"),
// a /dev/serial/by-id (or by-path) link by the device it leads to.
std::string port_name(const std::string& path) {
    if (path.find("/serial/by-") != std::string::npos) return base_name(serial_device(path));
    return base_name(path);
}

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

// ---------------------------------------------------------------- the port, found

SerialPortId serial_identify(const std::string& path) {
    SerialPortId id;
    id.path = path;
    const std::string real = serial_device(path);
    for (const UsbDevice& d : usb_list())
        for (const UsbPort& p : d.ports)
            if (serial_device(p.path) == real) {
                id.serial = d.serial;
                id.vid = d.vid, id.pid = d.pid;
                id.iface = p.iface;
                id.name = !d.product.empty() ? d.product : d.manufacturer;
                return id;
            }
    return id;
}

std::string serial_locate(const SerialPortId& id, std::string* why) {
    if (id.serial.empty()) {
        struct stat st;
        if (::stat(id.path.c_str(), &st) == 0) return id.path;
        why->clear();
        return {};
    }
    for (const UsbDevice& d : usb_list()) {
        if (d.serial != id.serial || (id.vid && (d.vid != id.vid || d.pid != id.pid))) continue;
        const UsbPort* port = nullptr;
        for (const UsbPort& p : d.ports)
            if (!port || (p.iface == id.iface && port->iface != id.iface)) port = &p;
        if (!port) continue;
        // (The path it was asked for, while that is the port still: a by-id
        // link, a /dev/cu name; else the device's own.)
        return serial_device(id.path) == serial_device(port->path) ? id.path : port->path;
    }
    why->clear();
    return {};
}

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

SerialTab::SerialTab(SerialPortId id, SerialConfig cfg, SerialReturn ret, std::mutex& mu, Term& term)
    : mu_(mu), term_(term), id_(std::move(id)), cfg_(cfg), ret_(ret) {
    int p[2];
    if (::pipe(p) == 0) {
        wake_r_ = p[0], wake_w_ = p[1];
        for (const int fd : p) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    shown_ = port_name(id_.path.empty() ? id_.serial : id_.path);
}

SerialTab::~SerialTab() {
    if (log_) std::fclose(log_);
    if (wake_r_ >= 0) ::close(wake_r_);
    if (wake_w_ >= 0) ::close(wake_w_);
}

u64 SerialTab::wall_ns() {
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
}

void SerialTab::poke() {
    if (wake_w_ >= 0) {
        const char c = 1;
        [[maybe_unused]] const ssize_t k = ::write(wake_w_, &c, 1);
    }
}

void SerialTab::drain() {
    char buf[64];
    while (wake_r_ >= 0 && ::read(wake_r_, buf, sizeof buf) > 0) {
    }
}

// ---------------------------------------------------------------- asked by the main thread

std::string SerialTab::title() {
    std::lock_guard lock(mu_);
    std::string t = shown_ + " " + std::to_string(cfg_.baud);
    if (const std::string f = serial_framing(cfg_); f != "8N1") t += " " + f;
    if (state_ == kOpening) t += " (opening)";
    else if (state_ == kGone) t += " (gone)";
    else if (state_ == kReleased) t += " (released)";
    return t;
}

SerialPortId SerialTab::id() {
    std::lock_guard lock(mu_);
    return id_;
}

std::string SerialTab::name() {
    std::lock_guard lock(mu_);
    return shown_;
}

std::string SerialTab::device() {
    std::lock_guard lock(mu_);
    return device_;
}

SerialConfig SerialTab::config() {
    std::lock_guard lock(mu_);
    return cfg_;
}

void SerialTab::set_config(const SerialConfig& c) {
    std::lock_guard lock(mu_);
    if (c == cfg_) return;
    cfg_ = c;
    if (state_ == kOpen) ops_.push_front({now_ns(), kConfig, -1, -1, shown_ + " at " + serial_text(c)});
    poke();
}

SerialReturn SerialTab::return_mode() {
    std::lock_guard lock(mu_);
    return ret_;
}

void SerialTab::set_return(SerialReturn r) {
    std::lock_guard lock(mu_);
    ret_ = r;
}

std::string SerialTab::return_bytes() {
    std::lock_guard lock(mu_);
    return ret_ == SerialReturn::kLF ? "\n" : ret_ == SerialReturn::kCRLF ? "\r\n" : "\r";
}

bool SerialTab::stamps() {
    std::lock_guard lock(mu_);
    return stamps_;
}

void SerialTab::set_stamps(bool on) {
    std::lock_guard lock(mu_);
    if (stamps_ == on) return;
    stamps_ = on;
    rebuild();
}

bool SerialTab::hex() {
    std::lock_guard lock(mu_);
    return hex_;
}

void SerialTab::set_hex(bool on) {
    std::lock_guard lock(mu_);
    if (hex_ == on) return;
    hex_ = on;
    rebuild();
}

void SerialTab::set_lines(int dtr, int rts) {
    std::lock_guard lock(mu_);
    if (dtr >= 0) dtr_ = dtr;
    if (rts >= 0) rts_ = rts;
    lines_set_ = true;
    if (state_ != kOpen) {
        news_.push_back({shown_ + " is not open: DTR and RTS are set when it is", 0});
        return;
    }
    std::string say;
    if (dtr >= 0) say += std::string("DTR ") + (dtr ? "on" : "off");
    if (rts >= 0) say += std::string(say.empty() ? "" : ", ") + "RTS " + (rts ? "on" : "off");
    const Op op{now_ns(), kLines, dtr, rts, say + " on " + shown_};
    ops_.insert(std::ranges::upper_bound(ops_, op.due, {}, &Op::due), op);
    poke();
}

void SerialTab::lines(bool* dtr, bool* rts) {
    std::lock_guard lock(mu_);
    *dtr = dtr_, *rts = rts_;
}

void SerialTab::reset_esp() {
    std::lock_guard lock(mu_);
    if (state_ != kOpen) {
        news_.push_back({shown_ + " is not open", 1});
        return;
    }
    // Both off first (a bridge's two transistors: EN and IO0 high, and the
    // USB-Serial/JTAG resets on RTS rising while DTR is off), then EN low
    // (RTS on) with IO0 high (DTR off): the chip held in reset, then let go
    // of to start its program (esptool's hard reset: 0.1 s through a
    // bridge, 0.2 s on the USB-Serial/JTAG).
    const u64 now = now_ns();
    ops_.push_back({now, kLines, 0, 0, {}});
    ops_.push_back({now + 50'000'000ull, kLines, -1, 1, {}});
    ops_.push_back({now + 250'000'000ull, kLines, -1, 0, "Reset through RTS (EN) on " + shown_});
    dtr_ = 0, rts_ = 0;
    lines_set_ = true;
    poke();
}

void SerialTab::reset_arduino() {
    std::lock_guard lock(mu_);
    if (state_ != kOpen) {
        news_.push_back({shown_ + " is not open", 1});
        return;
    }
    // DTR (and RTS) low, then high again: the falling edge through the
    // board's capacitor pulls RESET low (avrdude's arduino programmer: 250 ms low).
    const u64 now = now_ns();
    ops_.push_back({now, kLines, 0, 0, {}});
    ops_.push_back({now + 250'000'000ull, kLines, 1, 1, "Reset through DTR on " + shown_});
    dtr_ = 1, rts_ = 1;
    lines_set_ = false;  // (as opening leaves them)
    poke();
}

void SerialTab::send_break() {
    std::lock_guard lock(mu_);
    if (state_ != kOpen) {
        news_.push_back({shown_ + " is not open", 1});
        return;
    }
    ops_.push_front({now_ns(), kBreak, -1, -1, "Break sent on " + shown_});
    poke();
}

bool SerialTab::log_to(const std::string& path, std::string* why) {
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

void SerialTab::log_stop() {
    std::lock_guard lock(mu_);
    if (!log_) return;
    if (!log_line_start_) std::fputc('\n', log_);
    std::fclose(log_);
    log_ = nullptr;
    log_path_.clear();
}

std::string SerialTab::log_path() {
    std::lock_guard lock(mu_);
    return log_path_;
}

void SerialTab::release() {
    want_release_ = true;
    poke();
    { std::lock_guard o(open_mu_); }  // (an opening under way is over: the reader sees the release before it opens again)
    for (const u64 until = now_ns() + 1'000'000'000ull; now_ns() < until; ::usleep(2000)) {
        std::lock_guard lock(mu_);
        if (state_ != kOpen) return;
    }
}

void SerialTab::reclaim() {
    want_release_ = false;
    poke();
}

void SerialTab::clear() {
    std::lock_guard lock(mu_);
    base_ += bytes_.size();
    bytes_.clear();
    chunks_.clear();
    const bool open_gap = !events_.empty() && events_.back().kind == kGap && !events_.back().back;
    if (open_gap) {
        Event e = events_.back();
        e.at = base_;
        events_.assign(1, e);
    } else {
        events_.clear();
    }
    rebuild();
}

bool SerialTab::released() { return want_release_; }

bool SerialTab::is_open() {
    std::lock_guard lock(mu_);
    return state_ == kOpen;
}

bool SerialTab::is(const std::string& serial, const std::string& port) {
    SerialPortId id;
    std::string dev;
    {
        std::lock_guard lock(mu_);
        id = id_;
        dev = device_;
    }
    if (!serial.empty() && serial == id.serial) return true;
    if (port.empty()) return false;
    if (port == id.path || port == dev) return true;
    const std::string real = serial_device(port);
    return real == serial_device(id.path) || (!dev.empty() && real == serial_device(dev));
}

std::vector<std::pair<std::string, int>> SerialTab::take_news() {
    std::lock_guard lock(mu_);
    return std::exchange(news_, {});
}

void SerialTab::set_scan(std::unique_ptr<SerialScan> scan) {
    std::lock_guard lock(mu_);
    scan_ = std::move(scan);
    scan_line_.clear();
}

std::string SerialTab::dump() {
    std::lock_guard lock(mu_);
    static const char* const kState[] = {"opening", "open", "gone", "released"};
    std::string out = std::string(kState[state_]) + " " + base_name(device_) + " " + serial_text(cfg_) + " return " + serial_return_name(ret_) +
                      (stamps_ ? " times" : "") + (hex_ ? " hex" : "") + " dtr " + (dtr_ ? "on" : "off") + " rts " + (rts_ ? "on" : "off");
    if (!id_.serial.empty()) out += " serial " + id_.serial;
    if (log_) out += " log " + base_name(log_path_);
    out += " kept " + std::to_string(bytes_.size()) + " gaps " + std::to_string(std::ranges::count(events_, kGap, &Event::kind));
    for (const std::string& a : asked_) out += " | " + a;
    return out;
}

// ---------------------------------------------------------------- the reader's

bool SerialTab::release_wanted() { return want_release_; }

int SerialTab::open_wait(std::stop_token stop) {
    std::string shown;  // (what the status line says now)
    const u64 began = now_ns();
    for (;;) {
        if (stop.stop_requested()) return -1;
        bool known;
        {
            std::lock_guard lock(mu_);
            known = identified_;
        }
        if (!known) {  // (the first time: the USB device it is on, to find it again by)
            const SerialPortId found = serial_identify(id_.path);
            std::lock_guard lock(mu_);
            if (id_.serial.empty() && !found.serial.empty()) {
                id_.serial = found.serial;
                id_.vid = found.vid, id_.pid = found.pid;
                id_.iface = found.iface;
                if (id_.name.empty()) id_.name = found.name;
            }
            identified_ = true;
        }
        SerialPortId id;
        SerialConfig cfg;
        u8 state;
        {
            std::lock_guard lock(mu_);
            id = id_;
            cfg = cfg_;
            state = state_;
        }
        std::string say;
        if (!want_release_) {
            std::string why;
            const std::string path = serial_locate(id, &why);
            if (!path.empty()) {
                int fd = -1;
                std::string err;
                bool opened = false;
                {
                    std::lock_guard o(open_mu_);
                    if (!want_release_) opened = serial_open(path, cfg, &fd, &err);
                }
                if (opened) {
                    std::unique_lock lock(mu_);
                    if (lines_set_ && !serial_set_lines(fd, dtr_, rts_, &err)) news_.push_back({"Cannot set DTR and RTS on " + base_name(path) + ": " + err, 1});
                    if (!lines_set_) dtr_ = rts_ = 1;
                    device_ = path;
                    shown_ = port_name(path);
                    if ((state_ == kGone || state_ == kReleased) && !events_.empty() && events_.back().kind == kGap) {
                        Event& e = events_.back();
                        e.back = wall_ns();
                        status_end(gap_text(e));
                        log_line(gap_text(e), e.back);
                    } else {
                        status_end({});
                    }
                    state_ = kOpen;
                    lock.unlock();
                    if (told) told();
                    return fd;
                }
                if (!want_release_) why = err;
            }
            if (!want_release_) {
                std::lock_guard lock(mu_);
                const std::string waiting = !why.empty() ? why + ", trying again" : "waiting for " + (id_.serial.empty() ? shown_ : id_.serial);
                if (state == kOpening) say = "[" + waiting + "]";
                else if (!events_.empty() && events_.back().kind == kGap) say = gap_text(events_.back(), waiting);
            }
        }
        if (!say.empty() && say != shown) {
            shown = say;
            {
                std::lock_guard lock(mu_);
                status(say);
            }
            if (told) told();
        }
        // Looked for again in a quarter of a second (a second, once it has been gone a while), or when asked.
        pollfd w{wake_r_, POLLIN, 0};
        ::poll(&w, 1, now_ns() - began < 10'000'000'000ull ? 250 : 1000);
        drain();
    }
}

std::string SerialTab::take(const char* p, size_t n, u64 ns) {
    std::lock_guard lock(mu_);
    log_bytes(p, n, ns);
    keep(p, n, ns);
    if (scan_) scan_lines(p, n, ns);
    else render(p, n, ns);
    return term_.take_reply();
}

void SerialTab::lost(const std::string& why, u64 ns) {
    {
        std::lock_guard lock(mu_);
        state_ = why == kReleasedWhy ? kReleased : kGone;
        Event e;
        e.at = base_ + bytes_.size();
        e.ns = ns;
        e.kind = kGap;
        e.text = why;
        events_.push_back(e);
        status(gap_text(e, why == kReleasedWhy || id_.serial.empty() ? std::string() : "waiting for " + id_.serial));
        log_line(gap_text(e), ns);
    }
    if (told) told();
}

void SerialTab::run_ops(int fd) {
    std::vector<Op> due;
    {
        std::lock_guard lock(mu_);
        const u64 now = now_ns();
        while (!ops_.empty() && ops_.front().due <= now) {
            due.push_back(std::move(ops_.front()));
            ops_.pop_front();
        }
    }
    if (due.empty()) return;
    for (const Op& op : due) {
        std::string err;
        bool ok = true;
        SerialConfig cfg;
        if (op.kind == kConfig) {
            {
                std::lock_guard lock(mu_);
                cfg = cfg_;
            }
            ok = serial_configure(fd, cfg, &err);
        } else if (op.kind == kBreak) {
            ok = serial_break(fd, &err);
        } else {
            ok = serial_set_lines(fd, op.dtr, op.rts, &err);
        }
        std::lock_guard lock(mu_);
        if (op.kind == kLines) {
            std::string a;
            if (op.dtr >= 0) a += std::string("dtr ") + (op.dtr ? "on" : "off");
            if (op.rts >= 0) a += std::string(a.empty() ? "" : " ") + "rts " + (op.rts ? "on" : "off");
            asked_.push_back(a + (ok ? "" : " (failed)"));
        }
        if (!ok) {
            const char* what = op.kind == kConfig ? "Cannot set " : op.kind == kBreak ? "Cannot send a break on " : "Cannot set DTR and RTS on ";
            news_.push_back({what + (op.kind == kConfig ? serial_text(cfg) + " on " : std::string()) + shown_ + ": " + err, 1});
        } else if (!op.say.empty()) {
            news_.push_back({op.say, 0});
        }
    }
    if (told) told();
}

int SerialTab::wait_ms(int most) {
    std::lock_guard lock(mu_);
    if (ops_.empty()) return most;
    const u64 now = now_ns(), due = ops_.front().due;
    return due <= now ? 0 : static_cast<int>(std::min<u64>((due - now) / 1'000'000ull + 1, static_cast<u64>(most)));
}

// ---------------------------------------------------------------- what came, kept

void SerialTab::keep(const char* p, size_t n, u64 ns) {
    const u64 at = base_ + bytes_.size();
    if (chunks_.empty() || ns / 1'000'000ull != chunks_.back().ns / 1'000'000ull) chunks_.push_back({at, ns});  // (a time to the millisecond, as it is shown)
    bytes_.append(p, n);
    if (bytes_.size() > kKeepMost || chunks_.size() > kChunksMost) trim();
}

// The oldest goes (at a line's start, where one is near), down to what is
// kept after a trim, or to half the times kept.
void SerialTab::trim() {
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

void SerialTab::render(const char* p, size_t n, u64 ns) {
    if (hex_) render_hex(p, n, ns);
    else render_text(p, n, ns);
    render_at_ += n;
}

// Each line from the device a line: LF alone is taken as CR LF (as a
// terminal's ONLCR would), CR alone goes back along the line (a progress
// count written over itself).
void SerialTab::render_text(const char* p, size_t n, u64 ns) {
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
void SerialTab::render_hex(const char* p, size_t n, u64 ns) {
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

void SerialTab::hex_end_row() {
    if (!hex_open_) return;
    std::string out;
    hex_pad(out, hex_row_.size(), hex_n_);
    hex_text(out, hex_row_);
    term_.feed(out);
    hex_open_ = false;
}

void SerialTab::end_line() {
    if (hex_open_) {
        hex_end_row();
    } else if (!line_start_) {
        term_.feed("\r\n");
    }
    line_start_ = true;
    after_cr_ = false;
}

// The host's time before a line, dim, the device's own pen put back after it.
void SerialTab::stamp(u64 ns) {
    const TermCell pen = term_.pen();
    TermCell dim;
    dim.attr = TermCell::kDim;
    term_.set_pen(dim);
    term_.feed(clock_text(ns, false) + " ");
    term_.set_pen(pen);
}

void SerialTab::dim_line(const std::string& text, bool open) {
    const TermCell pen = term_.pen();
    TermCell dim;
    dim.attr = TermCell::kDim;
    term_.set_pen(dim);
    term_.feed(text);
    term_.set_pen(pen);
    if (!open) term_.feed("\r\n");
}

void SerialTab::status(const std::string& text) {
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

void SerialTab::status_end(const std::string& last) {
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

std::string SerialTab::gap_text(const Event& e, const std::string& waiting) const {
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
void SerialTab::rebuild() {
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

void SerialTab::log_bytes(const char* p, size_t n, u64 ns) {
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

void SerialTab::log_line(const std::string& text, u64 ns) {
    if (!log_) return;
    if (!log_line_start_) std::fputc('\n', log_);
    std::fprintf(log_, "%s %s\n", clock_text(ns, true).c_str(), text.c_str());
    log_line_start_ = true;
    std::fflush(log_);
}

// ---------------------------------------------------------------- a recogniser of lines

void SerialTab::scan_lines(const char* p, size_t n, u64 ns) {
    size_t i = 0;
    std::vector<std::string> add;
    while (i < n) {
        const void* nl = std::memchr(p + i, '\n', n - i);
        const size_t end = nl ? static_cast<size_t>(static_cast<const char*>(nl) - p) + 1 : n;
        render(p + i, end - i, ns);
        scan_line_.append(p + i, end - i);
        i = end;
        if (!nl) break;
        while (!scan_line_.empty() && (scan_line_.back() == '\n' || scan_line_.back() == '\r')) scan_line_.pop_back();
        add.clear();
        scan_->line(scan_line_, ns, &add);
        scan_line_.clear();
        for (std::string& a : add) {
            Event e;
            e.at = render_at_;
            e.ns = ns;
            e.kind = kNote;
            e.text = std::move(a);
            end_line();
            dim_line(e.text, false);
            log_line(e.text, ns);
            events_.push_back(std::move(e));
        }
    }
    if (scan_line_.size() > 64 * 1024) scan_line_.erase(0, scan_line_.size() - 64 * 1024);  // (a "line" that never ends is not kept whole)
}

}  // namespace blit
