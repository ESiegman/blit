#pragma once
// Flashing a board, and programming an FPGA: which tool a board is flashed
// with (its board object's `flash`, else the one its probe's kind says),
// how each tool is run (a table, data, in flash.cpp: esptool, OpenOCD,
// probe-rs, J-Link Commander, picotool, dfu-util, pyOCD,
// STM32CubeProgrammer, openFPGALoader, Vivado, iceprog, ecpprog, Lattice's
// pgrcmd; and the copy of a .uf2 to an RP2040's or RP2350's boot ROM
// volume), what its image is (an ELF, ESP-IDF's flasher_args.json, a .uf2,
// a bitstream) and a hash of what it loads, so that a flash of an image
// that has not changed is not run again.
// appflash.cpp runs a flash as a task of the Build tab.
#include <string>
#include <string_view>
#include <vector>

#include "build.hpp"

namespace blit {

// How a tool's output reaches the task's terminal: the terminal itself,
// for a tool that draws its progress only to one (probe-rs's bars, which
// indicatif hides on a pipe), or a pipe, for one checked to print the same
// progress there (a line for each step, as a reader reads best).
enum class FlashTransport : u8 { kPty, kPipe };

// What is written: the board's flash; an FPGA's SRAM (gone when its power
// goes); an FPGA's flash.
enum class FlashMode : u8 { kFlash, kSram, kFpgaFlash };

// A tool, as data: board.flash's name for it, its programs (the first on
// the PATH of the machine it runs on is run), its transport, the kind of
// task it is, and its command line as parts, each left out whole when a
// ${name} in it has nothing.
struct FlashTool {
    std::string_view name;
    std::string_view programs[2];
    FlashTransport transport;
    std::string_view kind;  // "flash", or "program" (an FPGA)
    std::string_view parts[12];
};
// The tool board.flash names, or null.
const FlashTool* flash_tool(std::string_view name);
// The tool a board is flashed with: its flash, else the one its probe's
// kind says (an ESP32's USB-Serial/JTAG: esptool; an FTDI chip, as on
// Digilent's boards: openFPGALoader; ST-LINK and CMSIS-DAP: probe-rs;
// J-Link: J-Link Commander), else esptool for an ESP32 named as its chip
// with no probe (a serial bridge); "" when nothing says.
std::string flash_tool_for(const Board& b);
// The programs a flash with that tool may run, in the order looked for
// (Espressif's OpenOCD first for an ESP32).
std::vector<std::string> flash_programs(const Board& b, const std::string& tool);

// What is written, as found: files (from the folder) at their addresses
// ("" for an ELF or a bitstream, which say where they go), and what
// ESP-IDF's flasher_args.json says besides (its chip, esptool's options).
struct FlashImage {
    struct Part {
        std::string address, path;
    };
    std::vector<Part> parts;
    std::vector<std::string> options;  // ("--flash-mode", "dio", ...)
    std::string chip, before, after;
    bool no_stub = false;
    std::string volume;  // the UF2 copy's: the boot ROM's volume the .uf2 goes to,
    u64 flash_bytes = 0;  // and what of it goes into flash (uf2_refused)
    int spi_width = 1;    // an FPGA's bitstream: the SPI bus width it boots with (bitstream_spi_width)
};
// ESP-IDF's flasher_args.json (`dir`: the folder it is in, from the
// project's): its flash_files at their addresses, write_flash_args, and
// extra_esptool_args (in esptool v5's words: dashes for underscores).
bool esp_flasher_args(std::string_view text, const std::string& dir, FlashImage* out, std::string* why);

// The segments an ELF loads (PT_LOAD, with bytes in the file): where each
// goes (its physical address) and its bytes. False if it is not an ELF.
bool elf_loads(std::string_view bytes, std::vector<std::pair<u64, std::string_view>>* out);
// A hash of what an image puts into the target: an ELF's loadable
// segments (so that a build that changes only its debug information is
// the same image), else the bytes whole.
u64 flash_hash(std::string_view bytes);

// OpenOCD's interface and target scripts for a board (its flash object's
// interface and target, else the ones its probe's kind and its chip say;
// "" when neither does): its debugging takes the same as its flashing.
std::string openocd_interface(const Board& b);
std::string openocd_target(const Board& b);
// Whether a board's chip is an ESP32 (any: "ESP32-S3" is "esp32s3").
bool esp_chip(std::string_view chip);

// The SPI bus width (1, 2 or 4) a 7-series bitstream (a .bit, or a .bin
// without its header) boots from its flash with: its write of the BSPI
// register after the sync word (bits 9-8; Vivado writes one only for
// SPI_BUSWIDTH 2 or 4), else 1. Vivado's write_cfgmem takes no other
// -interface for it. Cheap: only the first few KB are looked at.
int bitstream_spi_width(std::string_view bytes);

// The flash part Vivado writes an FPGA's flash as (its name in
// get_cfgmem_parts): the board's flash.cfgmem, else the one blit knows for
// its board (flash.board, openFPGALoader's name for it); "" if neither.
std::string vivado_cfgmem(const Board& b);

// What a flash runs: the command line for the shell (the tool's program by
// its path, the transport applied), the kind of task, and what else a
// flash is the same flash only with (the tool, the probe, the target and
// the mode; the image's hash goes with it). `why`: it cannot be run.
struct FlashPlan {
    std::string tool, kind, command, same, why;
    FlashTransport transport = FlashTransport::kPty;
};
FlashPlan flash_plan(const Board& b, const std::string& tool, const std::string& program, const FlashImage& image, FlashMode mode);


// Vivado where AMD installs it, on the machine whose paths start with
// `there` ("" here, "ssh://host" there): the newest version's bin/vivado
// in /tools/Xilinx or /opt/Xilinx, as that machine's path; "" if none.
// (A worker's call: folders are listed, there through the agent.)
std::string vivado_installed(const std::string& there);

// A Raspberry Pi boot ROM's volume (BOOTSEL), as mounted: its path, and
// its INFO_UF2.TXT's Board-ID ("RPI-RP2" an RP2040's, "RP2350").
struct RpVolume {
    std::string path, board;
};
// Those mounted on the machine whose paths start with `there` ("" here,
// "ssh://host" there): each folder in /Volumes, /media, /media/USER,
// /run/media/USER whose INFO_UF2.TXT a Raspberry Pi boot ROM wrote
// (BLIT_UF2_VOLUMES, folders split by ':', in their place, for the tests).
// A worker's call: folders are listed, there through the agent.
std::vector<RpVolume> rp_volumes(const std::string& there);
// Why the boot ROM whose Board-ID is `board` would not take this .uf2 ("":
// it would): it is not a UF2, or (an RP2040's) its blocks are not for the
// RP2040's family; and the bytes it would write into flash (the blocks'
// payloads it takes, half the file's as the Pico SDK writes them).
std::string uf2_refused(std::string_view bytes, std::string_view board, u64* flash_bytes = nullptr);
// A Pico SDK build's platform ("rp2040", "rp2350-arm-s", "rp2350-riscv"),
// from its CMakeCache.txt's text; "" if it is not a Pico SDK build.
std::string pico_platform(std::string_view cmake_cache);

// openFPGALoader's cable (-c) for an FTDI chip, by its ids ("0403:6010"):
// Digilent's (their boards' and cables' product strings say so) or the
// plain chip's; "" for another.
std::string ofl_ftdi_cable(std::string_view usb, bool digilent);

}  // namespace blit
