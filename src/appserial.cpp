// Serial monitors: "Open a serial port…" lists the ports of the USB
// devices plugged in here (named as the probe picker names them, probes.cpp;
// on Linux each with its /dev/serial/by-id name, which the tab opens it
// by), or takes a path typed; "Open the board's serial monitor" opens the
// port of the board of the run configuration the Run button runs, at its
// speed, in one step. The tab is a terminal whose program is the device
// (appterm.cpp, serialtab.hpp). The commands here act on the serial tab in
// front, else the one on show, else the only one open. Flashing lets go of
// the board's port while its tool runs, and gives it back after
// (serial_release and serial_reclaim, called by appflash.cpp). Crash dumps
// in what a port sends are decoded in place, with its board's ELF
// (serial_open; appterm.cpp's SerialCrash), and are problems.
#include <SDL3/SDL.h>

#include <algorithm>
#include <ctime>
#include <mutex>
#include <optional>
#include <thread>

#include "app.hpp"
#include "crashscan.hpp"
#include "fsio.hpp"
#include "probes.hpp"
#include "remote.hpp"
#include "serialtab.hpp"
#include "usb.hpp"

namespace blit {

namespace {

// A row of the picker: the port, and what it says.
struct PortRow {
    SerialPortId id;
    std::string text;
};

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 57;
    SDL_PushEvent(&ev);
}

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Each port of each device (a worker's call: IOKit and sysfs are slow):
// "cu.usbmodem1101  Pico SDK USB serial (RP2040) (Pico)  A58565170D696C53",
// on Linux "ttyACM0  …  /dev/serial/by-id/usb-…", the device's name as the
// probe picker has it, and the interface's name where a device has more
// than one port (an FT2232's two channels).
std::vector<PortRow> port_rows(const std::vector<UsbDevice>& devices) {
    std::vector<PortRow> rows;
    for (const UsbDevice& d : devices) {
        if (d.ports.empty() || d.cls == 9) continue;
        const std::vector<ProbeRow> named = probe_rows({d});
        const std::string name = !named.empty() ? named.front().name : !d.product.empty() ? d.product : "USB serial port";
        for (size_t i = 0; i < d.ports.size(); ++i) {
            const UsbPort& p = d.ports[i];
            PortRow r;
            const std::string by_id = serial_by_id(p.path);
            r.id.path = !by_id.empty() ? by_id : p.path;
            r.id.serial = d.serial;
            r.id.vid = d.vid, r.id.pid = d.pid;
            r.id.iface = p.iface;
            r.id.name = name;
            r.text = base_name(p.path) + "  " + name;
            if (!d.serial.empty()) r.text += "  " + d.serial;
            if (d.ports.size() > 1) {  // (which of them: by its interface's name, else its number, as an FT2232's two say the same)
                const bool own = !p.name.empty() && std::ranges::count(d.ports, p.name, &UsbPort::name) == 1;
                r.text += own ? "  " + p.name : "  interface " + std::to_string(p.iface);
            }
            if (!by_id.empty()) r.text += "  " + by_id;
            rows.push_back(std::move(r));
        }
    }
    std::ranges::stable_sort(rows, {}, &PortRow::text);
    return rows;
}

// The speeds a find box offers (typed values are taken too): the common
// ones, ESP8266's boot ROM's 74880, and the fast ones bridges reach.
constexpr u32 kSpeeds[] = {300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200, 230400, 250000, 460800, 500000, 921600, 1000000, 1500000, 2000000, 3000000};
constexpr const char* kFramings[] = {"8N1", "8E1", "8O1", "8N2", "7E1", "7O1", "7N1"};
constexpr const char* kFlows[] = {"No flow control", "RTS/CTS flow control", "XON/XOFF flow control"};

}  // namespace

struct App::SerialUi {
    std::mutex mu;
    std::optional<std::vector<PortRow>> listed;  // (mu)
    bool busy = false;
    std::vector<PortRow> rows;  // the last listing's, for the tests
    std::vector<std::weak_ptr<SerialTab>> released;  // let go of for a flash, to give back
    // Last, so that it is joined first, while all it uses is still there.
    std::jthread worker;
};

// ---------------------------------------------------------------- opening one

void App::serial_pick() {
    if (!serialui_) serialui_ = std::make_shared<SerialUi>();
    SerialUi& u = *serialui_;
    if (u.busy) return;
    if (u.worker.joinable()) u.worker.join();
    u.busy = true;
    u.worker = std::jthread([&u] {
        std::vector<PortRow> rows = port_rows(usb_list());
        {
            std::lock_guard lock(u.mu);
            u.listed = std::move(rows);
        }
        wake();
    });
}

namespace {

// The serial tab on that port, if one is open.
Doc* open_on(const std::vector<std::pair<Doc*, std::shared_ptr<SerialTab>>>& tabs, const std::string& serial, const std::string& path) {
    for (const auto& [d, s] : tabs)
        if (s->is(serial, path)) return d;
    return nullptr;
}

}  // namespace

void App::serial_sync() {
    if (!serialui_) return;
    SerialUi& u = *serialui_;
    std::optional<std::vector<PortRow>> listed;
    {
        std::lock_guard lock(u.mu);
        listed.swap(u.listed);
    }
    if (!listed) return;
    u.busy = false;
    u.rows = *listed;
    const auto tabs = term_serials();
    std::vector<std::string> items;
    for (const PortRow& r : *listed) items.push_back(r.text + (open_on(tabs, r.id.serial, r.id.path) ? "  (open)" : ""));
    auto take = [this](SerialPortId id) {
        const auto open = term_serials();
        if (Doc* d = open_on(open, id.serial, id.path)) {  // (open already: its tab)
            Pane* p = nullptr;
            size_t i = 0;
            if (!find_doc(d->id, &p, &i) && dock_show(pane_)) find_doc(d->id, &p, &i);
            if (!p) return;
            Pane* const was = pane_;
            pane_ = p;
            tab_show(i);
            pane_ = was;
            focus_pane(p);
            return;
        }
        SerialConfig cfg;
        cfg.baud = cfg_.serial_baud;
        SerialReturn ret = SerialReturn::kCR;
        serial_return_parse(cfg_.serial_line_end, &ret);
        serial_open(id, cfg, ret, nullptr);
    };
    find_choose_item("Serial port", std::move(items), [rows = *listed, take](int i) {
        if (i >= 0 && static_cast<size_t>(i) < rows.size()) take(rows[static_cast<size_t>(i)].id);
    });
    find_take_typed([take](const std::string& typed) {  // (a port that is on no USB device: /dev/ttyAMA0, a path typed)
        std::string path = typed;
        while (!path.empty() && path.back() == ' ') path.pop_back();
        if (path.empty()) return;
        SerialPortId id;
        id.path = path;
        take(id);
    });
    if (listed->empty()) find_ask_footer("No USB serial ports are plugged in; a port's path may be typed", true);
}

void App::serial_settle() {
    if (!serialui_) return;
    const u64 until = now_ns() + 30'000'000'000ull;
    while (serialui_->busy && now_ns() < until) {
        serial_sync();
        if (serialui_->busy) SDL_Delay(2);
    }
}

// The board's port (the run configuration's board: its serial.port, else
// the port of the device with its probe's serial number), at its speed.
void App::serial_board() {
    if (!build_root().empty() && !build_ready_for(build_root())) return build_when_ready([this] { serial_board(); });
    const BuildTask* c = board_config();
    if (!c) return notify("No run configuration has a board");
    const Board& b = c->board;
    std::string host, there;
    if (split_remote(build_root(), &host, &there)) return notify(c->name + "'s board is on " + host + ": a serial port there cannot be opened from here yet", 1);
    if (b.port.empty() && b.probe.empty()) return notify(c->name + "'s board has no serial port", 1);
    if (Doc* d = open_on(term_serials(), b.probe, b.port)) {
        Pane* p = nullptr;
        size_t i = 0;
        if (!find_doc(d->id, &p, &i) && dock_show(pane_)) find_doc(d->id, &p, &i);
        if (!p) return;
        Pane* const was = pane_;
        pane_ = p;
        tab_show(i);
        pane_ = was;
        focus_pane(p);
        return;
    }
    SerialPortId id;
    id.path = b.port;
    if (b.port.empty()) id.serial = b.probe;  // (found by the probe's serial number: its first port)
    SerialConfig cfg;
    cfg.baud = b.baud > 0 ? static_cast<u32>(b.baud) : cfg_.serial_baud;
    SerialReturn ret = SerialReturn::kCR;
    serial_return_parse(cfg_.serial_line_end, &ret);
    serial_open(id, cfg, ret, c);
}

// A port chosen from the picker, or typed, is decoded with the board's of
// the run configuration whose port it is, if one is (by its serial.port, or
// its probe's serial number); else with the newest ELF the build made, and
// no patterns of a configuration's.
void App::serial_open(const SerialPortId& id, const SerialConfig& cfg, SerialReturn ret, const BuildTask* board) {
    if (!board) board = board_config_on(id);
    term_open_serial(id, cfg, ret, crash_options_of(board));
}

// ---------------------------------------------------------------- the tab's commands

std::shared_ptr<SerialTab> App::serial_here(Doc** doc, bool say) {
    std::shared_ptr<SerialTab> s;
    Doc* at = nullptr;
    if (pane_->doc && (s = term_serial(*pane_->doc))) at = pane_->doc;
    for (const std::unique_ptr<Pane>& p : panes_)
        if (!s && p->doc && (s = term_serial(*p->doc))) at = p->doc;
    if (!s) {
        const auto all = term_serials();
        if (all.size() == 1) at = all.front().first, s = all.front().second;
    }
    if (!s && say) notify("No serial tab is open");
    if (doc) *doc = at;
    return s;
}

void App::serial_speed_pick() {
    const std::shared_ptr<SerialTab> s = serial_here();
    if (!s) return;
    const SerialConfig now = s->config();
    std::vector<std::string> items;
    items.push_back(serial_text(now) + "  (now)");  // (first: Enter as it opens changes nothing)
    for (const u32 b : kSpeeds) items.push_back(std::to_string(b));
    for (const char* f : kFramings) items.push_back(f);
    for (const char* f : kFlows) items.push_back(f);
    std::weak_ptr<SerialTab> w = s;
    auto apply = [this, w](const std::string& text) {
        const std::shared_ptr<SerialTab> t = w.lock();
        if (!t) return;
        SerialConfig c = t->config();
        std::string words = text.substr(0, text.find("  ("));
        if (words.ends_with(" flow control")) words.resize(words.size() - 13);
        if (!serial_parse(words, &c)) return notify("Not a speed or a framing: " + text, 1);
        t->set_config(c);
        term_sync();
    };
    find_choose_item("Speed and framing", items, [items, apply](int i) {
        if (i >= 0 && static_cast<size_t>(i) < items.size()) apply(items[static_cast<size_t>(i)]);
    });
    find_take_typed(apply);
}

void App::serial_return_pick() {
    const std::shared_ptr<StreamTab> s = stream_here();  // (an RTT channel's too: apprtt.cpp)
    if (!s) return;
    const SerialReturn now = s->return_mode();
    static constexpr SerialReturn kAll[] = {SerialReturn::kCR, SerialReturn::kLF, SerialReturn::kCRLF};
    std::vector<std::string> items;
    for (const SerialReturn r : kAll) items.push_back(std::string(serial_return_name(r)) + (r == now ? "  (now)" : ""));
    std::weak_ptr<StreamTab> w = s;
    find_choose_item("Return sends", std::move(items), [this, w](int i) {
        const std::shared_ptr<StreamTab> t = w.lock();
        if (!t || i < 0 || i > 2) return;
        t->set_return(kAll[i]);
        notify(std::string("Return sends ") + serial_return_name(kAll[i]));
    });
}

void App::serial_stamps() {
    if (const std::shared_ptr<StreamTab> s = stream_here()) s->set_stamps(!s->stamps());
    dirty_ = true;
}

void App::serial_hex() {
    if (const std::shared_ptr<StreamTab> s = stream_here()) s->set_hex(!s->hex());
    dirty_ = true;
}

void App::serial_log_pick() {
    Doc* sd = nullptr;
    const std::shared_ptr<StreamTab> s = stream_here(&sd);
    if (!s) return;
    // A name of its own beside the project: the port's, and when.
    const time_t now = std::time(nullptr);
    tm t{};
    ::localtime_r(&now, &t);
    char when[32];
    std::strftime(when, sizeof when, "%Y%m%d-%H%M%S", &t);
    std::string dir = build_root();
    if (dir.empty() || is_remote(dir)) dir = default_workdir();
    std::string name = s->name();
    for (char& c : name)
        if (c == ' ' || c == ':' || c == '/') c = '-';
    const std::string start = dir + (sd && term_link(*sd) ? "/rtt-" : "/serial-") + name + "-" + when + ".log";
    std::weak_ptr<StreamTab> w = s;
    find_choose(kFindInput, "Log to", start, [this, w](const std::string& typed) {
        const std::shared_ptr<StreamTab> tab = w.lock();
        if (!tab || typed.empty()) return;
        std::string why;
        const std::string path = full_path(typed);
        if (!tab->log_to(path, &why)) return notify(why, 1);
        notify("Logging to " + path);
    });
}

void App::serial_log_stop() {
    const std::shared_ptr<StreamTab> s = stream_here();
    if (!s) return;
    const std::string was = s->log_path();
    if (was.empty()) return notify("The tab is not being logged");
    s->log_stop();
    notify("Logged to " + was);
}

void App::serial_send_pick() {
    Doc* d = nullptr;
    const std::shared_ptr<SerialTab> s = serial_here(&d);
    if (!s || !d) return;
    std::string start = build_root();
    if (start.empty() || is_remote(start)) start = default_workdir();
    const u64 doc = d->id;
    find_choose(kFindOpen, "Send to the port", start + "/", [this, doc](const std::string& path) {
        Doc* to = find_doc(doc);
        if (!to) {
            for (const auto& [h, tab] : term_serials())
                if (h->id == doc) to = h;
        }
        const std::shared_ptr<SerialTab> port = to ? term_serial(*to) : nullptr;
        if (!port || path.empty()) return;
        if (is_remote(path)) return notify("Only a file here can be sent to the port", 1);
        std::string bytes;
        if (!fs_read(path, &bytes, 64u << 20)) return notify("Cannot read " + path, 1);
        if (!port->is_open()) return notify(base_name(port->id().path) + " is not open", 1);
        term_send(*to, bytes);
        char size[32];
        if (bytes.size() < 10'000) std::snprintf(size, sizeof size, "%zu bytes", bytes.size());
        else std::snprintf(size, sizeof size, "%.1f KB", static_cast<double>(bytes.size()) / 1024.0);
        notify("Sending " + base_name(path) + " (" + size + ") to " + base_name(port->device()));
    });
}

void App::serial_line(bool dtr) {
    const std::shared_ptr<SerialTab> s = serial_here();
    if (!s) return;
    bool d = true, r = true;
    s->lines(&d, &r);
    if (dtr) s->set_lines(d ? 0 : 1, -1);
    else s->set_lines(-1, r ? 0 : 1);
    term_sync();
}

void App::serial_reset(bool esp) {
    const std::shared_ptr<SerialTab> s = serial_here();
    if (!s) return;
    if (esp) s->reset_esp();
    else s->reset_arduino();
    term_sync();
}

void App::serial_break() {
    if (const std::shared_ptr<SerialTab> s = serial_here()) s->send_break();
    term_sync();
}

// ---------------------------------------------------------------- around flashing

// The serial tabs on the board's port (its probe's serial number, or the
// port) let go of it, each once its reader has closed it (a second at
// most), so that the flash tool can open it; given back by serial_reclaim.
void App::serial_release(const Board& b) {
    if (b.probe.empty() && b.port.empty()) return;
    if (!serialui_) serialui_ = std::make_shared<SerialUi>();
    for (const auto& [d, s] : term_serials()) {
        if (s->released() || !s->is(b.probe, b.port)) continue;
        s->release();
        serialui_->released.push_back(s);
    }
    term_sync();
}

void App::serial_reclaim() {
    if (!serialui_) return;
    for (const std::weak_ptr<SerialTab>& w : serialui_->released)
        if (const std::shared_ptr<SerialTab> s = w.lock()) s->reclaim();
    serialui_->released.clear();
}

// ---------------------------------------------------------------- for the tests

std::string App::serial_dump() {
    std::string out;
    Doc* d = nullptr;
    if (const std::shared_ptr<SerialTab> s = serial_here(&d, false); s && d) {
        term_sync();
        out = d->title + ": " + s->dump();
    }
    if (serialui_ && !serialui_->rows.empty()) {
        std::string rows;
        for (const PortRow& r : serialui_->rows) rows += (rows.empty() ? "" : " | ") + r.text;
        out += (out.empty() ? "" : " || ") + rows;
    }
    return out.empty() ? "no serial tab" : out;
}

}  // namespace blit
