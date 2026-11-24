#include "probes.hpp"

#include "build.hpp"
#include "fsio.hpp"
#include "remote.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace blit {

namespace {

using enum ProbeRole;

// What each device is, by its ids: the first entry that matches. Each id
// is from a tool's own table or the vendor's firmware, named beside it
// (the tag and file; the line where it helps). Debug probes, then FPGA
// programmers, boot ROMs and serial bridges.
const ProbeKind kKinds[] = {
    // ST-LINK, 0483: probe-rs 0.32.0 probe-rs/src/probe/stlink/usb_interface.rs
    // USB_PID_EP_MAP (lines 22-31), and OpenOCD 0.12.0
    // src/jtag/drivers/stlink_usb.c (lines 78-86) and tcl/interface/stlink.cfg
    // (line 11, hla_vid_pid). Nucleo and Discovery boards carry a V2-1 or a
    // V3E, whose product string is the same as a stand-alone one's.
    {0x0483, 0x3744, 0xffff, "", "stlink", "ST-LINK V1", kProbe},
    {0x0483, 0x3748, 0xffff, "", "stlink", "ST-LINK V2", kProbe},
    {0x0483, 0x374b, 0xffff, "", "stlink", "ST-LINK V2-1", kProbe},
    {0x0483, 0x374a, 0xffff, "", "stlink", "ST-LINK V2-1", kProbe},  // (with audio, probe-rs)
    {0x0483, 0x3742, 0xffff, "", "stlink", "ST-LINK V2-1", kProbe},  // (no mass storage, probe-rs)
    {0x0483, 0x3752, 0xffff, "", "stlink", "ST-LINK V2-1", kProbe},  // (no mass storage, OpenOCD)
    {0x0483, 0x374e, 0xffff, "", "stlink", "ST-LINK V3E", kProbe},
    {0x0483, 0x374f, 0xffff, "", "stlink", "ST-LINK V3SET", kProbe},  // (OpenOCD's V3S; probe-rs's V3 with the bridge)
    {0x0483, 0x3753, 0xffff, "", "stlink", "ST-LINK V3", kProbe},     // (two virtual COM ports)
    {0x0483, 0x3754, 0xffff, "", "stlink", "ST-LINK V3E", kProbe},    // (no mass storage)
    {0x0483, 0x3757, 0xffff, "", "stlink", "STLINK-V3PWR", kProbe},   // (probe-rs)
    {0x0483, 0x374d, 0xffff, "", "stlink", "ST-LINK V3 boot loader", kBoot},  // (OpenOCD's STLINK_V3_USBLOADER_PID)
    // SEGGER J-Link, 1366: probe-rs 0.32.0 probe-rs/src/probe/jlink/mod.rs
    // (lines 1425-1452, from SEGGER's udev rules): the old ids 0101 to 0108
    // but 0106 (a serial port alone), and the new ones with bit 0x1000 and
    // bit 0x10 or 0x20 (a J-Link set to be a CMSIS-DAP, 1008, is neither:
    // its product string names it). openFPGALoader v1.2.0's --list-cables
    // has jlink_base 1366:0101, jlink 1366:0105 and jtrace_pro 1366:1020.
    {0x1366, 0x0101, 0xffff, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x0102, 0xffff, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x0103, 0xffff, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x0104, 0xffff, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x0105, 0xffff, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x0107, 0xffff, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x0108, 0xffff, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x1010, 0x1010, "", "jlink", "J-Link", kProbe},
    {0x1366, 0x1020, 0x1020, "", "jlink", "J-Link", kProbe},
    // The Raspberry Pi Debug Probe (a CMSIS-DAP): debugprobe-v2.3.1
    // src/usb_descriptors.c (line 50, 2e8a:000c; line 137, "Raspberry Pi");
    // the older Picoprobe, picotool 2.3.1
    // picoboot_connection/picoboot_connection.h (PRODUCT_ID_PICOPROBE).
    {0x2e8a, 0x000c, 0xffff, "", "cmsis-dap", "Raspberry Pi Debug Probe", kProbe},
    {0x2e8a, 0x0004, 0xffff, "", "cmsis-dap", "Picoprobe", kProbe},
    // Espressif's USB-Serial/JTAG, built into the ESP32-S3, C3, C6 and H2:
    // Espressif's OpenOCD v0.12.0-esp32-20260831
    // share/openocd/scripts/interface/esp_usb_jtag.cfg (line 8, adapter usb
    // vid_pid 0x303a 0x1001); openFPGALoader v1.2.0's --list-cables, esp32s3
    // 0x303a:1001. Its USB bridge firmware (on ESP-Prog-2 and others):
    // OpenOCD 0.12.0 tcl/interface/esp_usb_bridge.cfg (line 8, 303a:1002).
    {0x303a, 0x1001, 0xffff, "", "esp-usb-jtag", "Espressif USB-Serial/JTAG", kProbe},
    {0x303a, 0x1002, 0xffff, "", "esp-usb-bridge", "Espressif USB Bridge", kProbe},
    // Black Magic Probe: blackmagic v2.0.0 src/platforms/common/usb_descriptors.h
    // (lines 51-52, 1d50:6018; lines 423-427, "Black Magic Debug", "Black
    // Magic GDB Server", "Black Magic UART Port"); probe-rs 0.32.0
    // probe-rs/src/probe/blackmagic/mod.rs (lines 35-36).
    {0x1d50, 0x6018, 0xffff, "", "blackmagic", "Black Magic Probe", kProbe, "UART", "GDB"},
    // FTDI's chips, 0403: Linux v6.12 drivers/usb/serial/ftdi_sio_ids.h
    // (lines 22-27). Digilent's boards and cables say so in their product
    // string ("Digilent USB Device", "Digilent Adept USB Device": OpenOCD
    // 0.12.0 tcl/interface/ftdi/digilent_jtag_hs3.cfg line 9,
    // digilent-hs1.cfg line 7). openFPGALoader v1.2.0's --list-cables:
    // digilent 0x0403:6010 (interface A for JTAG; --list-boards gives
    // basys3 that cable), digilent_hs2 and digilent_hs3 0x0403:6014,
    // digilent_ft4232 0x0403:6011. The FT2232's channel B is the board's
    // UART, the last of its ports.
    {0x0403, 0x6010, 0xffff, "Digilent", "ftdi", "Digilent FT2232 (FPGA)", kFpga, "last"},
    {0x0403, 0x6011, 0xffff, "Digilent", "ftdi", "Digilent FT4232 (FPGA)", kFpga, "last"},
    {0x0403, 0x6014, 0xffff, "Digilent", "ftdi", "Digilent JTAG-HS2/HS3 (FPGA)", kFpga},
    {0x0403, 0x6010, 0xffff, "", "ftdi", "FTDI FT2232", kProbe, "last"},
    {0x0403, 0x6011, 0xffff, "", "ftdi", "FTDI FT4232", kProbe, "last"},
    {0x0403, 0x6014, 0xffff, "", "ftdi", "FTDI FT232H", kProbe},
    {0x0403, 0x6001, 0xffff, "", "ftdi-serial", "FTDI FT232R serial bridge", kSerial},
    {0x0403, 0x6015, 0xffff, "", "ftdi-serial", "FTDI FT-X serial bridge", kSerial},
    // The RP2040's and RP2350's boot ROMs (BOOTSEL), and the Pico SDK's
    // USB serial: picotool 2.3.1 picoboot_connection/picoboot_connection.h
    // (lines 19-25). Both are flashed with picotool (flash.cpp), which
    // finds the board by the SDK's serial number, its flash's unique ID.
    // (Seen on the owner's Pico through IOKit, 2026-11-23: 2e8a:000a
    // "Pico" A58565170D696C53 with /dev/cu.usbmodem1101 running blinky;
    // 2e8a:0003 "RP2 Boot" E0C9125B0D9B, no port, in BOOTSEL.)
    {0x2e8a, 0x0003, 0xffff, "", "rp-boot", "RP2040 boot ROM", kBoot},
    {0x2e8a, 0x000f, 0xffff, "", "rp-boot", "RP2350 boot ROM", kBoot},
    {0x2e8a, 0x000a, 0xffff, "", "pico-serial", "Pico SDK USB serial (RP2040)", kSerial},
    {0x2e8a, 0x0009, 0xffff, "", "pico-serial", "Pico SDK USB serial", kSerial},
    // STM32's DfuSe boot ROM (system memory, BOOT0 high): dfu-util 0.11
    // doc/60-dfuse.rules (line 4). Flashed with dfu-util.
    {0x0483, 0xdf11, 0xffff, "", "dfu", "STM32 DFU boot ROM", kBoot},
    // Silicon Labs CP210x: Linux v6.12 drivers/usb/serial/cp210x.c (lines
    // 170-174: ea60 the CP2102, CP2104 and CP2102N, ea70 the CP2105, ea71
    // the CP2108).
    {0x10c4, 0xea60, 0xffff, "", "cp210x", "CP210x serial bridge", kSerial},
    {0x10c4, 0xea70, 0xffff, "", "cp210x", "CP2105 serial bridge", kSerial},
    {0x10c4, 0xea71, 0xffff, "", "cp210x", "CP2108 serial bridge", kSerial},
    // WCH: Linux v6.12 drivers/usb/serial/ch341.c (lines 84-86: 5523,
    // 7522, 7523); the CH343 and CH9102, WCH's own ch343ser_linux driver
    // (commit b5515b9) driver/ch343.c (lines 2432, 2452).
    {0x1a86, 0x7523, 0xffff, "", "ch340", "CH340 serial bridge", kSerial},
    {0x1a86, 0x7522, 0xffff, "", "ch340", "CH340 serial bridge", kSerial},
    {0x1a86, 0x5523, 0xffff, "", "ch340", "CH341 serial bridge", kSerial},
    {0x1a86, 0x55d3, 0xffff, "", "ch340", "CH343 serial bridge", kSerial},
    {0x1a86, 0x55d4, 0xffff, "", "ch340", "CH9102 serial bridge", kSerial},
};

bool has(std::string_view s, std::string_view part) { return !part.empty() && s.find(part) != std::string_view::npos; }

// probe-rs 0.32.0 probe-rs/src/probe/stlink/tools.rs read_serial_number:
// an ST-LINK V2's serial number is 12 bytes of binary, shorter than the 24
// characters of a later one's, and is written as its bytes in hex (as
// probe-rs's --probe takes it).
std::string stlink_serial(const std::string& s) {
    if (s.size() >= 24) return s;
    std::string out;
    char b[4];
    for (const char c : s) {
        std::snprintf(b, sizeof b, "%02X", static_cast<unsigned char>(c));
        out += b;
    }
    return out;
}

// Which port of a device the rule picks: "" the first, "last" the last,
// else the first whose interface's name has it in (skipping `not`).
const UsbPort* pick_port(const UsbDevice& d, const char* rule, const UsbPort* not_this = nullptr) {
    if (d.ports.empty() || !rule) return nullptr;
    const std::string_view r = rule;
    if (r == "last") return &d.ports.back();
    if (!r.empty())
        for (const UsbPort& p : d.ports)
            if (&p != not_this && has(p.name, r)) return &p;
    for (const UsbPort& p : d.ports)
        if (&p != not_this) return &p;
    return nullptr;
}

std::string hex4(u16 v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04x", v);
    return b;
}

}  // namespace

const ProbeKind* probe_kind(const UsbDevice& d) {
    for (const ProbeKind& k : kKinds)
        if (d.vid == k.vid && (d.pid & k.mask) == k.pid && (!*k.says || has(d.manufacturer, k.says) || has(d.product, k.says))) return &k;
    // (CMSIS-DAP 2's specification: "The USB Device Product String must
    // contain CMSIS-DAP"; probe-rs 0.32.0 probe-rs/src/probe/cmsisdap/tools.rs
    // takes it from an interface's name too.)
    static const ProbeKind cmsis{0, 0, 0, "", "cmsis-dap", "", kProbe};
    bool says = has(d.product, "CMSIS-DAP");
    for (const std::string& i : d.interfaces) says |= has(i, "CMSIS-DAP");
    return says ? &cmsis : nullptr;
}

std::vector<ProbeRow> probe_rows(const std::vector<UsbDevice>& devices, const std::string& host) {
    std::vector<ProbeRow> named, others;
    for (const UsbDevice& d : devices) {
        if (d.cls == 9) continue;  // (a hub)
        ProbeRow r;
        r.host = host;
        r.usb = hex4(d.vid) + ":" + hex4(d.pid);
        r.serial = d.serial;
        const ProbeKind* k = probe_kind(d);
        if (k) {
            r.role = k->role;
            r.kind = k->kind;
            r.name = *k->name ? k->name : !d.product.empty() ? d.product : "CMSIS-DAP probe";
            if (r.kind == "stlink") r.serial = stlink_serial(d.serial);
            // (The RP2040's boot ROM's serial number is its version, the
            // chip's git reference and the ROM's, the same on every RP2040:
            // boot ROM b2, bootrom/usb_boot_device.c lines 428-429. It names
            // no board.)
            if (d.vid == 0x2e8a && d.pid == 0x0003) r.serial.clear();
            const UsbPort* gdb = pick_port(d, k->gdb);
            if (const UsbPort* console = pick_port(d, k->console, gdb)) r.port = console->path;
            if (gdb) r.gdb = gdb->path;
            // (A bridge's own name for itself says which chip it is.)
            if (r.role == kSerial && !d.product.empty()) r.name += " (" + d.product + ")";
            named.push_back(std::move(r));
            continue;
        }
        if (!d.ports.empty()) {
            r.role = kSerial;
            r.kind = "serial";
            r.name = !d.product.empty() ? d.product : !d.manufacturer.empty() ? d.manufacturer : "USB serial port";
            r.port = d.ports.front().path;
            named.push_back(std::move(r));
            continue;
        }
        r.role = kOther;
        r.kind = "usb";
        r.name = !d.product.empty() ? d.product : !d.manufacturer.empty() ? d.manufacturer : "USB device";
        others.push_back(std::move(r));
    }
    std::vector<ProbeRow>& rows = named.empty() ? others : named;
    std::stable_sort(rows.begin(), rows.end(), [](const ProbeRow& a, const ProbeRow& b) {
        const int ra = a.role == kFpga ? 0 : static_cast<int>(a.role), rb = b.role == kFpga ? 0 : static_cast<int>(b.role);
        if (ra != rb) return ra < rb;
        if (a.name != b.name) return a.name < b.name;
        return a.serial < b.serial;
    });
    for (ProbeRow& r : rows) {
        r.text = r.name;
        if (!r.serial.empty()) r.text += "  " + r.serial;
        if (!r.port.empty()) r.text += "  " + r.port;
        if (!r.gdb.empty()) r.text += "  gdb " + r.gdb;
        if (r.role == kOther) r.text += "  " + r.usb;
        if (!r.host.empty()) r.text += "  on " + r.host;
    }
    return std::move(rows);
}

// ---------------------------------------------------------------- the board object

namespace {

// JSON text edited in place: a value's span found by skimming (strings,
// brackets), so that what is not changed stays as it was written. (blit's
// JSON writer writes whole documents: a run.json is the user's.)
size_t skip_space(std::string_view t, size_t i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
    return i;
}

// The end of the value at i (after it), or npos.
size_t skip_value(std::string_view t, size_t i) {
    if (i >= t.size()) return std::string_view::npos;
    if (t[i] == '"') {
        for (++i; i < t.size(); ++i) {
            if (t[i] == '\\') ++i;
            else if (t[i] == '"') return i + 1;
        }
        return std::string_view::npos;
    }
    if (t[i] == '{' || t[i] == '[') {
        int depth = 0;
        for (; i < t.size(); ++i) {
            const char c = t[i];
            if (c == '"') {
                const size_t e = skip_value(t, i);
                if (e == std::string_view::npos) return e;
                i = e - 1;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return std::string_view::npos;
    }
    while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']' && t[i] != ' ' && t[i] != '\n' && t[i] != '\r' && t[i] != '\t') ++i;
    return i;
}

// A member of the object or an element of the array whose value is at
// [open, close]: where its key starts (its value, for an element), and its
// value's span.
struct Item {
    size_t key = 0, value = 0, end = 0;
    std::string name;  // (a member's key)
};

bool items_of(std::string_view t, size_t open, std::vector<Item>* out, size_t* close) {
    out->clear();
    const bool object = t[open] == '{';
    size_t i = skip_space(t, open + 1);
    if (i < t.size() && t[i] == (object ? '}' : ']')) return *close = i, true;
    for (;;) {
        Item it;
        it.key = i;
        if (object) {
            const size_t ke = skip_value(t, i);
            if (ke == std::string_view::npos || t[i] != '"') return false;
            json::Doc k;
            if (!k.parse(std::string(t.substr(i, ke - i)))) return false;
            it.name = std::string(k.root().str());
            i = skip_space(t, ke);
            if (i >= t.size() || t[i] != ':') return false;
            i = skip_space(t, i + 1);
        }
        it.value = i;
        it.end = skip_value(t, i);
        if (it.end == std::string_view::npos) return false;
        out->push_back(std::move(it));
        i = skip_space(t, out->back().end);
        if (i < t.size() && t[i] == ',') {
            i = skip_space(t, i + 1);
            continue;
        }
        if (i < t.size() && t[i] == (object ? '}' : ']')) return *close = i, true;
        return false;
    }
}

// The spaces before position i on its line (and whether only spaces are
// before it there).
std::string indent_at(std::string_view t, size_t i, bool* alone = nullptr) {
    size_t s = i;
    while (s > 0 && (t[s - 1] == ' ' || t[s - 1] == '\t')) --s;
    if (alone) *alone = s == 0 || t[s - 1] == '\n';
    return std::string(t.substr(s, i - s));
}

// Text in which each line after the first is moved in by `by`.
std::string moved_in(std::string_view v, const std::string& by) {
    std::string out;
    for (const char c : v) {
        out += c;
        if (c == '\n') out += by;
    }
    return out;
}

// The member `key` of the object at `open` set to `value` (JSON text: its
// lines after the first written at the member's depth), put in last if it
// is not there.
bool set_member(std::string& t, size_t open, std::string_view key, std::string_view value) {
    std::vector<Item> items;
    size_t close = 0;
    if (!items_of(t, open, &items, &close)) return false;
    for (const Item& it : items)
        if (it.name == key) {
            t.replace(it.value, it.end - it.value, moved_in(value, indent_at(t, it.key)));
            return true;
        }
    std::string k;
    json::quote(k, key);
    if (!items.empty()) {
        bool alone = false;
        const std::string ind = indent_at(t, items.front().key, &alone);
        const std::string member = alone ? ",\n" + ind + k + ": " + moved_in(value, ind) : ", " + k + ": " + std::string(value);
        t.insert(items.back().end, member);
        return true;
    }
    // (An empty object: its members a step further in than its own line.)
    std::string outer;
    size_t s = open;
    while (s > 0 && t[s - 1] != '\n') --s;
    while (s < t.size() && (t[s] == ' ' || t[s] == '\t')) outer += t[s++];
    const std::string ind = outer + "  ";
    t.replace(open + 1, close - open - 1, "\n" + ind + k + ": " + moved_in(value, ind) + "\n" + outer);
    return true;
}

// The member `key` of the object at `open` taken out, with its comma.
bool remove_member(std::string& t, size_t open, std::string_view key) {
    std::vector<Item> items;
    size_t close = 0;
    if (!items_of(t, open, &items, &close)) return false;
    for (size_t n = 0; n < items.size(); ++n) {
        if (items[n].name != key) continue;
        if (n > 0) t.erase(items[n - 1].end, items[n].end - items[n - 1].end);
        else if (n + 1 < items.size()) t.erase(items[n].key, items[n + 1].key - items[n].key);
        else t.erase(open + 1, close - open - 1);
        return true;
    }
    return true;
}

// Where the value of member `key` of the object at `open` starts (npos: none).
size_t member_at(std::string_view t, size_t open, std::string_view key) {
    std::vector<Item> items;
    size_t close = 0;
    if (!items_of(t, open, &items, &close)) return std::string_view::npos;
    for (const Item& it : items)
        if (it.name == key) return it.value;
    return std::string_view::npos;
}

std::string quoted(std::string_view s) {
    std::string out;
    json::quote(out, s);
    return out;
}

std::string text_of(json::Value v) { return std::string(v.str()); }

}  // namespace

bool board_set_probe(std::string& text, std::string_view config, const ProbeRow& row, std::string* why) {
    std::string t = text;
    const size_t root = skip_space(t, 0);
    if (root >= t.size() || t[root] != '{') return *why = "it is not a JSON object", false;
    const size_t list = member_at(t, root, "configurations");
    if (list == std::string::npos || t[list] != '[') return *why = "it has no \"configurations\" list", false;
    std::vector<Item> configs;
    size_t close = 0;
    if (!items_of(t, list, &configs, &close)) return *why = "it is not JSON", false;
    size_t at = std::string::npos;
    for (const Item& c : configs) {
        if (t[c.value] != '{') continue;
        const size_t name = member_at(t, c.value, "name");
        if (name == std::string::npos) continue;
        json::Doc d;
        if (d.parse(t.substr(name, skip_value(t, name) - name)) && d.root().str() == config) at = c.value;
    }
    if (at == std::string::npos) return *why = "it has no configuration called " + std::string(config), false;
    // The probe (a port alone has none, but for the Pico SDK's, which is
    // the board itself: picotool finds it by that serial number), and the
    // port; nothing else of the board changes.
    std::string probe;
    if ((row.role != ProbeRole::kSerial || row.kind == "pico-serial") && !row.kind.empty()) {
        probe = "{\"kind\": " + quoted(row.kind);
        if (!row.serial.empty()) probe += ", \"serial\": " + quoted(row.serial);
        probe += ", \"usb\": " + quoted(row.usb);
        if (!row.gdb.empty()) probe += ", \"gdb\": " + quoted(row.gdb);
        probe += "}";
    }
    // (A board of the Boards view's: flashed by openFPGALoader, as its board or on its cable.)
    std::string fpga;
    if (!row.fpga_board.empty()) fpga = "{\"tool\": \"openfpgaloader\", \"board\": " + quoted(row.fpga_board) + "}";
    else if (!row.fpga_cable.empty()) fpga = "{\"tool\": \"openfpgaloader\", \"cable\": " + quoted(row.fpga_cable) + "}";
    size_t board = member_at(t, at, "board");
    if (board == std::string::npos || t[board] != '{') {  // (made whole: a line a member where the configuration has its members so, else on one line)
        std::vector<Item> members;
        size_t end = 0;
        bool alone = false;
        if (items_of(t, at, &members, &end) && !members.empty()) indent_at(t, members.front().key, &alone);
        std::vector<std::string> parts;
        if (!probe.empty()) parts.push_back("\"probe\": " + probe);
        if (!row.port.empty()) parts.push_back("\"serial\": {\"port\": " + quoted(row.port) + "}");
        if (!fpga.empty()) parts.push_back("\"flash\": " + fpga);
        std::string value = "{";
        for (size_t n = 0; n < parts.size(); ++n) value += (n ? (alone ? ",\n  " : ", ") : (alone ? "\n  " : "")) + parts[n];
        value += alone && !parts.empty() ? "\n}" : "}";
        if (!set_member(t, at, "board", value)) return *why = "it is not JSON", false;
        json::Doc check;
        if (!check.parse(t)) return *why = "it is not JSON", false;
        text = std::move(t);
        return true;
    }
    bool ok = probe.empty() ? remove_member(t, board, "probe") : set_member(t, board, "probe", probe);
    const size_t serial = member_at(t, board, "serial");
    if (serial != std::string::npos && t[serial] == '{') {  // (its baud, or whatever else it has, kept)
        ok = ok && (row.port.empty() ? remove_member(t, serial, "port") : set_member(t, serial, "port", quoted(row.port)));
        // (Nothing left in it: it goes.)
        std::vector<Item> left;
        size_t end = 0;
        if (ok && items_of(t, serial, &left, &end) && left.empty()) ok = remove_member(t, board, "serial");
    } else if (row.port.empty()) {
        ok = ok && remove_member(t, board, "serial");
    } else {
        ok = ok && set_member(t, board, "serial", "{\"port\": " + quoted(row.port) + "}");
    }
    if (ok && !fpga.empty()) {
        const size_t flash = member_at(t, board, "flash");
        // (A board loaded through Vivado stays so: the picker's board and cable are openFPGALoader's words, which it leaves alone.)
        const size_t tool = flash != std::string::npos && t[flash] == '{' ? member_at(t, flash, "tool") : flash;
        const bool vivado = tool != std::string::npos && t.compare(tool, 8, "\"vivado\"") == 0;
        if (vivado && t[flash] != '{') {
            ok = set_member(t, board, "flash", "{\"tool\": \"vivado\"" + fpga.substr(fpga.find(',')));
        } else if (flash != std::string::npos && t[flash] == '{') {  // (its address, or whatever else it has, kept)
            if (!vivado) ok = set_member(t, flash, "tool", "\"openfpgaloader\"");
            ok = ok && (row.fpga_board.empty() ? remove_member(t, flash, "board") : set_member(t, flash, "board", quoted(row.fpga_board)));
            ok = ok && (!row.fpga_board.empty() ? remove_member(t, flash, "cable") : set_member(t, flash, "cable", quoted(row.fpga_cable)));
        } else {
            ok = set_member(t, board, "flash", fpga);
        }
    }
    json::Doc check;
    if (!ok || !check.parse(t)) return *why = "it is not JSON", false;
    text = std::move(t);
    return true;
}

std::string board_probe_text(const Board& b) {
    if (b.probe.empty() && b.probe_kind.empty()) return {};
    std::string name = b.probe_kind;
    if (b.probe_usb.size() == 9 && b.probe_usb[4] == ':') {
        UsbDevice d;
        d.vid = static_cast<u16>(std::strtoul(b.probe_usb.substr(0, 4).c_str(), nullptr, 16));
        d.pid = static_cast<u16>(std::strtoul(b.probe_usb.substr(5).c_str(), nullptr, 16));
        // (Without the product string, which the board does not keep, a
        // Digilent board's chip is named as the plain chip.)
        if (const ProbeKind* k = probe_kind(d); k && *k->name) name = k->name;
    }
    if (name.empty()) name = "Probe";
    return b.probe.empty() ? name : name + " " + b.probe;
}

void board_read(json::Value b, const std::string& config, Board* out, std::string* problem) {
    if (!b.exists()) return;
    if (!b.is_object()) {
        *problem = std::string(kRunConfigs) + ": " + config + "'s board is an object";
        return;
    }
    out->set = true;
    const json::Value probe = b["probe"];
    if (probe.is_string()) {
        out->probe = text_of(probe);
    } else {
        out->probe = text_of(probe["serial"]);
        out->probe_kind = text_of(probe["kind"]);
        out->probe_usb = text_of(probe["usb"]);
        out->probe_gdb = text_of(probe["gdb"]);
    }
    const json::Value serial = b["serial"];
    if (serial.is_string()) {
        out->port = text_of(serial);
    } else {
        out->port = text_of(serial["port"]);
        out->baud = static_cast<int>(serial["baud"].integer());
    }
    out->chip = text_of(b["chip"]);
    out->elf = text_of(b["elf"]);
    out->svd = text_of(b["svd"]);
    out->flash = b["flash"].is_string() ? text_of(b["flash"]) : text_of(b["flash"]["tool"]);
    if (const json::Value f = b["flash"]; f.is_object()) {
        out->flash_address = text_of(f["address"]);
        out->flash_board = text_of(f["board"]);
        out->flash_cable = text_of(f["cable"]);
        out->flash_interface = text_of(f["interface"]);
        out->flash_target = text_of(f["target"]);
        out->flash_device = text_of(f["device"]);
        out->flash_alt = text_of(f["alt"]);
        out->flash_xcf = text_of(f["xcf"]);
        out->flash_cfgmem = text_of(f["cfgmem"]);
    }
    out->bitstream = text_of(b["bitstream"]);
    out->debug = b["debug"].is_string() ? text_of(b["debug"]) : text_of(b["debug"]["preset"]);
    if (const json::Value d = b["debug"]; d.is_object()) {  // (boarddebug.hpp's)
        out->debug_gdb = text_of(d["gdb"]);
        out->debug_port = d["port"].is_string() ? text_of(d["port"]) : d["port"].exists() ? std::to_string(d["port"].integer()) : std::string();
        out->debug_machine = text_of(d["machine"]);
        out->debug_script = text_of(d["script"]);
        out->debug_main = text_of(d["main"]);
    }
    if (const json::Value r = b["rtt"]; r.exists()) {  // (rtt.hpp's)
        out->rtt = r.is_object() || r.boolean();
        out->rtt_port = static_cast<int>(r["port"].integer());
        for (const json::Value c : r["channels"]) out->rtt_channels.push_back(static_cast<int>(c.integer()));
        out->rtt_address = r["address"].is_string() ? text_of(r["address"]) : r["address"].exists() ? std::to_string(r["address"].integer()) : std::string();
        if (r["defmt"].exists()) out->rtt_defmt = r["defmt"].boolean() ? "on" : "off";
        if (r.is_object() && r["enabled"].exists()) out->rtt = r["enabled"].boolean();
    }
    if (const json::Value s = b["semihosting"]; s.exists()) {
        out->semihosting = s.is_object() || s.boolean();
        out->semihosting_port = static_cast<int>(s["port"].integer());
        if (s["defmt"].exists()) out->semihosting_defmt = s["defmt"].boolean() ? "on" : "off";
        if (s.is_object() && s["enabled"].exists()) out->semihosting = s["enabled"].boolean();
    }
    if (!out->debug.empty() && std::ranges::find(kDebugPresets, out->debug) == std::end(kDebugPresets)) {
        std::string presets;
        for (const std::string_view p : kDebugPresets) presets += (presets.empty() ? "\"" : ", \"") + std::string(p) + "\"";
        *problem = std::string(kRunConfigs) + ": " + config + "'s board.debug is one of " + presets;
    }
    if (!out->flash.empty() && std::ranges::find(kFlashTools, out->flash) == std::end(kFlashTools)) {
        std::string tools;
        for (const std::string_view f : kFlashTools) tools += (tools.empty() ? "\"" : ", \"") + std::string(f) + "\"";
        *problem = std::string(kRunConfigs) + ": " + config + "'s board.flash is one of " + tools;
    }
}

namespace {

// The board's probe among these devices, for a probe whose serial number
// is not to be trusted (probe_location): an ST-LINK V2 by its serial
// number as the picker wrote it, else, when that matches none, the one
// plugged in (a clone gives another serial number at another plugging
// in: the lab box's, "6068C3BF..." one time and "37C3BF67..." the next,
// 2026-11-24); ST's DFU boot ROM, whose serial number is the chip's IDCODE
// and more that changes ("200364500000" as the system read it, "...0070"
// to dfu-util a minute on), the one plugged in, else the one with the
// board's serial number. Null for another probe, or none.
const UsbDevice* probe_device(const Board& b, const std::vector<UsbDevice>& devices) {
    const bool v2 = b.probe_usb == "0483:3748", rom = b.probe_usb == "0483:df11";
    if (!v2 && !rom) return nullptr;
    const u16 pid = v2 ? 0x3748 : 0xdf11;
    const UsbDevice *match = nullptr, *only = nullptr;
    int count = 0;
    for (const UsbDevice& d : devices) {
        if (d.vid != 0x0483 || d.pid != pid) continue;
        ++count, only = &d;
        if (!match && !b.probe.empty() && (v2 ? stlink_serial(d.serial) : d.serial) == b.probe) match = &d;
    }
    if (v2) return match ? match : count == 1 ? only : nullptr;
    return count == 1 ? only : match;
}

}  // namespace

std::string probe_location(const Board& b, const std::vector<UsbDevice>& devices) {
    const UsbDevice* d = probe_device(b, devices);
    if (!d) return {};
    // (Linux's sysfs name is libusb's; macOS's locationID has the bus in
    // its top byte and a port in each nibble after, to the first 0, as
    // libusb's darwin_usb.c reads it.)
    if (!d->location.starts_with("0x")) return d->location;
    const unsigned long id = std::strtoul(d->location.c_str() + 2, nullptr, 16);
    std::string out = std::to_string((id >> 24) & 0xff);
    for (int shift = 20; shift >= 0; shift -= 4) {
        const unsigned long port = (id >> shift) & 0xf;
        if (!port) break;
        out += (shift == 20 ? "-" : ".") + std::to_string(port);
    }
    return out;
}

void probe_locate(Board* b, const std::string& root) {
    if (b->probe_usb != "0483:3748" && b->probe_usb != "0483:df11") return;
    std::vector<UsbDevice> devices;
    std::string why;
    if (!usb_list_at(root, &devices, &why)) return;
    const UsbDevice* d = probe_device(*b, devices);
    if (!d) return;
    b->probe_location = probe_location(*b, devices);
    // (An ST-LINK V2 found other than by its serial number: probe-rs is
    // given the one it has now, as probe-rs reads it.)
    if (b->probe_usb == "0483:3748") b->probe = stlink_serial(d->serial);
}

bool usb_list_at(const std::string& root, std::vector<UsbDevice>* out, std::string* why) {
    std::string host, there;
    if (!split_remote(root, &host, &there)) {
        *out = usb_list();
        return true;
    }
    const std::shared_ptr<Remote> r = remote_for(root, &there, kRemoteWaitMs);
    const std::string agent = r ? r->agent_path() : std::string();
    if (agent.empty()) {
        *why = "No connection to " + host + ": its USB devices cannot be listed";
        return false;
    }
    std::string text;
    if (!fs_run(root, sh_word(agent) + " --usb", &text, 20'000) || !usb_parse(text, out)) {
        *why = "Cannot list the USB devices on " + host;
        return false;
    }
    return true;
}

}  // namespace blit
