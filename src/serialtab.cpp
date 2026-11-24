#include "serialtab.hpp"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include "usb.hpp"

namespace blit {

namespace {

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

// ---------------------------------------------------------------- the tab

SerialTab::SerialTab(SerialPortId id, SerialConfig cfg, SerialReturn ret, std::mutex& mu, Term& term)
    : StreamTab(ret, mu, term), id_(std::move(id)), cfg_(cfg) {
    shown_ = port_name(id_.path.empty() ? id_.serial : id_.path);
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

std::string SerialTab::dump() {
    std::lock_guard lock(mu_);
    static const char* const kState[] = {"opening", "open", "gone", "released"};
    std::string out = std::string(kState[state_]) + " " + base_name(device_) + " " + serial_text(cfg_) + " return " + serial_return_name(ret_now()) +
                      (stamps_on() ? " times" : "") + (hex_on() ? " hex" : "") + " dtr " + (dtr_ ? "on" : "off") + " rts " + (rts_ ? "on" : "off");
    if (!id_.serial.empty()) out += " serial " + id_.serial;
    if (!log_file().empty()) out += " log " + base_name(log_file());
    out += " kept " + std::to_string(kept_bytes()) + " gaps " + std::to_string(gap_count());
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
                    if (state_ == kGone || state_ == kReleased) gap_end(wall_ns());
                    else status_end({});
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
                else say = last_gap_text(waiting);
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
        pollfd w{wake_fd(), POLLIN, 0};
        ::poll(&w, 1, now_ns() - began < 10'000'000'000ull ? 250 : 1000);
        drain();
    }
}

void SerialTab::lost(const std::string& why, u64 ns) {
    scan_gone();  // (a line cut short by it is none; a dump it was in is over)
    {
        std::lock_guard lock(mu_);
        state_ = why == kReleasedWhy ? kReleased : kGone;
        gap_begin(why, ns, why == kReleasedWhy || id_.serial.empty() ? std::string() : "waiting for " + id_.serial);
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

}  // namespace blit
