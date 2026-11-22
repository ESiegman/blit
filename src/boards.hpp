#pragma once
// The Boards view: the FPGA boards and cables openFPGALoader sees on a
// machine (--scan-usb, then --detect on each cable it can ask through, run
// headless through Engine::run), as rows of the probe picker
// (appprobe.cpp), each after the row of the USB device it is on. A board is
// named from openFPGALoader's own list of boards on Digilent's cables (data,
// in boards.cpp), matched on the FPGA --detect finds; Enter writes it into
// the run configuration's board as openFPGALoader's board (or, with no
// board named, its cable), with the probe's serial number.
#include <string>
#include <string_view>
#include <vector>

#include "probes.hpp"

namespace blit {

// openFPGALoader's --scan-usb: a line for each cable it knows (an FTDI
// chip by its kind, "FTDI2232"; another by its cable's name, "dirtyJtag").
struct OflCable {
    std::string bus, device, usb, probe_type, manufacturer, serial, product;  // (usb: "0403:6010")
};
std::vector<OflCable> ofl_scan(std::string_view out);
// openFPGALoader's --detect: each FPGA on the cable's chain.
struct OflChip {
    std::string idcode, manufacturer, family, model;  // ("0x362d093", "xilinx", "artix a7 35t", "xc7a35")
};
std::vector<OflChip> ofl_detect(std::string_view out);
// The arguments --detect takes for a cable (-c NAME, and its serial number
// or its place on the bus); empty for one it is not asked about (a debug
// probe for MCUs that openFPGALoader knows too).
std::vector<std::string> ofl_detect_args(const OflCable& c);
// The Boards view's rows: for each FPGA found on a cable, the boards
// openFPGALoader names that have it there (each a row: the Basys 3 and the
// Cmod A7-35T have the same Artix-7), else the FPGA on its cable; and each
// cable that is not among the probe picker's rows (`native`: their console
// ports are taken from there).
std::vector<ProbeRow> ofl_rows(const std::vector<OflCable>& cables, const std::vector<std::vector<OflChip>>& chips, const std::vector<ProbeRow>& native,
                               const std::string& host);
// The picker's rows with those put in: each after the rows of the device it
// is on, those on none first.
std::vector<ProbeRow> ofl_merge(std::vector<ProbeRow> native, const std::vector<ProbeRow>& boards);
// The rows openFPGALoader gives on a machine ("" here): it is run there
// (headless: a worker's call, each run limited to `ms`). None when it is
// not on that machine's PATH.
std::vector<ProbeRow> fpga_boards(const std::string& host, const std::vector<ProbeRow>& native, u32 ms = 15'000);

}  // namespace blit
