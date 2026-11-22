#include "flash.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

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
};

// J-Link Commander's command file, a line each (SEGGER's wiki, "J-Link
// Commander": Device, SelectInterface, Speed, Connect, LoadFile, Reset, Go,
// Exit).
constexpr std::string_view kJLinkScript[] = {"device ${device}", "si ${si}", "speed ${speed}", "connect", "loadfile ${image}", "r", "g", "exit"};

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
    const bool fpga = tool_name == "openfpgaloader";
    if (fpga && mode == FlashMode::kFlash) mode = FlashMode::kSram;
    if (!fpga && mode != FlashMode::kFlash) return plan.why = "The board is not an FPGA's (its flash tool is " + tool_name + ", not openFPGALoader)", plan;
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
    if (!script.empty()) {  // (J-Link's command file, made for the run and gone after)
        std::string lines;
        for (const std::string& l : script) lines += " " + sh_word(l);
        cmd = "f=$(mktemp) && printf '%s\\n'" + lines + " >\"$f\" && " + cmd + "; s=$?; rm -f \"$f\"; exit $s";
    }
    if (plan.transport == kPipe)  // (its output and errors through a pipe, and its exit status kept)
        cmd = "exec 3>&1; s=$( { { " + cmd + " 2>&1 3>&- 4>&-; echo $? >&4; } | cat >&3; } 4>&1 ); exit $s";
    plan.command = std::move(cmd);
    return plan;
}

std::string ofl_ftdi_cable(std::string_view usb, bool digilent) {
    for (const FtdiCable& c : kFtdiCables)
        if (usb == c.usb) return std::string(digilent && !c.digilent.empty() ? c.digilent : c.plain);
    return {};
}

}  // namespace blit
