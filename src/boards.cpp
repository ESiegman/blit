#include "boards.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "engine.hpp"
#include "flash.hpp"
#include "missing.hpp"

namespace blit {

namespace {

// The cables of other makers --detect is asked through (openFPGALoader
// v1.2.0 src/cable.hpp): not the debug probes for MCUs it knows too
// (cmsisdap, jlink, esp32s3), which the probe picker has.
constexpr std::string_view kJtagCables[] = {"dirtyJtag",       "usb-blaster",     "usb-blaster_1",          "usb-blaster_2",
                                            "usb-blasterII",   "usb-blasterII_1", "xilinxPlatformCableUsb", "xilinxPlatformCableUsb_alt",
                                            "ch347_jtag",      "anlogicCable",    "gwu2x"};

// The boards openFPGALoader names on Digilent's cables, through the
// chip's first channel as --detect asks (v1.2.0 src/board.hpp lines
// 113-200: each JTAG_BOARD's name, FPGA part and cable), with the names
// their makers give them. A board is matched on the start of its part, as
// --detect names the FPGA (src/part.hpp: "xc7a35" for 0x0362D093); of
// boards with the same FPGA, the first here is the first row.
struct FpgaBoard {
    std::string_view name, title, part, usb;
};
constexpr FpgaBoard kBoards[] = {
    {"basys3", "Basys 3", "xc7a35tcpg236", "0403:6010"},
    {"arty_a7_35t", "Arty A7-35T", "xc7a35tcsg324", "0403:6010"},
    {"cmoda7_35t", "Cmod A7-35T", "xc7a35tcpg236", "0403:6010"},
    {"cmoda7_15t", "Cmod A7-15T", "xc7a15tcpg236", "0403:6010"},
    {"nexys_a7_50", "Nexys A7-50T", "xc7a50tcsg324", "0403:6010"},
    {"arty_a7_100t", "Arty A7-100T", "xc7a100tcsg324", "0403:6010"},
    {"nexys_a7_100", "Nexys A7-100T", "xc7a100tcsg324", "0403:6010"},
    {"arty_s7_25", "Arty S7-25", "xc7s25csga324", "0403:6010"},
    {"arty_s7_50", "Arty S7-50", "xc7s50csga324", "0403:6010"},
    {"cmod_s7", "Cmod S7", "xc7s25csga225", "0403:6010"},
    {"arty_z7_10", "Arty Z7-10", "xc7z010clg400", "0403:6010"},
    {"zybo_z7_10", "Zybo Z7-10", "xc7z010clg400", "0403:6010"},
    {"arty_z7_20", "Arty Z7-20", "xc7z020clg400", "0403:6010"},
    {"zybo_z7_20", "Zybo Z7-20", "xc7z020clg400", "0403:6010"},
    {"zedboard", "ZedBoard", "xc7z020clg484", "0403:6014"},
};

std::string lower(std::string_view s) {
    std::string out;
    for (const char c : s) out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return std::string(s);
}

// "0x0403:0x6010" as the probe picker writes ids: "0403:6010".
std::string usb_ids(std::string_view s) {
    std::string out = lower(s);
    for (size_t at; (at = out.find("0x")) != std::string::npos;) out.erase(at, 2);
    return out;
}

}  // namespace

std::vector<OflCable> ofl_scan(std::string_view out) {
    // (Its columns as its header has them: each field is padded to the
    // longest of its column, src/libusb_ll.cpp lines 250-272.)
    std::vector<OflCable> cables;
    size_t col[7] = {};
    bool head = false;
    while (!out.empty()) {
        const size_t nl = out.find('\n');
        std::string_view line = out.substr(0, nl);
        out = nl == std::string_view::npos ? std::string_view{} : out.substr(nl + 1);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("Bus ")) {
            static constexpr std::string_view kNames[] = {"Bus", "device", "vid:pid", "probe_type", "manufacturer", "serial", "product"};
            head = true;
            for (int i = 0; i < 7; ++i) {
                const size_t at = line.find(kNames[i], i ? col[i - 1] + 1 : 0);
                if (at == std::string_view::npos) head = false;
                else col[i] = at;
            }
            continue;
        }
        if (!head || line.size() <= col[6] || line.substr(col[2], 2) != "0x") continue;
        auto field = [&](int i) { return trim(line.substr(col[i], i < 6 ? col[i + 1] - col[i] : std::string_view::npos)); };
        OflCable c{field(0), field(1), usb_ids(field(2)), field(3), field(4), field(5), field(6)};
        if (c.usb.size() == 9) cables.push_back(std::move(c));
    }
    return cables;
}

std::vector<OflChip> ofl_detect(std::string_view out) {
    // (src/main.cpp lines 448-469: "index N:", then the FPGA's idcode,
    // manufacturer, family and model, a tab before each.)
    std::vector<OflChip> chips;
    while (!out.empty()) {
        const size_t nl = out.find('\n');
        const std::string line = trim(out.substr(0, nl));
        out = nl == std::string_view::npos ? std::string_view{} : out.substr(nl + 1);
        if (line.starts_with("index ") && line.ends_with(":")) {
            chips.emplace_back();
            continue;
        }
        if (chips.empty()) continue;
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp), value = trim(line.substr(sp));
        OflChip& c = chips.back();
        if (key == "idcode") c.idcode = value;
        else if (key == "manufacturer") c.manufacturer = value;
        else if (key == "family") c.family = value;
        else if (key == "model") c.model = value;
    }
    std::erase_if(chips, [](const OflChip& c) { return c.model.empty(); });  // (a device on the chain that is not an FPGA)
    return chips;
}

namespace {

// The cable openFPGALoader takes for a scanned one ("" none it is asked through).
std::string ofl_cable_name(const OflCable& c) {
    if (c.usb.starts_with("0403:")) return ofl_ftdi_cable(c.usb, lower(c.manufacturer).find("digilent") != std::string::npos || lower(c.product).find("digilent") != std::string::npos);
    if (std::ranges::find(kJtagCables, c.probe_type) != std::end(kJtagCables)) return c.probe_type;
    return {};
}

bool has_serial(const OflCable& c) { return !c.serial.empty() && c.serial != "none"; }

}  // namespace

std::vector<std::string> ofl_detect_args(const OflCable& c) {
    const std::string cable = ofl_cable_name(c);
    if (cable.empty()) return {};
    std::vector<std::string> args = {"--detect", "-c", cable};
    if (has_serial(c) && c.usb.starts_with("0403:")) args.insert(args.end(), {"--usb-serial-num", c.serial});
    else args.insert(args.end(), {"--busdev-num", std::to_string(std::atoi(c.bus.c_str())) + ":" + std::to_string(std::atoi(c.device.c_str()))});
    return args;
}

std::vector<ProbeRow> ofl_rows(const std::vector<OflCable>& cables, const std::vector<std::vector<OflChip>>& chips, const std::vector<ProbeRow>& native,
                               const std::string& host) {
    std::vector<ProbeRow> out;
    for (size_t i = 0; i < cables.size(); ++i) {
        const OflCable& c = cables[i];
        const std::string cable = ofl_cable_name(c);
        const ProbeRow* same = nullptr;
        for (const ProbeRow& n : native)
            if (n.usb == c.usb && (!has_serial(c) || n.serial == c.serial)) same = &n;
        ProbeRow base;
        base.role = ProbeRole::kFpga;
        base.kind = same ? same->kind : c.usb.starts_with("0403:") ? "ftdi" : cable;
        base.serial = has_serial(c) ? c.serial : std::string();
        base.usb = c.usb;
        base.port = same ? same->port : std::string();
        base.host = host;
        bool any = false;
        for (const OflChip& chip : i < chips.size() ? chips[i] : std::vector<OflChip>{}) {
            bool named = false;
            for (const FpgaBoard& b : kBoards)
                if (b.usb == c.usb && cable.starts_with("digilent") && b.part.starts_with(chip.model)) {
                    ProbeRow r = base;
                    // (The part to its size, without the package: "xc7a35tcpg236" is an xc7a35t, "xc7z010clg400" an xc7z010.)
                    size_t n = 0;
                    auto skip = [&](bool digits) {
                        while (n < b.part.size() && (b.part[n] >= '0' && b.part[n] <= '9') == digits) ++n;
                    };
                    skip(false), skip(true), skip(false), skip(true);
                    if (n < b.part.size() && b.part[n] == 't') ++n;
                    r.name = std::string(b.title) + " (" + std::string(b.part.substr(0, n)) + ")";
                    r.fpga_board = std::string(b.name);
                    out.push_back(std::move(r));
                    named = any = true;
                }
            if (!named && !cable.empty()) {
                ProbeRow r = base;
                r.name = chip.model + " on " + (same ? same->name : c.probe_type);
                r.fpga_cable = cable;
                out.push_back(std::move(r));
                any = true;
            }
        }
        if (!any && !same && !cable.empty()) {  // (a cable the picker does not know: openFPGALoader's name for it)
            ProbeRow r = base;
            r.name = c.probe_type + (c.product.empty() || c.product == "none" ? std::string() : " (" + c.product + ")");
            r.fpga_cable = cable;
            out.push_back(std::move(r));
        }
    }
    for (ProbeRow& r : out) {
        r.text = r.name;
        if (!r.serial.empty()) r.text += "  " + r.serial;
        if (!r.port.empty()) r.text += "  " + r.port;
        if (!r.host.empty()) r.text += "  on " + r.host;
    }
    return out;
}

std::vector<ProbeRow> ofl_merge(std::vector<ProbeRow> rows, const std::vector<ProbeRow>& boards) {
    size_t front = 0;  // (those on no row of the picker's: first, in the order found)
    for (const ProbeRow& b : boards) {
        size_t at = std::string::npos;
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].usb == b.usb && rows[i].serial == b.serial) at = i;  // (the last of that device's: its own row, then its boards')
        if (at == std::string::npos) rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(front++), b);
        else rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(at + 1), b);
    }
    return rows;
}

namespace {

// What a program printed, run headless on a machine (a worker's call).
bool capture(const std::string& host, std::vector<std::string> argv, u32 ms, std::string* out) {
    std::mutex mu;
    std::string text;
    EngineExit ended;
    EngineSpec spec;
    spec.host = host;
    spec.argv = std::move(argv);
    spec.timeout_ms = ms;
    std::shared_ptr<Engine> e = Engine::run(
        std::move(spec),
        [&](std::string_view line, Engine::Stream) {
            std::lock_guard lock(mu);
            text.append(line).push_back('\n');
        },
        [&](const EngineExit& x) {
            std::lock_guard lock(mu);
            ended = x;
        });
    e->wait(static_cast<int>(ms) + 5000);
    e.reset();  // (its thread joined: nothing it calls is left)
    *out = std::move(text);
    return ended.started && ended.code == 0;
}

}  // namespace

std::vector<ProbeRow> fpga_boards(const std::string& host, const std::vector<ProbeRow>& native, u32 ms, std::string* missing) {
    const std::string ofl = Engine::which(host, "openFPGALoader");
    if (ofl.empty()) {
        if (missing && std::ranges::any_of(native, [](const ProbeRow& r) { return r.role == ProbeRole::kFpga; }))
            *missing = not_installed("openFPGALoader", host) + ": the boards on its cables are not named";
        return {};
    }
    std::string out;
    capture(host, {ofl, "--scan-usb"}, ms, &out);
    const std::vector<OflCable> cables = ofl_scan(out);
    std::vector<std::vector<OflChip>> chips(cables.size());
    for (size_t i = 0; i < cables.size(); ++i) {
        std::vector<std::string> args = ofl_detect_args(cables[i]);
        if (args.empty()) continue;
        args.insert(args.begin(), ofl);
        std::string found;
        capture(host, std::move(args), ms, &found);  // (no board on the cable: an error, and no FPGA)
        chips[i] = ofl_detect(found);
    }
    return ofl_rows(cables, chips, native, host);
}

}  // namespace blit
