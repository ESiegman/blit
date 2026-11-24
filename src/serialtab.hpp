#pragma once
// A serial monitor's tab: a terminal tab whose program is a device on a
// serial port (appterm.cpp runs it, appserial.cpp has its commands). What
// the device sends is kept and drawn as streamtab.hpp says (as text, with
// the host's times, or as hex; logged; its lines told to a recogniser).
//
// The port is the device's, found by its USB serial number: when it hangs
// up (a reset, a flash: the device leaves USB and comes back) the tab
// stays and waits for the same serial number to come back, then opens it
// again, and one dim line says when it went and for how long. Flashing
// asks for it to be let go of (by serial number or port) and given back.
//
// A port on another machine (SerialPortId::host) is opened there by blit's
// agent, run as the port's pump (pump.hpp), which waits for it, lets go of
// it and does what is asked of its lines there; the tab is told what the
// pump says (far_open and the rest) and gives what is asked as its escapes
// (far_ops), and is otherwise as one here is.
//
// Threads: the tab's reader (TermView::serial_loop, serial_far_loop) opens
// the port (or runs its pump), reads it and does what is asked of the
// port's lines; the main thread asks.
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "common.hpp"
#include "serial.hpp"
#include "streamtab.hpp"

namespace blit {

class SerialTab final : public StreamTab {
public:
    // A tab drawing onto `term`, whose mutex is `mu` (the TermView's).
    SerialTab(SerialPortId id, SerialConfig cfg, SerialReturn ret, std::mutex& mu, Term& term);

    // ---- the main thread (each takes the mutex)

    std::string title() override;   // "cu.usbmodem1101 115200" ("ttyACM0 on box 115200" there), and "(gone)", "(released)", "(opening)" while it is not open
    std::string name() override;    // what it is called: "cu.usbmodem1101", "ttyACM0"
    std::string dump() override;    // (the tests: state, and the line changes asked for)
    SerialPortId id();
    std::string device();  // the path it is open at, or was last ("" never)
    SerialConfig config();
    void set_config(const SerialConfig& c);  // (applied by the reader, which says if it could not be)
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
    // Let go of for flashing (the reader closes the port: waited for, a
    // second at most), and given back (opened again as soon as it is there).
    void release();
    void reclaim();
    bool released();
    bool is_open();
    // This port: the USB serial number, or the port's path, is that, on
    // that machine ("" this one, else an ssh host).
    bool is(const std::string& serial, const std::string& port, const std::string& host);
    u64 came();  // (the benchmarks: how many bytes the device has sent since the tab opened)

    // ---- the reader's thread

    // The port opened, or opened again (after a hang-up or a release),
    // waiting as long as it takes; the waiting said on the tab's last line.
    // -1: the reader is to stop.
    int open_wait(std::stop_token stop);
    // The port has gone (hung up, or let go of for flashing: `why`).
    void lost(const std::string& why, u64 wall_ns);
    bool release_wanted();
    // The port's line changes, speed and breaks asked for, done now (those
    // whose time has come); how long until the next is due (ms, at most `most`).
    void run_ops(int fd);
    int wait_ms(int most);

    // ---- the reader's thread, for a port on another machine: what blit's
    // agent there, run as the port's pump (pump.hpp), says of it, and what
    // is to be asked of it. The tab is as one here is: open, gone, released,
    // its lines and speed changed as asked; but the pump opens it, waits for
    // it, and does what is asked, each at its time, there.

    void far_id(const SerialPortId& found);   // the USB device it is on
    void far_open(const std::string& path);   // opened (again), at that path there
    void far_wait(const std::string& why);    // not open, and waited for there (why "": not there)
    // Not open, and waited for meanwhile here: what the tab's last line says
    // ("connecting to box").
    void waiting(const std::string& text);
    // What has been asked of the port since (line changes, speeds, breaks),
    // as the escapes its pump takes, each numbered and with its time from
    // now; what each comes to, told back (lines that could not be set said,
    // as here).
    std::string far_ops();
    void far_done(u32 n, bool ok, const std::string& err);
    void far_lost();  // (the pump has ended: what it was asked, and is not yet done, is not)
    // Said on the bottom line (notify's level).
    void tell(const std::string& text, int level);

private:
    struct Op {
        u64 due = 0;
        u8 kind = 0;  // kLines, kConfig, kBreak
        int dtr = -1, rts = -1;
        std::string say;  // said once done ("" nothing)
        bool quiet = false;  // (DTR and RTS as they were, set again: only a failure is said)
    };
    enum : u8 { kLines, kConfig, kBreak };

    // (Each with the mutex held.)
    void opened_at(const std::string& path);  // open now, at that path: a gap that was open closed
    std::string waiting_text(const std::string& why, u8 state) const;  // the last line while it is waited for
    void done_at(const Op& op, bool ok, const std::string& err, const SerialConfig& cfg);  // an op done, or not: said
    void show_waiting(const std::string& say);  // (takes the mutex)

    SerialPortId id_;
    bool identified_ = false;
    std::string device_;  // where it is open, or was
    std::string shown_;   // what the title calls it: the device's name ("ttyACM0" for a by-id link)
    SerialConfig cfg_;
    int dtr_ = 1, rts_ = 1;
    bool lines_set_ = false;  // (changed from what opening gives: set again when it opens again)
    enum : u8 { kOpening, kOpen, kGone, kReleased } state_ = kOpening;
    std::atomic<bool> want_release_{false};
    std::mutex open_mu_;  // (held while the reader opens the port: a release waits for that to be over)
    std::deque<Op> ops_;
    std::vector<std::string> asked_;  // (the tests: line changes as they were done)
    // A port on another machine: the ops sent to its pump, by their number
    // (0: those set again quietly), until it says how each went; and the
    // last line said while it is waited for.
    std::map<u32, Op> sent_;
    u32 next_op_ = 1;
    std::string far_shown_;
};

}  // namespace blit
