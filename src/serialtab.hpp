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
// Threads: the tab's reader (TermView::serial_loop) opens the port, reads
// it and does what is asked of the port's lines; the main thread asks.
#include <atomic>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "common.hpp"
#include "serial.hpp"
#include "streamtab.hpp"

namespace blit {

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

class SerialTab final : public StreamTab {
public:
    // A tab drawing onto `term`, whose mutex is `mu` (the TermView's).
    SerialTab(SerialPortId id, SerialConfig cfg, SerialReturn ret, std::mutex& mu, Term& term);

    // ---- the main thread (each takes the mutex)

    std::string title() override;   // "cu.usbmodem1101 115200", and "(gone)", "(released)", "(opening)" while it is not open
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
    // This port: the USB serial number, or the port's path, is that.
    bool is(const std::string& serial, const std::string& port);

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

private:
    struct Op {
        u64 due = 0;
        u8 kind = 0;  // kLines, kConfig, kBreak
        int dtr = -1, rts = -1;
        std::string say;  // said once done ("" nothing)
    };
    enum : u8 { kLines, kConfig, kBreak };

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
};

}  // namespace blit
