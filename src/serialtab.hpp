#pragma once
// A serial monitor's tab: a terminal tab whose program is a device on a
// serial port (appterm.cpp runs it, appserial.cpp has its commands). What
// the device sends is kept here as it came (the bytes, when each part of
// them came, and when the port went and came back), and drawn from that
// onto the tab's Term: as text, each line from the device a line whether
// it ends with CR LF or LF alone; with the host's time before each line
// (a column turned on and off, the times stored as the lines came); or as
// hex beside the text, the same bytes. Turning either on or off draws the
// tab again from what is kept.
//
// The port is the device's, found by its USB serial number: when it hangs
// up (a reset, a flash: the device leaves USB and comes back) the tab
// stays and waits for the same serial number to come back, then opens it
// again, and one dim line says when it went and for how long. Flashing
// asks for it to be let go of (by serial number or port) and given back.
//
// Threads: the tab's reader (TermView::serial_loop) opens the port, reads
// it and does what is asked of the port's lines; the main thread asks. All
// that is shared is under the tab's mutex, which the Term's drawing holds
// too, but the recogniser of lines (SerialScan), which has a mutex of its
// own and is told the lines before the tab's is taken.
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "serial.hpp"

namespace blit {

class Term;

// What a port is: its path, and (a port on a USB device) the device's ids,
// its serial number and the interface the port is on, to find it again by.
struct SerialPortId {
    std::string path;     // as it was asked for: /dev/cu.usbmodem1101, /dev/serial/by-id/usb-..., /dev/ttyACM0
    std::string serial;   // the USB device's serial number ("": none known; found again at its path)
    u16 vid = 0, pid = 0;
    int iface = -1;       // (a device with two ports, an FT2232: the one that was opened)
    std::string name;     // what the device is called ("Pico SDK USB serial (RP2040)")
};

// The port at a path, named from the USB devices plugged in (just the path
// when it is on none). Slow enough to be a worker's call.
SerialPortId serial_identify(const std::string& path);
// Where that port is now: on the USB device with its serial number (and
// ids, and interface), else at its path, if it is there; "" when it is not
// (why empty: it is not plugged in). A worker's call.
std::string serial_locate(const SerialPortId& id, std::string* why);

// What Return sends: CR (a terminal's own), LF, or both.
enum class SerialReturn : u8 { kCR, kLF, kCRLF };
const char* serial_return_name(SerialReturn r);  // "CR", "LF", "CR LF"
bool serial_return_parse(std::string_view s, SerialReturn* r);  // "cr", "lf", "crlf", "cr lf" (any case)

// A recogniser of lines (the crash decoder's place, E7: appterm.cpp's
// SerialCrash): told each whole line the device sends (without its line
// end, escapes and all), on the reader's thread as it comes, before the
// tab's mutex is taken (so that it may take its time: read an ELF); the
// lines it adds, plain text, go after that line, dim, in the tab, what is
// kept of it, and its log. Told when the port goes, too (a reset): what it
// was in the middle of is over. Attached with SerialTab::set_scan; with
// none, lines are not put together at all.
struct SerialScan {
    virtual ~SerialScan() = default;
    virtual void line(std::string_view text, u64 wall_ns, std::vector<std::string>* add) = 0;
    virtual void gone() {}
};

class SerialTab {
public:
    // A tab drawing onto `term`, whose mutex is `mu` (the TermView's).
    SerialTab(SerialPortId id, SerialConfig cfg, SerialReturn ret, std::mutex& mu, Term& term);
    ~SerialTab();
    SerialTab(const SerialTab&) = delete;
    SerialTab& operator=(const SerialTab&) = delete;

    // ---- the main thread (each takes the mutex)

    std::string title();   // "cu.usbmodem1101 115200", and "(gone)", "(released)", "(opening)" while it is not open
    SerialPortId id();
    std::string device();  // the path it is open at, or was last ("" never)
    std::string name();    // what it is called: "cu.usbmodem1101", "ttyACM0"
    SerialConfig config();
    void set_config(const SerialConfig& c);  // (applied by the reader, which says if it could not be)
    SerialReturn return_mode();
    void set_return(SerialReturn r);
    std::string return_bytes();
    bool stamps();
    void set_stamps(bool on);  // (the tab drawn again)
    bool hex();
    void set_hex(bool on);
    // DTR and RTS: 1 on, 0 off, -1 as they are; and as last set (both on
    // when a port opens).
    void set_lines(int dtr, int rts);
    void lines(bool* dtr, bool* rts);
    // The board reset through them: ESP32's (EN low through RTS, IO0 high
    // through DTR, as esptool's hard reset), Arduino's (DTR and RTS low,
    // then high: its auto-reset capacitor, as avrdude's).
    void reset_esp();
    void reset_arduino();
    void send_break();
    // Everything that comes written to a file as well, from now: each line
    // after its date and time; false (why) when it cannot be written.
    bool log_to(const std::string& path, std::string* why);
    void log_stop();
    std::string log_path();
    // Let go of for flashing (the reader closes the port: waited for, a
    // second at most), and given back (opened again as soon as it is there).
    void release();
    void reclaim();
    bool released();
    bool is_open();
    // This port: the USB serial number, or the port's path, is that.
    bool is(const std::string& serial, const std::string& port);
    // What the reader has to say (a line that could not be set, a reset
    // done), each with its level for notify; taken once.
    std::vector<std::pair<std::string, int>> take_news();
    void set_scan(std::unique_ptr<SerialScan> scan);
    void clear();  // what is kept and shown goes (Cmd+K), but a gap still open
    std::string dump();  // (the tests: state, and the line changes asked for)

    // ---- the reader's thread

    // Called when something changed that the main thread should see (the
    // TermView's wake), set before the reader starts.
    std::function<void()> told;
    int wake_fd() const { return wake_r_; }
    void drain();  // (the wake pipe emptied)
    // The port opened, or opened again (after a hang-up or a release),
    // waiting as long as it takes; the waiting said on the tab's last line.
    // -1: the reader is to stop.
    int open_wait(std::stop_token stop);
    // What came, at this wall-clock time: kept, logged, drawn. What the
    // Term answers the device is returned, to be sent.
    std::string take(const char* p, size_t n, u64 wall_ns);
    // The port has gone (hung up, or let go of for flashing: `why`).
    void lost(const std::string& why, u64 wall_ns);
    bool release_wanted();
    // The port's line changes, speed and breaks asked for, done now (those
    // whose time has come); how long until the next is due (ms, at most `most`).
    void run_ops(int fd);
    int wait_ms(int most);

    static u64 wall_ns();  // (now, by the wall clock: what the times are)
    static constexpr const char* kReleasedWhy = "released for flashing";  // (lost's why, for a release)

private:
    struct Event {
        u64 at = 0;              // the place in what came: after the bytes before it
        u64 ns = 0, back = 0;    // a gap: when the port went, and came back (0: not yet)
        u8 kind = 0;             // kGap, kNote
        std::string text;        // a gap: why; a note: its line
    };
    enum : u8 { kGap, kNote };
    struct Chunk {
        u64 at = 0, ns = 0;  // the bytes from `at` came at ns
    };
    struct Op {
        u64 due = 0;
        u8 kind = 0;  // kLines, kConfig, kBreak
        int dtr = -1, rts = -1;
        std::string say;  // said once done ("" nothing)
    };
    enum : u8 { kLines, kConfig, kBreak };

    void poke();
    void keep(const char* p, size_t n, u64 ns);
    void trim();
    void render(const char* p, size_t n, u64 ns);
    void render_text(const char* p, size_t n, u64 ns);
    void render_hex(const char* p, size_t n, u64 ns);
    void hex_end_row();
    void end_line();  // (the drawing at the start of a line)
    void stamp(u64 ns);
    void dim_line(const std::string& text, bool open);
    void status(const std::string& text);  // (the last line, rewritten as what is waited for changes)
    void status_end(const std::string& last);  // (and that line made `last`, or taken away: "")
    std::string gap_text(const Event& e, const std::string& waiting = {}) const;
    void rebuild();
    void log_bytes(const char* p, size_t n, u64 ns);
    void log_line(const std::string& text, u64 ns);
    void scan_lines(const char* p, size_t n, u64 ns);  // (scan_mu_: into notes_)

    std::mutex& mu_;
    Term& term_;
    int wake_r_ = -1, wake_w_ = -1;

    SerialPortId id_;
    bool identified_ = false;
    std::string device_;  // where it is open, or was
    std::string shown_;   // what the title calls it: the device's name ("ttyACM0" for a by-id link)
    SerialConfig cfg_;
    SerialReturn ret_ = SerialReturn::kCR;
    bool stamps_ = false, hex_ = false;
    int dtr_ = 1, rts_ = 1;
    bool lines_set_ = false;  // (changed from what opening gives: set again when it opens again)
    enum : u8 { kOpening, kOpen, kGone, kReleased } state_ = kOpening;
    std::atomic<bool> want_release_{false};
    std::mutex open_mu_;  // (held while the reader opens the port: a release waits for that to be over)
    std::deque<Op> ops_;
    std::vector<std::pair<std::string, int>> news_;
    std::vector<std::string> asked_;  // (the tests: line changes as they were done)

    // What came, kept: the bytes (`base_` is the place of the first, in all
    // that came since the tab opened), when each part came, and the gaps
    // and notes, all in order of place.
    std::string bytes_;
    u64 base_ = 0;
    std::deque<Chunk> chunks_;
    std::deque<Event> events_;

    // Drawing: at the start of a line; the last byte drawn a CR; the hex
    // row being filled (its place, its time, its bytes so far).
    bool line_start_ = true, after_cr_ = false;
    u64 render_at_ = 0;
    bool hex_open_ = false;
    std::string hex_row_;
    size_t hex_n_ = 16;  // (the bytes in the row being filled: 16, or 8 where the tab is too narrow for 16)
    std::string scratch_;
    // The status line on the tab's last row while the port is not open:
    // the row it starts on, counted as the Term counts lines (~0: none).
    u64 status_row_ = ~0ull;

    FILE* log_ = nullptr;
    std::string log_path_;
    bool log_line_start_ = true;

    // The recogniser, and the line it is to be told once it ends: the
    // reader's, under their own mutex (set from the main thread), never
    // under the tab's. The lines it added to a piece of what came, each
    // after which byte of it (the reader's).
    std::mutex scan_mu_;
    std::unique_ptr<SerialScan> scan_;
    std::string scan_line_;
    std::vector<std::string> scan_add_;
    std::vector<std::pair<size_t, std::string>> notes_;
};

}  // namespace blit
