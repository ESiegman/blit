// ESP-IDF's and PlatformIO's tasks that are blit's own (buildsdk.cpp's
// finders give them: BuildTask::action): Flash runs "Flash the board" on
// the project's firmware (appflash.cpp), Monitor opens its board's serial
// port in a serial tab (appserial.cpp's), at the speed the project says.
// The board is the run configuration's that has one, if one does (the
// probe and port chosen for it), with what it leaves unsaid taken from the
// project (its ELF, chip, speed and tools); else the project's alone. A
// board with no port known is looked for among the USB devices plugged in,
// on a worker: the one port that a board of its chip could be on is opened,
// several are offered to choose from (the choice kept for the folder while
// blit runs), and none is said.
#include <SDL3/SDL.h>

#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "flash.hpp"
#include "missing.hpp"
#include "probes.hpp"
#include "remote.hpp"
#include "serial.hpp"
#include "serialtab.hpp"
#include "usb.hpp"

namespace blit {

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 84;
    SDL_PushEvent(&ev);
}

// The family a chip is of, as the ports are chosen by.
bool rp_chip(std::string_view chip) { return chip.starts_with("rp2") || chip.starts_with("RP2"); }

}  // namespace

std::vector<ProbeRow> board_ports(const Board& b, const std::vector<UsbDevice>& devices) {
    std::vector<ProbeRow> out;
    const bool esp = esp_chip(b.chip), rp = rp_chip(b.chip);
    for (ProbeRow& r : probe_rows(devices)) {
        if (r.port.empty()) continue;
        const bool fits = esp ? r.kind == "esp-usb-jtag" || r.kind == "esp-usb-bridge" || r.kind == "cp210x" || r.kind == "ch340" : rp && r.usb.starts_with("2e8a:");
        if (fits) out.push_back(std::move(r));
    }
    return out;
}

struct App::FirmwareUi {
    std::mutex mu;
    // The ports listed for a Monitor (mu): the task, and the rows.
    struct Listed {
        BuildTask task;
        std::vector<ProbeRow> ports;
    };
    std::optional<Listed> listed;
    bool busy = false;
    std::unordered_map<std::string, std::string> chosen;  // by folder: the port chosen from several
    u64 holding = 0;  // the run of a task on the board by its own tool, while serial tabs are let go of for it
    // Last, so that it is joined first, while all it uses is still there.
    std::jthread worker;
};

BuildTask App::firmware_board(const BuildTask& task) {
    BuildTask c = task;
    const BuildTask* user = board_config();  // (the Run button's run configuration if it has a board, else the first that has one)
    if (!user || !user->config) {
        // (The port chosen for its Monitor, from several: its Flash's too,
        // so that a serial tab on it lets go of it for the flash.)
        if (c.board.port.empty() && c.board.probe.empty() && firmwareui_)
            if (const auto it = firmwareui_->chosen.find(build_root()); it != firmwareui_->chosen.end()) c.board.port = it->second;
        return c;
    }
    Board b = user->board;  // (its word first)
    const Board& own = task.board;
    if (b.elf.empty()) b.elf = own.elf;
    if (b.chip.empty()) b.chip = own.chip;
    if (b.flash.empty() && b.probe.empty() && b.probe_kind.empty()) b.flash = own.flash;
    if (b.baud <= 0) b.baud = own.baud;
    b.tool_dirs = own.tool_dirs;
    b.touch = own.touch && (b.flash.empty() || b.flash == "picotool");
    c.board = std::move(b);
    c.crash = user->crash;
    return c;
}

void App::firmware_monitor(const BuildTask& task) {
    const BuildTask c = firmware_board(task);
    const std::string root = build_root();
    std::string host, there;
    split_remote(root, &host, &there);  // (a board on another machine: its port opened there, by blit's agent)
    if (!firmwareui_) firmwareui_ = std::make_shared<FirmwareUi>();
    FirmwareUi& u = *firmwareui_;
    // Its port, as SerialTab::is takes it: its own (or the one chosen
    // before: firmware_board), else the probe's serial number (its first port).
    SerialPortId id;
    id.path = c.board.port;
    id.host = host;
    if (id.path.empty()) id.serial = c.board.probe;
    if (id.path.empty() && id.serial.empty()) {  // (looked for among those plugged in: a worker's)
        if (u.busy) return;
        if (u.worker.joinable()) u.worker.join();
        u.busy = true;
        u.worker = std::jthread([&u, c, root] {
            std::vector<UsbDevice> devices;
            std::string why;
            usb_list_at(root, &devices, &why);  // (none, when another machine's cannot be listed: said as none plugged in there)
            FirmwareUi::Listed l{c, board_ports(c.board, devices)};
            {
                std::lock_guard lock(u.mu);
                u.listed = std::move(l);
            }
            wake();
        });
        return;
    }
    // (Open already: its tab shown.)
    for (const auto& [d, s] : term_serials())
        if (s->is(id.serial, id.path, host)) {
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
    cfg.baud = c.board.baud > 0 ? static_cast<u32>(c.board.baud) : cfg_.serial_baud;
    SerialReturn ret = SerialReturn::kCR;
    serial_return_parse(cfg_.serial_line_end, &ret);
    serial_open(id, cfg, ret, &c);  // (its crash dumps decoded with its ELF)
}

void App::firmware_sync() {
    if (!firmwareui_) return;
    FirmwareUi& u = *firmwareui_;
    std::optional<FirmwareUi::Listed> l;
    {
        std::lock_guard lock(u.mu);
        l.swap(u.listed);
    }
    if (!l) return;
    u.busy = false;
    const std::string root = build_root();
    std::string host, there;
    split_remote(root, &host, &there);
    auto open = [this, root, task = l->task](const std::string& port) {
        if (!firmwareui_) return;
        firmwareui_->chosen[root] = port;
        firmware_monitor(task);
    };
    if (l->ports.empty()) {
        const std::string family = esp_chip(l->task.board.chip) ? "an ESP32's" : rp_chip(l->task.board.chip) ? "an RP2040's" : std::string();
        return notify(family.empty() ? l->task.firmware + "'s board has no serial port (a run configuration's board.serial says it)"
                                     : "No serial port of " + family + " is plugged in to " + machine_name(host),
                      1);
    }
    if (l->ports.size() == 1) return open(l->ports.front().port);
    std::vector<std::string> items;
    for (const ProbeRow& r : l->ports) items.push_back(r.port.substr(r.port.rfind('/') + 1) + "  " + r.name + (r.serial.empty() ? "" : "  " + r.serial));
    find_choose_item("Serial port", std::move(items), [ports = l->ports, open](int i) {
        if (i >= 0 && static_cast<size_t>(i) < ports.size()) open(ports[static_cast<size_t>(i)].port);
    });
}

void App::firmware_holding(u64 doc) {
    if (!doc) return;
    if (!firmwareui_) firmwareui_ = std::make_shared<FirmwareUi>();
    firmwareui_->holding = doc;
}

void App::firmware_ended(u64 doc) {
    if (!firmwareui_ || !doc || firmwareui_->holding != doc) return;
    firmwareui_->holding = 0;
    serial_reclaim();
}

void App::firmware_settle() {
    if (!firmwareui_) return;
    const u64 until = now_ns() + 30'000'000'000ull;
    while (firmwareui_->busy && now_ns() < until) {
        firmware_sync();
        if (firmwareui_->busy) SDL_Delay(2);
    }
}

}  // namespace blit
