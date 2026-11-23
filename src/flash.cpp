#include "flash.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "fsio.hpp"
#include "json.hpp"

namespace blit {

namespace {

using enum FlashTransport;

// How each tool is run. Each part's ${name} is put in quoted for the shell
// (and the part left out when it has nothing); each tool's options, as
// its own help printed them, and its transport, as checked, beside it.
constexpr FlashTool kTools[] = {
    // esptool v5.6.0 (`esptool --help`, `esptool write-flash --help`):
    // --chip, -p, --before and --after go before the command; write-flash
    // reads back what it wrote by itself ("Hash of data verified."; v5 has
    // no --verify). A pipe: on one its progress (esp-pylib 1.2.0's
    // logger.py, lines 735-750 and 851-864) is a whole line for each step,
    // each flushed, which the esptool5 sample's script printed so.
    {"esptool", {"esptool", "esptool.py"}, kPipe, "flash", {"--chip ${chip}", "-p ${port}", "--before ${before}", "--after ${after}", "${stub}", "write-flash", "${options}", "${files}"}},
    // OpenOCD 0.12.0: the interface's script, the probe's serial number
    // (`adapter serial`), the target's script, then `program FILE [ADDRESS]
    // verify reset exit` (src/flash/startup.tcl), or Espressif's
    // `program_esp FILE ADDRESS verify` for each file (v0.12.0-esp32-20260831
    // target/esp_common.cfg lines 535-709). A pipe: it draws nothing in
    // place, and printed the same bytes on a pipe as on a terminal (no probe).
    {"openocd", {"openocd", ""}, kPipe, "flash", {"-f ${interface}", "-c ${adapter}", "-f ${target}", "${program}"}},
    // probe-rs 0.32.0 (`probe-rs download --help`): --probe VID:PID:SERIAL,
    // --non-interactive, --verify, --reset. A terminal: its bars are
    // indicatif's, which draws nothing when its output is not one (indicatif
    // 0.18.0 src/draw_target.rs lines 65-70).
    {"probe-rs", {"probe-rs", ""}, kPty, "flash",
     {"download", "--chip ${chip}", "--probe ${probe}", "--non-interactive", "--binary-format ${format}", "--base-address ${address}", "--verify", "--reset", "${image}"}},
    // J-Link Commander with a command file of its commands (kJLinkScript;
    // SEGGER's wiki, "J-Link Commander": -NoGui, -ExitOnError, -USB SERIAL,
    // -CommandFile; loadfile reads back what it wrote by itself). A
    // terminal: not installed here (SEGGER's licence), so not checked.
    {"jlink", {"JLinkExe", ""}, kPty, "flash", {"-NoGui 1", "-ExitOnError 1", "-USB ${serial}", "-CommandFile ${script}"}},
    // openFPGALoader v1.2.0 (`openFPGALoader --help`): -b a board or -c a
    // cable, --usb-serial-num (its --ftdi-serial is deprecated), into SRAM
    // unless -f, --verify (SPI flash only). A pipe: when its output is not a
    // terminal its bar is a whole line for each step (src/progressBar.cpp
    // lines 20 and 58), each flushed (src/display.cpp).
    {"openfpgaloader", {"openFPGALoader", ""}, kPipe, "program", {"-b ${board}", "-c ${cable}", "--usb-serial-num ${serial}", "${write}", "${image}"}},
    // AMD Vivado in batch mode with a Tcl file of its hardware manager's
    // commands (kVivadoScript; -mode batch, -nojournal, -nolog, -notrace,
    // -source): into the FPGA's SRAM, or its flash. A pipe: it draws
    // nothing in place (2026.2 on the lab box, loading the owner's Basys 3
    // and writing its flash, 2026-11-23).
    {"vivado", {"vivado", ""}, kPipe, "program", {"-mode batch", "-nojournal", "-nolog", "-notrace", "-source ${script}"}},
};

// J-Link Commander's command file, a line each (SEGGER's wiki, "J-Link
// Commander": Device, SelectInterface, Speed, Connect, LoadFile, Reset, Go,
// Exit).
constexpr std::string_view kJLinkScript[] = {"device ${device}", "si ${si}", "speed ${speed}", "connect", "loadfile ${image}", "r", "g", "exit"};

// Vivado's: the hardware manager, the cable whose name has the probe's
// serial number (Digilent's are "localhost:3121/xilinx_tcf/Digilent/SERIAL"
// and a letter), the device whose part begins the board's chip (its PART is
// "xc7a35t" for an "xc7a35tcpg236-1"; else the first of AMD's), the
// bitstream into it (UG835: open_hw_target, get_hw_targets, get_hw_devices
// -filter, program_hw_devices). A failing command ends the run with an
// "ERROR: [...]" line and exit status 1; the script's own failure (no such
// device: Tcl's error would print its message bare) says ERROR so too.
// The hw_server connect_hw_server starts when none answers on its port
// keeps the cable for a minute after Vivado ends (openFPGALoader cannot
// claim it then): one the run started is ended with it, by its name (not
// its command line, which the shell running this one has in its own).
//
// Into its flash (UG908, "Programming Configuration Memory Devices"; the
// commands as Vivado's hardware manager journals them): the flash part
// first (a name get_cfgmem_parts does not have fails before the cable is
// touched), the bitstream made a configuration memory file beside this
// file (write_cfgmem: the part's size, its MEM_DENSITY in Mb, in MB; the
// bitstream's own SPI bus width, the only -interface it takes), the part
// set on the device with what is done to it (the file's sectors erased
// and written, then read back; no blank check, as they are erased), the
// FPGA loaded with Vivado's design that writes the flash for it
// (PROGRAM.HW_CFGMEM_BITFILE), program_hw_cfgmem, then the FPGA booted
// from its flash. A part that is not the board's fails before anything is
// erased ("Part selected s25fl032p, but part mx25l3273f detected", 2026.2
// on the owner's Basys 3).
enum VivadoWhen : u8 { kBoth, kIntoSram, kIntoFlash };
struct VivadoLine {
    VivadoWhen when;
    std::string_view text;
};
constexpr VivadoLine kVivadoScript[] = {
    {kBoth, "set ours [catch {close [socket localhost 3121]}]"},
    {kIntoFlash, "set p [lindex [get_cfgmem_parts ${cfgmem}] 0]"},
    {kIntoFlash, "set m [info script].mcs"},
    {kIntoFlash, "write_cfgmem -force -format mcs -size [expr {max(1, [get_property MEM_DENSITY $p] / 8)}] -interface SPIx${width} -loadbit [list up 0x0 ${image}] -file $m"},
    {kBoth, "open_hw_manager"},
    {kBoth, "connect_hw_server -allow_non_jtag"},
    {kBoth, "open_hw_target [lindex [get_hw_targets ${target}] 0]"},
    {kBoth, "set d [lindex [get_hw_devices -filter {PART =~ x*}] 0]"},
    {kBoth, "foreach h [get_hw_devices] {if {[string match [get_property PART $h]* ${chip}]} {set d $h}}"},
    {kBoth, "if {[string match [get_property PART $d]* ${chip}] == 0} {puts {ERROR: No device on the cable is a ${chip}}; exit 1}"},
    {kBoth, "current_hw_device $d"},
    {kIntoSram, "set_property PROGRAM.FILE ${image} [current_hw_device]"},
    {kIntoSram, "program_hw_devices [current_hw_device]"},
    {kIntoFlash, "create_hw_cfgmem -hw_device $d $p"},
    {kIntoFlash, "set c [get_property PROGRAM.HW_CFGMEM $d]"},
    {kIntoFlash, "set_property PROGRAM.FILES [list $m] $c"},
    {kIntoFlash, "set_property PROGRAM.ADDRESS_RANGE use_file $c"},
    {kIntoFlash, "set_property PROGRAM.UNUSED_PIN_TERMINATION pull-none $c"},
    {kIntoFlash, "set_property PROGRAM.BLANK_CHECK 0 $c"},
    {kIntoFlash, "set_property PROGRAM.ERASE 1 $c"},
    {kIntoFlash, "set_property PROGRAM.CFG_PROGRAM 1 $c"},
    {kIntoFlash, "set_property PROGRAM.VERIFY 1 $c"},
    {kIntoFlash, "set_property PROGRAM.CHECKSUM 0 $c"},
    {kIntoFlash, "create_hw_bitstream -hw_device $d [get_property PROGRAM.HW_CFGMEM_BITFILE $d]"},
    {kIntoFlash, "program_hw_devices $d"},
    {kIntoFlash, "program_hw_cfgmem -hw_cfgmem $c"},
    {kIntoFlash, "boot_hw_device $d"},
    {kBoth, "close_hw_target"},
    {kBoth, "disconnect_hw_server"},
    {kBoth, "close_hw_manager"},
    {kBoth, "if {$ours} {catch {exec pkill -u $::env(USER) -x hw_server}; catch {exec pkill -u $::env(USER) -x cs_server}}"},
};

// The flash part of each board blit knows (openFPGALoader's name for it,
// as boards.cpp has them), as Vivado's get_cfgmem_parts names it: the
// Basys 3's Macronix MX25L3233F (JEDEC 0xc22016, as openFPGALoader read it
// on the owner's board) is Vivado 2026.2's mx25l3273f, whose
// MEM_DEVICE_ALIAS is mx25l3233f (it has no part of that name). Boards
// whose flash changed from one revision to the next are left to their
// flash.cfgmem.
constexpr std::pair<std::string_view, std::string_view> kCfgmem[] = {
    {"basys3", "mx25l3273f-spi-x1_x2_x4"},
};

// Where AMD installs Vivado when it is not on the PATH: FOLDER/VERSION/
// Vivado/bin/vivado (2025.1 on, as on the lab box), FOLDER/Vivado/VERSION/
// bin/vivado (before). BLIT_VIVADO_HOMES, a list split by ':', for the tests.
constexpr std::string_view kVivadoHomes[] = {"/tools/Xilinx", "/opt/Xilinx"};

// The tool a probe's kind is flashed with, when the board says none.
constexpr std::pair<std::string_view, std::string_view> kKindTool[] = {
    {"esp-usb-jtag", "esptool"}, {"ftdi", "openfpgaloader"}, {"stlink", "probe-rs"}, {"cmsis-dap", "probe-rs"}, {"jlink", "jlink"},
};

// OpenOCD's interface script for a probe's kind (OpenOCD 0.12.0's
// tcl/interface, and Espressif's for its USB-Serial/JTAG).
constexpr std::pair<std::string_view, std::string_view> kInterface[] = {
    {"stlink", "interface/stlink.cfg"},           {"cmsis-dap", "interface/cmsis-dap.cfg"},           {"jlink", "interface/jlink.cfg"},
    {"esp-usb-jtag", "interface/esp_usb_jtag.cfg"}, {"esp-usb-bridge", "interface/esp_usb_bridge.cfg"},
};

// OpenOCD's target script for a chip, by the start of its name (lower
// case, no dashes; OpenOCD 0.12.0's tcl/target, Espressif's for the ESP32s).
constexpr std::pair<std::string_view, std::string_view> kTarget[] = {
    {"stm32f0", "target/stm32f0x.cfg"}, {"stm32f1", "target/stm32f1x.cfg"}, {"stm32f2", "target/stm32f2x.cfg"}, {"stm32f3", "target/stm32f3x.cfg"},
    {"stm32f4", "target/stm32f4x.cfg"}, {"stm32f7", "target/stm32f7x.cfg"}, {"stm32g0", "target/stm32g0x.cfg"}, {"stm32g4", "target/stm32g4x.cfg"},
    {"stm32h7", "target/stm32h7x.cfg"}, {"stm32l0", "target/stm32l0.cfg"},  {"stm32l1", "target/stm32l1.cfg"},  {"stm32l4", "target/stm32l4x.cfg"},
    {"stm32l5", "target/stm32l5x.cfg"}, {"stm32u5", "target/stm32u5x.cfg"}, {"stm32wb", "target/stm32wbx.cfg"}, {"stm32wl", "target/stm32wlx.cfg"},
    {"rp2040", "target/rp2040.cfg"},    {"nrf51", "target/nrf51.cfg"},      {"nrf52", "target/nrf52.cfg"},      {"atsamd", "target/at91samdXX.cfg"},
    {"atsame5", "target/atsame5x.cfg"}, {"esp32s3", "target/esp32s3.cfg"},  {"esp32s2", "target/esp32s2.cfg"},  {"esp32c3", "target/esp32c3.cfg"},
    {"esp32c6", "target/esp32c6.cfg"},  {"esp32h2", "target/esp32h2.cfg"},  {"esp32", "target/esp32.cfg"},
};

// openFPGALoader's cable for an FTDI chip, by its ids: Digilent's (its
// boards' and cables' product strings say so) and the plain chip's
// (openFPGALoader v1.2.0 src/cable.hpp lines 89-120).
struct FtdiCable {
    std::string_view usb, digilent, plain;
};
constexpr FtdiCable kFtdiCables[] = {
    {"0403:6010", "digilent", "ft2232"}, {"0403:6011", "digilent_ft4232", "ft4232"}, {"0403:6014", "digilent_hs2", "ft232"}, {"0403:6043", "", "ft4232hp"},
};

std::string lower(std::string_view s) {
    std::string out;
    for (const char c : s) out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

// A chip's name as the tables have it: "ESP32-S3" is "esp32s3".
std::string chip_key(std::string_view chip) {
    std::string out;
    for (const char c : lower(chip))
        if (c != '-' && c != '_' && c != ' ') out += c;
    return out;
}

template <size_t N>
std::string_view look_up(const std::pair<std::string_view, std::string_view> (&table)[N], std::string_view key, bool prefix = false) {
    for (const auto& [k, v] : table)
        if (prefix ? key.starts_with(k) : key == k) return v;
    return {};
}

// esptool v5's words: --flash_mode is --flash-mode, default_reset is default-reset.
std::string dashed(std::string_view s) {
    std::string out(s);
    std::ranges::replace(out, '_', '-');
    return out;
}

// A part of a command line with its ${names} put in (each value already as
// the shell or the file takes it); false when one of them has nothing.
using Vars = std::vector<std::pair<std::string_view, std::string>>;
bool expand(std::string_view part, const Vars& vars, std::string* out) {
    out->clear();
    size_t at = 0;
    for (;;) {
        const size_t open = part.find("${", at);
        if (open == std::string_view::npos) break;
        const size_t close = part.find('}', open);
        if (close == std::string_view::npos) break;
        *out += part.substr(at, open - at);
        const std::string_view name = part.substr(open + 2, close - open - 2);
        std::string value;
        for (const auto& [k, v] : vars)
            if (k == name) value = v;
        if (value.empty()) return false;
        *out += value;
        at = close + 1;
    }
    *out += part.substr(at);
    return true;
}

// A path as a Tcl word in OpenOCD's commands.
std::string tcl_word(std::string_view s) {
    return s.find_first_of(" \t{}[]$\"\\;") == std::string_view::npos ? std::string(s) : "{" + std::string(s) + "}";
}

bool is_esp(std::string_view chip) { return chip_key(chip).starts_with("esp32"); }

// A value as one word for the shell; nothing for nothing (its part is left out).
std::string word(const std::string& s) { return s.empty() ? std::string() : sh_word(s); }

// The board's probe as probe-rs's --probe takes it: VID:PID:SERIAL.
std::string probe_rs_probe(const Board& b) {
    if (b.probe_usb.size() != 9) return {};
    return b.probe.empty() ? b.probe_usb : b.probe_usb + ":" + b.probe;
}

// openFPGALoader's cable for the board's probe, when the board says none.
std::string ofl_cable_for(const Board& b) {
    if (!b.flash_cable.empty()) return b.flash_cable;
    if (b.probe_kind != "ftdi") return {};
    return ofl_ftdi_cable(b.probe_usb, b.probe_usb == "0403:6010");  // (an FT2232's: a Digilent board's, as the owner's Basys 3 is)
}

void mix(u64& h, std::string_view bytes) {
    for (const char c : bytes) {
        h ^= static_cast<u8>(c);
        h *= 0x100000001b3ull;
    }
}

void mix_u64(u64& h, u64 v) {
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<char>(v >> (8 * i));
    mix(h, std::string_view(b, 8));
}
}  // namespace

// ---------------------------------------------------------------- the tools

const FlashTool* flash_tool(std::string_view name) {
    for (const FlashTool& t : kTools)
        if (t.name == name) return &t;
    return nullptr;
}

std::string flash_tool_for(const Board& b) {
    if (!b.flash.empty()) return b.flash;
    if (const std::string_view t = look_up(kKindTool, b.probe_kind); !t.empty()) return std::string(t);
    if (b.probe.empty() && b.probe_kind.empty() && is_esp(b.chip)) return "esptool";
    return {};
}

std::vector<std::string> flash_programs(const Board& b, const std::string& tool) {
    std::vector<std::string> out;
    if (tool == "openocd" && (is_esp(b.chip) || b.probe_kind.starts_with("esp-"))) out.emplace_back("openocd-esp32");  // (Espressif's, with program_esp)
    if (const FlashTool* t = flash_tool(tool))
        for (const std::string_view p : t->programs)
            if (!p.empty()) out.emplace_back(p);
    return out;
}

bool esp_flasher_args(std::string_view text, const std::string& dir, FlashImage* out, std::string* why) {
    json::Doc doc;
    if (!doc.parse(std::string(text)) || !doc.root().is_object()) return *why = "flasher_args.json is not JSON", false;
    const json::Value root = doc.root();
    const json::Value files = root["flash_files"];
    if (!files.is_object() || files.size() == 0) return *why = "flasher_args.json has no flash_files", false;
    FlashImage img;
    for (const json::Value f : files) {
        if (!f.is_string() || f.str().empty()) continue;
        img.parts.push_back({std::string(f.key()), (dir.empty() ? std::string() : dir + "/") + std::string(f.str())});
    }
    std::ranges::stable_sort(img.parts, {}, [](const FlashImage::Part& p) { return std::strtoull(p.address.c_str(), nullptr, 0); });
    for (const json::Value a : root["write_flash_args"])
        if (a.is_string()) img.options.push_back(a.str().starts_with("--") ? dashed(a.str()) : std::string(a.str()));
    const json::Value extra = root["extra_esptool_args"];
    img.chip = std::string(extra["chip"].str());
    img.before = dashed(extra["before"].str());
    img.after = dashed(extra["after"].str());
    img.no_stub = extra["stub"].exists() && !extra["stub"].boolean(true);
    *out = std::move(img);
    return true;
}

bool elf_loads(std::string_view b, std::vector<std::pair<u64, std::string_view>>* out) {
    out->clear();
    if (b.size() < 52 || b.substr(0, 4) != std::string_view("\x7f" "ELF", 4) || (b[4] != 1 && b[4] != 2) || (b[5] != 1 && b[5] != 2)) return false;
    const bool wide = b[4] == 2, little = b[5] == 1;
    bool ok = true;
    auto rd = [&](u64 off, int n) -> u64 {
        if (off + static_cast<u64>(n) > b.size()) return ok = false, 0;
        u64 v = 0;
        for (int i = 0; i < n; ++i) {
            const u64 byte = static_cast<u8>(b[off + static_cast<u64>(little ? i : n - 1 - i)]);
            v |= byte << (8 * i);
        }
        return v;
    };
    const u64 phoff = wide ? rd(32, 8) : rd(28, 4);
    const u64 entsize = wide ? rd(54, 2) : rd(42, 2);
    const u64 count = wide ? rd(56, 2) : rd(44, 2);
    if (!ok || entsize < (wide ? 56u : 32u)) return false;
    for (u64 i = 0; i < count; ++i) {
        const u64 p = phoff + i * entsize;
        if (rd(p, 4) != 1) continue;  // (PT_LOAD)
        const u64 offset = wide ? rd(p + 8, 8) : rd(p + 4, 4);
        const u64 paddr = wide ? rd(p + 24, 8) : rd(p + 12, 4);
        const u64 filesz = wide ? rd(p + 32, 8) : rd(p + 16, 4);
        if (!ok || offset > b.size() || filesz > b.size() - offset) return false;  // (cut short)
        if (filesz) out->emplace_back(paddr, b.substr(offset, filesz));
    }
    return ok;
}

u64 flash_hash(std::string_view bytes) {
    u64 h = 0xcbf29ce484222325ull;  // (FNV-1a)
    std::vector<std::pair<u64, std::string_view>> loads;
    if (elf_loads(bytes, &loads)) {
        mix(h, "elf");
        for (const auto& [addr, data] : loads) {
            mix_u64(h, addr);
            mix_u64(h, data.size());
            mix(h, data);
        }
        return h;
    }
    mix(h, bytes);
    return h;
}

// ---------------------------------------------------------------- a flash's command

FlashPlan flash_plan(const Board& b, const std::string& tool_name, const std::string& program, const FlashImage& image, FlashMode mode) {
    FlashPlan plan;
    plan.tool = tool_name;
    const FlashTool* tool = flash_tool(tool_name);
    if (!tool) return plan.why = "No flash tool called " + tool_name, plan;
    plan.kind = std::string(tool->kind);
    plan.transport = tool->transport;
    const bool fpga = tool->kind == "program";
    if (fpga && mode == FlashMode::kFlash) mode = FlashMode::kSram;
    if (!fpga && mode != FlashMode::kFlash) return plan.why = "The board is not an FPGA's (its flash tool is " + tool_name + ")", plan;
    if (image.parts.empty()) return plan.why = "Nothing to flash", plan;
    const FlashImage::Part& first = image.parts.front();
    const std::string chip = !b.chip.empty() ? b.chip : image.chip;
    Vars vars;
    std::vector<std::string> script;
    std::string same = tool_name + "|" + b.probe_usb + ":" + b.probe;
    if (tool_name == "esptool") {
        std::string files;
        for (const FlashImage::Part& p : image.parts) {
            if (p.address.empty()) return plan.why = "esptool writes .bin files at their addresses: " + p.path + " has none (flash.address, or ESP-IDF's flasher_args.json)", plan;
            files += (files.empty() ? "" : " ") + p.address + " " + sh_word(p.path);
        }
        std::string options;
        for (const std::string& o : image.options) options += (options.empty() ? "" : " ") + sh_word(o);
        vars = {{"chip", word(chip_key(chip))}, {"port", word(b.port)},   {"before", word(image.before)}, {"after", word(image.after)},
                {"stub", image.no_stub ? "--no-stub" : ""}, {"options", options}, {"files", files}};
        same += "|" + chip_key(chip) + "|" + b.port;
    } else if (tool_name == "openocd") {
        const std::string iface = !b.flash_interface.empty() ? b.flash_interface : std::string(look_up(kInterface, b.probe_kind));
        if (iface.empty()) return plan.why = "OpenOCD needs the probe's interface: flash.interface, or a probe of a kind it knows", plan;
        const std::string target = !b.flash_target.empty() ? b.flash_target : std::string(look_up(kTarget, chip_key(chip), true));
        if (target.empty()) return plan.why = chip.empty() ? "OpenOCD needs the board's chip, or flash.target" : "OpenOCD has no target script blit knows for " + chip + ": flash.target", plan;
        std::string commands;
        const bool esp = is_esp(chip) || b.probe_kind.starts_with("esp-");
        for (size_t i = 0; i < image.parts.size(); ++i) {
            const FlashImage::Part& p = image.parts[i];
            const bool last = i + 1 == image.parts.size();
            std::string c;
            if (esp) {  // (Espressif's: each file at its address, the last then resetting and leaving)
                c = "program_esp " + tcl_word(p.path) + (p.address.empty() ? "" : " " + p.address) + " verify" + (last ? " reset exit" : "");
            } else {
                if (!last) return plan.why = "OpenOCD's program writes one file", plan;
                c = "program " + tcl_word(p.path) + (p.address.empty() ? "" : " " + p.address) + " verify reset exit";
            }
            commands += (commands.empty() ? "-c " : " -c ") + sh_word(c);
        }
        vars = {{"interface", word(iface)}, {"adapter", b.probe.empty() ? std::string() : word("adapter serial " + b.probe)}, {"target", word(target)}, {"program", commands}};
        same += "|" + iface + "|" + target;
    } else if (tool_name == "probe-rs") {
        if (image.parts.size() > 1) return plan.why = "probe-rs downloads one file", plan;
        const bool bin = !first.address.empty();
        vars = {{"chip", word(chip)}, {"probe", word(probe_rs_probe(b))}, {"format", bin ? "bin" : ""}, {"address", bin ? word(first.address) : std::string()}, {"image", word(first.path)}};
        same += "|" + chip;
    } else if (tool_name == "jlink") {
        if (image.parts.size() > 1) return plan.why = "J-Link Commander loads one file", plan;
        const std::string device = !b.flash_device.empty() ? b.flash_device : chip;
        if (device.empty()) return plan.why = "J-Link Commander needs the board's chip (its device), or flash.device", plan;
        const Vars lines = {{"device", device}, {"si", "SWD"}, {"speed", "4000"}, {"image", first.path + (first.address.empty() ? "" : " " + first.address)}};
        std::string line;
        for (const std::string_view l : kJLinkScript)
            if (expand(l, lines, &line)) script.push_back(line);
        vars = {{"serial", word(b.probe)}, {"script", "\"$f\""}};
        same += "|" + device;
    } else if (tool_name == "vivado") {
        if (image.parts.size() > 1) return plan.why = "Vivado loads one bitstream", plan;
        const std::string part = !b.flash_device.empty() ? b.flash_device : chip;
        const bool into_flash = mode == FlashMode::kFpgaFlash;
        const std::string cfgmem = into_flash ? vivado_cfgmem(b) : std::string();
        if (into_flash && cfgmem.empty()) return plan.why = "Vivado needs the board's flash part to write its flash: flash.cfgmem, as get_cfgmem_parts names it", plan;
        const Vars lines = {{"target", b.probe.empty() ? "*" : tcl_word("*/" + b.probe + "*")},
                            {"chip", tcl_word(lower(part))},
                            {"image", tcl_word(first.path)},
                            {"cfgmem", tcl_word(cfgmem)},
                            {"width", std::to_string(image.spi_width)}};
        std::string line;
        for (const VivadoLine& l : kVivadoScript)
            if (l.when == kBoth || (l.when == kIntoFlash) == into_flash)
                if (expand(l.text, lines, &line)) script.push_back(line);
        vars = {{"script", "\"$f\""}};
        same += "|" + lower(part) + (into_flash ? "|" + cfgmem : std::string());
    } else if (fpga) {
        if (image.parts.size() > 1) return plan.why = "openFPGALoader loads one bitstream", plan;
        const std::string cable = b.flash_board.empty() ? ofl_cable_for(b) : std::string();
        if (b.flash_board.empty() && cable.empty()) return plan.why = "openFPGALoader needs the board (flash.board) or its cable (flash.cable)", plan;
        vars = {{"board", word(b.flash_board)}, {"cable", word(cable)}, {"serial", word(b.probe)}, {"write", mode == FlashMode::kFpgaFlash ? "-f --verify" : ""}, {"image", word(first.path)}};
        same += "|" + b.flash_board + "|" + cable;
    }
    same += mode == FlashMode::kFpgaFlash ? "|flash" : mode == FlashMode::kSram ? "|sram" : "";
    for (const FlashImage::Part& p : image.parts) same += "|" + p.address + " " + p.path;
    plan.same = std::move(same);
    // The command line: the program by its path, then the parts that have what they name.
    std::string cmd = sh_word(program), part;
    for (const std::string_view p : tool->parts)
        if (!p.empty() && expand(p, vars, &part) && !part.empty()) cmd += " " + part;
    if (!script.empty()) {  // (J-Link's command file, Vivado's Tcl, made for the run and gone after, with what was written beside it: Vivado's .mcs and .prm)
        std::string lines;
        for (const std::string& l : script) lines += " " + sh_word(l);
        // (A subshell: its exit ends it alone, and the pipe's redirections below are all of its.)
        cmd = "(f=$(mktemp) && printf '%s\\n'" + lines + " >\"$f\" && " + cmd + "; s=$?; rm -f \"$f\" \"$f.mcs\" \"$f.prm\"; exit $s)";
    }
    if (plan.transport == kPipe)  // (its output and errors through a pipe, and its exit status kept)
        cmd = "exec 3>&1; s=$( { { " + cmd + " 2>&1 3>&- 4>&-; echo $? >&4; } | cat >&3; } 4>&1 ); exit $s";
    plan.command = std::move(cmd);
    return plan;
}

int bitstream_spi_width(std::string_view b) {
    // (Big-endian words after the sync word 0xAA995566: a type 1 write of
    // one word to register 0x1F is 0x3003E001, the word itself next.
    // Vivado 2026.2 wrote 0x13B for SPI_BUSWIDTH 2 and 0x26B for 4, the
    // read command after it, and none for 1; looked for before the frames.)
    const size_t sync = b.substr(0, 4096).find(std::string_view("\xAA\x99\x55\x66", 4));
    if (sync == std::string_view::npos) return 1;
    auto word = [&](size_t at) {
        u32 v = 0;
        for (size_t i = 0; i < 4; ++i) v = v << 8 | static_cast<u8>(b[at + i]);
        return v;
    };
    for (size_t at = sync + 4, n = 0; at + 8 <= b.size() && n < 64; at += 4, ++n) {
        if (word(at) != 0x3003E001u) continue;
        const u32 width = word(at + 4) >> 8 & 3;
        return width == 1 ? 2 : width == 2 ? 4 : 1;
    }
    return 1;
}

std::string vivado_cfgmem(const Board& b) {
    if (!b.flash_cfgmem.empty()) return b.flash_cfgmem;
    return std::string(look_up(kCfgmem, b.flash_board));
}

std::string vivado_installed(const std::string& there) {
    std::vector<std::string> homes;
    if (const char* e = std::getenv("BLIT_VIVADO_HOMES"); e && *e) {
        for (std::string_view l = e; !l.empty();) {
            const size_t colon = l.find(':');
            if (!l.substr(0, colon).empty()) homes.emplace_back(l.substr(0, colon));
            l = colon == std::string_view::npos ? std::string_view{} : l.substr(colon + 1);
        }
    } else {
        for (const std::string_view h : kVivadoHomes) homes.emplace_back(h);
    }
    // (A version's name: "2026.2", compared as numbers.)
    auto version = [](const std::string& n) {
        char* end = nullptr;
        const double v = std::strtod(n.c_str(), &end);
        return !n.empty() && std::isdigit(static_cast<unsigned char>(n[0])) && end && *end == '\0' ? v : -1.0;
    };
    std::string best;
    double best_v = -1;
    auto consider = [&](const std::string& path, double v) {
        if (v > best_v && fs_kind(there + path) == 1) best = path, best_v = v;
    };
    for (const std::string& h : homes) {
        for (const FsEntry& e : fs_list(there + h)) {
            if (!e.dir) continue;
            if (const double v = version(e.name); v >= 0) consider(h + "/" + e.name + "/Vivado/bin/vivado", v);
            if (e.name == "Vivado")
                for (const FsEntry& w : fs_list(there + h + "/Vivado"))
                    if (const double v = version(w.name); w.dir && v >= 0) consider(h + "/Vivado/" + w.name + "/bin/vivado", v);
        }
    }
    return best;
}

std::string ofl_ftdi_cable(std::string_view usb, bool digilent) {
    for (const FtdiCable& c : kFtdiCables)
        if (usb == c.usb) return std::string(digilent && !c.digilent.empty() ? c.digilent : c.plain);
    return {};
}

}  // namespace blit
