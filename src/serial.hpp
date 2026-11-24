#pragma once
// A serial port, through termios alone (no libserialport: it is LGPL-3.0
// and adds nothing termios does not give): opened for one program only,
// raw, at a speed and framing; its DTR and RTS lines; a break. Plain POSIX
// (with IOKit's one ioctl for an odd speed on a Mac, and termios2 on
// Linux), so that blit's agent can be built with it too. The serial tab
// (serialtab.hpp) reads and writes the descriptor.
#include <string>
#include <string_view>

#include "common.hpp"

namespace blit {

struct SerialConfig {
    u32 baud = 115200;
    u8 data = 8;        // 5 to 8 bits
    char parity = 'N';  // N, E or O
    u8 stop = 1;        // 1 or 2
    char flow = 'N';    // N none, R RTS/CTS, X XON/XOFF
    bool operator==(const SerialConfig&) const = default;
};

// The framing in its usual short form ("8N1"), and the flow control after
// it when there is any ("8N1 RTS/CTS").
std::string serial_framing(const SerialConfig& c);
// "115200 8N1", the speed first.
std::string serial_text(const SerialConfig& c);
// What a person types for it, on top of c: a speed ("74880"), a framing
// ("8E1", "7o2"), a flow control ("rts/cts", "xon/xoff", "none"), any of
// them together. False (c as it was) if any word is none of those.
bool serial_parse(std::string_view text, SerialConfig* c);

// The port opened, raw, with c (non-blocking, closed on exec, not this
// process's controlling terminal), and kept from other programs while it
// is open (TIOCEXCL, and the lock pyserial's programs take: esptool's).
// False, with why in words, naming the port: "cu.usbmodem1101 is in use by
// another program", or on Linux the group a user needs to be in.
bool serial_open(const std::string& path, const SerialConfig& c, int* fd, std::string* err);
// The speed, framing and flow control of a port open already.
bool serial_configure(int fd, const SerialConfig& c, std::string* err);
// DTR and RTS set together (one change of the lines, not two): 1 on, 0
// off, -1 as they are.
bool serial_set_lines(int fd, int dtr, int rts, std::string* err);
bool serial_get_lines(int fd, bool* dtr, bool* rts);
bool serial_break(int fd, std::string* err);

// A port's stable name on Linux: its link in /dev/serial/by-id (which has
// the device's serial number in it), for a /dev/ttyACM0; "" when it has
// none, and on a Mac (whose /dev/cu.* names are stable already).
std::string serial_by_id(const std::string& dev);
// The device a path is (a by-id link followed); the path itself if it is
// not a link.
std::string serial_device(const std::string& path);

}  // namespace blit
