#pragma once
// What a USB device is, to someone who works on boards: a debug probe
// (ST-LINK, J-Link, CMSIS-DAP, Espressif's USB-Serial/JTAG, Black Magic),
// an FPGA's programmer (an FTDI chip, Digilent's), a chip's boot ROM, or a
// serial bridge; named from a table of vendor and product ids (data, in
// probes.cpp, each id with where it comes from). "Choose the probe…"
// (appprobe.cpp) lists them as rows, a probe and its serial port as one,
// and writes the one chosen into the run configuration's `board` object.
#include <string>
#include <string_view>
#include <vector>

#include "build.hpp"
#include "json.hpp"
#include "usb.hpp"

namespace blit {

// What a row is: a debug probe, an FPGA's programmer, a boot ROM, a serial
// port alone, or (only when there is nothing else) some other device.
enum class ProbeRole : u8 { kProbe, kFpga, kBoot, kSerial, kOther };

// An entry of the table: a device whose ids are vid and pid (pid's bits in
// mask: 0xffff, that pid alone), and, where it is set, whose manufacturer
// or product string has `says` in it.
struct ProbeKind {
    u16 vid, pid, mask;
    const char* says;  // "" for any
    const char* kind;  // the board object's probe.kind: "stlink", "jlink", "cmsis-dap", "esp-usb-jtag", "blackmagic", "ftdi", ...
    const char* name;  // what the row calls it: "ST-LINK V3E"
    ProbeRole role;
    // Which of its serial ports is the console: "" the first, "last" the
    // last (an FT2232's channel B), else the one whose interface's name has
    // it in; and the one that is the probe's GDB server, by the same rule.
    const char* console = "";
    const char* gdb = nullptr;
};

// The entry a device is, or null: the first that matches in the table;
// then any device whose product or an interface's name says "CMSIS-DAP"
// (which the CMSIS-DAP specification asks a probe's product string to
// have), as an entry of kind "cmsis-dap" named by its product string.
const ProbeKind* probe_kind(const UsbDevice& d);

// A row of the picker: a device, named, with its serial number and its
// serial port (the console; for Black Magic, the UART port, with its GDB
// server's port in `gdb`).
struct ProbeRow {
    ProbeRole role = ProbeRole::kOther;
    std::string kind;    // as ProbeKind::kind; "serial" for a port alone, "usb" for another device
    std::string name;    // "ST-LINK V3", "CP210x serial bridge (Silicon Labs CP2102N USB to UART Bridge Controller)"
    std::string serial;  // as the tools take it (an ST-LINK V2's binary one in hex, as probe-rs has it)
    std::string usb;     // "0483:374e"
    std::string port;    // its console's path, if it has one
    std::string gdb;     // Black Magic's GDB server port
    std::string host;    // the machine it is on ("" here)
    std::string text;    // the row as the picker shows it
};

// The rows for these devices: each device the table names, and each other
// device with a serial port; the rest (keyboards, cameras: hubs never)
// only when there is nothing else. Probes and programmers first, then boot
// ROMs, then ports alone; by name and serial number within each. `host`:
// the machine they are on, which each row says ("" here).
std::vector<ProbeRow> probe_rows(const std::vector<UsbDevice>& devices, const std::string& host = {});

// The probe kinds' names the board object takes for its flash tool.
inline constexpr std::string_view kFlashTools[] = {"esptool", "openocd", "probe-rs", "jlink", "openfpgaloader"};

// The configuration named `config` in a .blit/run.json's text with its
// board's probe and port set to the row's (probe.kind, probe.serial,
// probe.usb, probe.gdb; serial.port), each member of the board the row has
// nothing for removed and the rest of the file kept as it was written:
// its other configurations, members, order, spacing and comments-free
// layout. A board object is made if it has none. False (and why) when the
// text is not such a file or has no such configuration.
bool board_set_probe(std::string& text, std::string_view config, const ProbeRow& row, std::string* why);

// What a board's probe is called (the table's name for its ids, else its
// kind) and its serial number; "" when it has no probe.
std::string board_probe_text(const Board& b);

// A board object read from run.json (read_run_configs's part): what is
// wrong with it, if anything (config: the configuration's name, to say so).
void board_read(json::Value board, const std::string& config, Board* out, std::string* problem);

}  // namespace blit
