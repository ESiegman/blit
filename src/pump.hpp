#pragma once
// blit's agent run as a pump: so that a serial port or a TCP port on the
// machine it is on behaves, to blit on another machine, as one here does.
// blit starts the agent there in one of these modes as it starts any
// program there (kSpawn, protocol.hpp), so the wire protocol is not
// changed; an agent of level 6 has them (kAgentLevel). Plain POSIX, built
// into the agent (and into blit, which says what goes to a pump and reads
// what it says back).
//
//   blit-agent --serial PATH CONFIG [SERIAL VID:PID IFACE]
//     The port at PATH opened as serial.hpp opens one (raw, held from other
//     programs), at CONFIG ("115200", or serial_parse's words: "74880 8E1
//     RTS/CTS"). What the device sends is the pump's output; its input is
//     what goes to the device, each 0xFF byte doubled, with what is asked
//     of the port escaped between the bytes (pump_lines and the rest,
//     below). What happens to the port it says on its errors, a line each
//     (PumpNews). The port is known by the USB device it is on (its serial
//     number, ids and interface: given, else read at PATH as it starts), so
//     that when it hangs up (a reset, a flash) the pump waits for that
//     device to come back, at whatever path, and opens it again, as a
//     serial tab here does (serialtab.hpp). It ends when its input ends.
//
//   blit-agent --connect HOST:PORT
//     A TCP connection to HOST:PORT made from that machine: the pump's input
//     goes to it, and what comes back is its output. The end of its input
//     ends the connection's sending side, and the connection's end ends its
//     output. Once connected it says "connected" on its errors; why it could
//     not connect is said there instead, and it ends with 1.
//
//   blit-agent --usb
//     The machine's USB devices (usb.hpp's usb_text), for blit's probe and
//     port pickers; then it ends.
#include <functional>
#include <string>
#include <string_view>

#include "common.hpp"
#include "serial.hpp"

namespace blit {

// ---------------------------------------------------------------- what goes to a serial pump

// The escape: 0xFF 0xFF is one 0xFF byte for the device; 0xFF, an
// operation, a length (one byte) and that many bytes is one of these. Each
// that can fail has a number, for the pump's "done N" or "fail N WHY", and
// is done `after` ms from when the pump reads it (a reset's steps go
// together and keep their timing whatever the connection's).
inline constexpr u8 kPumpEscape = 0xFF;
enum : u8 {
    kPumpConfig = 'c',   // u32 number, u16 after, then the speed, framing and flow control in serial_text's words
    kPumpLines = 'l',    // u32 number, u16 after, i8 DTR, i8 RTS (1 on, 0 off, -1 as it is)
    kPumpBreak = 'b',    // u32 number, u16 after
    kPumpRelease = 'r',  // the port let go of (a flash tool there is to open it): "released"
    kPumpReclaim = 'o',  // and taken again, as soon as it is there
};

// The device's bytes, each 0xFF doubled.
std::string pump_escape(std::string_view bytes);
std::string pump_config(u32 n, u32 after_ms, const SerialConfig& c);
std::string pump_lines(u32 n, u32 after_ms, int dtr, int rts);
std::string pump_break(u32 n, u32 after_ms);
std::string pump_release();
std::string pump_reclaim();

// ---------------------------------------------------------------- what a serial pump says

// A line of what it says, its fields after tabs:
//
//   id SERIAL VID:PID IFACE NAME   the USB device the port is on (as it starts, when it is on one)
//   open PATH                      the port opened (again), at PATH
//   wait WHY                       not open, and waited for: why ("": it is not there)
//   gone WHY                       it hung up, or could not be read ("hung up")
//   released                       let go of, as asked
//   done N                         operation N done
//   fail N WHY                     or not, and why
//   error WHY                      something else (what it was given cannot be used)
struct PumpNews {
    enum Kind : u8 { kId, kOpen, kWait, kGone, kReleased, kDone, kFail, kError } kind = kError;
    std::string text;  // the path, the why, or the device's name
    u32 n = 0;         // the operation's number
    SerialPortId id;   // (kId)
};
bool pump_news_parse(std::string_view line, PumpNews* out);

// ---------------------------------------------------------------- two streams joined

// What is read from a_in goes to b_out, and from b_in to a_out, each way on
// its own as the other side takes it (neither waits for the other: a far
// side that answers only once it has been sent everything still gets it).
// The end of what one side sends ends the other's writing: a socket's
// sending side is shut, a pipe closed (and set to -1 here, for its owner
// not to close again). A side that cannot be written to any more ends
// that way. Returns when both ways have ended, or when `stop` says so (it
// is asked a few times a second). A socket may be both of a side's.
struct PumpJoin {
    int a_in = -1, a_out = -1, b_in = -1, b_out = -1;
};
void pump_join(PumpJoin& j, const std::function<bool()>& stop = {});

// ---------------------------------------------------------------- the pumps (the agent's)

// blit-agent --serial's arguments after the mode, and --connect's target;
// each returns the agent's exit code.
int pump_serial(int argc, char** argv);
int pump_connect(const char* target);

}  // namespace blit
