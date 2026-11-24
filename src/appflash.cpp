// "Flash the board": the board of the run configuration the Run button
// runs (else the first that has one) flashed with its tool (flash.hpp's
// table), its build run first, the image found and hashed on a worker
// (here or on the project's machine: files read there), then the tool run
// as a task of kind "flash" (or "program", an FPGA) in the build's output
// terminal, so that its progress and what it came to show on the Run
// button and the configuration's row. A flash of an image (with the same
// tool, probe and target) that went well last time is not run again unless
// asked ("Flash anyway"), for as long as blit runs; a failed or stopped one
// is never remembered as done. An FPGA's SRAM load is always run (it is
// gone with the board's power, which blit does not see).
#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "engine.hpp"
#include "flash.hpp"
#include "fsio.hpp"
#include "missing.hpp"
#include "probes.hpp"
#include "remote.hpp"
#include "usb.hpp"

namespace blit {

namespace {

// What the worker found: the task to run, or why there is none.
struct Prepared {
    std::string board, row, key, why;  // (board: the folder and configuration; row: the configuration's task_key)
    bool keep = false, force = false;  // a write that stays (not an SRAM load); run whatever was last flashed
    BuildTask task;
};

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 53;
    SDL_PushEvent(&ev);
}

// The newest bitstream (.bit, else .bin) under a folder, as a path from it
// ("" none): walked here, asked of `find` there (a worker's call). With
// `ext` (".xcf"), the newest file of that one kind.
std::string newest_bitstream(const std::string& root, std::string_view ext = {}) {
    const std::string first = ext.empty() ? ".bit" : std::string(ext), second = ext.empty() ? ".bin" : std::string(ext);
    std::string host, there;
    if (split_remote(root, &host, &there)) {
        std::string out;
        if (!fs_run(root, "find . -maxdepth 6 -path '*/.*' -prune -o -type f \\( -name '*" + first + "' -o -name '*" + second + "' \\) -exec ls -t {} + 2>/dev/null", &out, 20'000)) return {};
        std::string bin;
        for (size_t at = 0; at < out.size();) {
            const size_t nl = std::min(out.find('\n', at), out.size());
            std::string line = out.substr(at, nl - at);
            at = nl + 1;
            if (line.starts_with("./")) line = line.substr(2);
            if (line.ends_with(first)) return line;
            if (bin.empty() && line.ends_with(second)) bin = line;
        }
        return bin;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path best[2];
    fs::file_time_type when[2];
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (name.starts_with('.') || name == "node_modules" || it.depth() >= 5) it.disable_recursion_pending();
            continue;
        }
        const int kind = name.ends_with(first) ? 0 : name.ends_with(second) ? 1 : -1;
        if (kind < 0 || !it->is_regular_file(ec)) continue;
        const fs::file_time_type t = it->last_write_time(ec);
        if (best[kind].empty() || t > when[kind]) best[kind] = it->path(), when[kind] = t;
    }
    const fs::path& p = !best[0].empty() ? best[0] : best[1];
    return p.empty() ? std::string() : p.lexically_relative(root).string();
}

// The folder of the configuration's ELF (else "build"), where ESP-IDF's
// build writes flasher_args.json beside it.
std::string elf_of(const std::string& root, const BuildTask& c) {
    const Board& b = c.board;
    if (!b.elf.empty()) return b.elf;
    if (c.program.starts_with(root + "/")) return c.program.substr(root.size() + 1);
    return c.program;
}
std::string esp_args_dir(const std::string& elf) {
    const size_t slash = elf.rfind('/');
    return slash != std::string::npos ? elf.substr(0, slash) : "build";
}

// An FPGA's programmer (openFPGALoader, Vivado): a bitstream, into SRAM unless its flash is asked for.
bool fpga_tool(const std::string& tool) {
    const FlashTool* t = flash_tool(tool);
    return t && t->kind == "program";
}

// A path without its image's extension ("build/blinky.elf": "build/blinky").
std::string stem_of(const std::string& path) {
    const size_t slash = path.rfind('/'), dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    const std::string_view ext = std::string_view(path).substr(dot);
    return ext == ".elf" || ext == ".bin" || ext == ".uf2" || ext == ".hex" || ext == ".dfu" ? path.substr(0, dot) : path;
}

// The folder of the configuration's ELF, and those above it in the
// project, for a Pico SDK build's CMakeCache.txt (its platform; "" none).
std::string pico_build(const std::string& root, const std::string& elf) {
    std::string dir = esp_args_dir(elf);
    for (int up = 0; up < 4 && !dir.empty(); ++up) {
        std::string text;
        if (fs_read(root + "/" + dir + "/CMakeCache.txt", &text, 1u << 20)) return pico_platform(text);
        const size_t slash = dir.rfind('/');
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash);
    }
    return {};
}

// What a board flashes (a worker's call: files read, here or there).
bool find_image(const std::string& root, const BuildTask& c, const std::string& tool, FlashImage* img, std::string* why) {
    const Board& b = c.board;
    if (tool == "pgrcmd") {  // (its chain file names the bitstream)
        const std::string xcf = !b.flash_xcf.empty() ? b.flash_xcf : newest_bitstream(root, ".xcf");
        if (xcf.empty()) return *why = "No chain file for pgrcmd: the board's flash.xcf, or an .xcf under the folder", false;
        img->parts.push_back({{}, xcf});
        return true;
    }
    if (fpga_tool(tool)) {
        const std::string bit = !b.bitstream.empty() ? b.bitstream : newest_bitstream(root);
        if (bit.empty()) return *why = "No bitstream to load: the board's bitstream, or a .bit the build made", false;
        img->parts.push_back({{}, bit});
        return true;
    }
    std::string elf = elf_of(root, c);
    // ESP-IDF's: the files and addresses its build wrote beside the ELF.
    const bool esp = tool == "esptool" || (tool == "openocd" && (b.probe_kind.starts_with("esp-") || b.chip.starts_with("esp") || b.chip.starts_with("ESP")));
    if (esp) {
        const std::string dir = esp_args_dir(elf);
        std::string text;
        if (fs_read(root + "/" + dir + "/flasher_args.json", &text)) return esp_flasher_args(text, dir, img, why);
    }
    if (elf.empty()) return *why = (c.firmware.empty() ? c.name : c.firmware) + "'s board has no image to flash (its elf)", false;
    // The file the tool writes, of its kind, beside the ELF the build made
    // (the Pico SDK writes NAME.uf2, NAME.bin and NAME.hex beside NAME.elf:
    // pico-sdk 2.3.1 src/cmake/on_device.cmake, pico_add_extra_outputs, line
    // 73); and a configuration's program named without its .elf (as one
    // made for a folder is: "build/blinky"), the .elf beside it.
    auto there = [&](const std::string& path) { return fs_kind(root + "/" + path) == 1; };
    if (stem_of(elf) == elf && !there(elf)) {
        if (there(elf + ".elf")) elf += ".elf";
        else if (there(elf + ".uf2")) elf += ".uf2";
    }
    if (tool == "uf2" && !elf.ends_with(".uf2")) {
        const std::string uf2 = stem_of(elf) + ".uf2";
        if (!there(uf2)) return *why = "No " + uf2 + " to copy to the board", false;
        elf = uf2;
    }
    // ST's DFU boot ROM is DfuSe, which writes a .bin at an address: with
    // none said, where the ELF beside it loads first, as objcopy -O binary
    // starts the .bin there (an STM32's flash, 0x08000000 on most).
    const bool st_rom = tool == "dfu-util" && b.probe_usb == "0483:df11";
    std::string address = b.flash_address;
    if (tool == "dfu-util" && !elf.ends_with(".bin") && !elf.ends_with(".dfu")) {
        const std::string bin = stem_of(elf) + ".bin";
        if (!there(bin)) return *why = "dfu-util writes a .bin or a .dfu: no " + bin + " beside " + elf, false;
        std::string bytes;
        std::vector<std::pair<u64, std::string_view>> loads;
        if (st_rom && address.empty() && fs_read(root + "/" + elf, &bytes) && elf_loads(bytes, &loads) && !loads.empty()) {
            u64 lowest = ~0ull;
            for (const auto& l : loads) lowest = std::min(lowest, l.first);
            char a[24];
            std::snprintf(a, sizeof a, "0x%08llx", static_cast<unsigned long long>(lowest));
            address = a;
        }
        elf = bin;
    }
    // (A .bin's address: esptool's are flasher_args.json's; picotool's is
    // the start of flash unless said; dfu-util's a DfuSe device's alone.)
    const bool bin = elf.ends_with(".bin");
    if (bin && address.empty() && ((tool != "esptool" && tool != "picotool" && tool != "dfu-util") || st_rom))
        return *why = elf + " is a .bin: the board's flash.address says where it goes", false;
    img->parts.push_back({bin ? address : std::string(), elf});
    return true;
}

std::string hex(u64 v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

Prepared prepare(const std::string& root, BuildTask c, std::string tool, bool guessed, FlashMode mode, bool force, std::stop_token stop) {
    Prepared p;
    const std::string who = c.firmware.empty() ? c.name : c.firmware;  // (a finder's Flash task: its firmware's name)
    // A board with no probe and no flash tool (a USB serial bridge, or no
    // device at all): the tool of the build beside its ELF, esptool for an
    // ESP-IDF build (which has said what it writes), picotool for a Pico
    // SDK one.
    if (guessed) {
        std::string text;
        if (fs_read(root + "/" + esp_args_dir(elf_of(root, c)) + "/flasher_args.json", &text)) {
            tool = "esptool";
        } else if (!pico_build(root, elf_of(root, c)).empty()) {
            tool = "picotool";
        } else {
            p.why = c.board.port.empty() ? who + "'s board has no flash tool, and its probe says none"
                                         : who + "'s board is a serial port with no flash tool or chip, and no ESP-IDF build says one (board.flash, or board.chip)";
            return p;
        }
    }
    p.board = root + "\n" + who;
    p.row = c.system + "\t" + c.name;
    p.force = force;
    p.keep = !(fpga_tool(tool) && mode != FlashMode::kFpgaFlash) && tool != "pgrcmd";  // (pgrcmd writes what its .xcf says: run every time)
    std::string host, there;
    split_remote(root, &host, &there);
    const std::string at = host.empty() ? std::string() : root.substr(0, root.size() - there.size());  // ("ssh://host", the machine's paths' start)
    // The tool's program, by its path on the machine it runs on: the
    // project SDK's own first (ESP-IDF's esptool, in its Python
    // environment, the version its build wrote flasher_args.json for, as
    // idf.py flash takes it), then the PATH's.
    std::string program;
    const std::vector<std::string> programs = flash_programs(c.board, tool);
    for (const std::string& dir : c.board.tool_dirs)
        for (const std::string& name : programs)
            if (program.empty() && fs_kind(at + dir + "/" + name) == 1) program = dir + "/" + name;
    for (const std::string& name : programs)
        if (program.empty()) program = Engine::which(host, name);
    // (Vivado, seldom on the PATH unless its settings64.sh was sourced: where AMD installs it.)
    if (program.empty() && tool == "vivado") program = vivado_installed(at);
    // A finder's board with no port (an ESP-IDF or PlatformIO project
    // without a run configuration's board): the one port here that a board
    // of its chip could be on, if there is one; with none or several,
    // esptool looks for itself, and picotool is not reset by a touch.
    if (!c.action.empty() && c.board.port.empty() && host.empty() && (tool == "esptool" || (tool == "picotool" && c.board.touch)))
        if (const std::vector<ProbeRow> ports = board_ports(c.board, usb_list()); ports.size() == 1) c.board.port = ports.front().port;
    // An RP2040 or RP2350: with no picotool, the .uf2 copied to its boot
    // ROM's volume, when it is in BOOTSEL; the board's "flash": "uf2", so
    // always. Its volume as its chip calls it, where its ids say.
    RpVolume volume;
    const std::string named = c.board.probe_usb == "2e8a:000f" || c.board.chip.starts_with("RP2350") || c.board.chip.starts_with("rp2350") ? "RP2350"
                              : c.board.probe_usb.starts_with("2e8a:") || c.board.chip.starts_with("RP2040") || c.board.chip.starts_with("rp2040") ? "RPI-RP2"
                                                                                                                                                     : "RPI-RP2 or RP2350";
    if (tool == "uf2" || (tool == "picotool" && program.empty())) {
        const std::vector<RpVolume> found = rp_volumes(at);
        const std::string missing = tool == "uf2" ? std::string() : not_installed("picotool", host) + ", and ";
        if (found.empty()) {
            p.why = (missing.empty() ? "No " + named + " volume is mounted on " + machine_name(host) : missing + "no " + named + " volume is mounted") +
                    ": hold BOOTSEL while plugging the board in";
            return p;
        }
        if (found.size() > 1) {
            std::string list;
            for (const RpVolume& v : found) list += (list.empty() ? "" : ", ") + v.path;
            p.why = missing + std::to_string(found.size()) + " RP2 boot ROM volumes are mounted on " + machine_name(host) + " (" + list + "): which is the board's is not known";
            return p;
        }
        volume = found.front();
        tool = "uf2";
    } else if (program.empty()) {
        p.why = not_installed(programs.empty() ? tool : programs.back(), host) + (tool == "vivado" ? " (nor in /tools/Xilinx or /opt/Xilinx)" : "");
        return p;
    }
    FlashImage img;
    if (!find_image(root, c, tool, &img, &p.why)) return p;
    img.volume = volume.path;
    // What it loads, hashed: with the tool, the probe and the target, what a
    // flash is the same flash with. (Read before the tool runs, so that a
    // file that is not there is said before picotool -f has rebooted the
    // board into BOOTSEL to find it missing.)
    u64 h = 0;
    for (const FlashImage::Part& part : img.parts) {
        std::string bytes, why;
        if (!fs_read_whole(root + "/" + part.path, &bytes, &why, 1ull << 30, stop)) {
            p.why = "Cannot read " + part.path + (why.empty() ? std::string() : ": " + why);
            return p;
        }
        if (tool == "uf2")
            if (const std::string no = uf2_refused(bytes, volume.board, &img.flash_bytes); !no.empty()) {
                p.why = part.path + " is " + no;
                return p;
            }
        h = h * 31 + flash_hash(bytes);
        if (fpga_tool(tool)) img.spi_width = bitstream_spi_width(bytes);  // (Vivado's write_cfgmem takes no other)
    }
    if (tool == "openocd" || tool == "dfu-util") probe_locate(&c.board, root);  // (an ST-LINK V2, ST's DFU boot ROM: by where it is, not its serial number)
    const FlashPlan plan = flash_plan(c.board, tool, program, img, mode);
    if (!plan.why.empty()) {
        p.why = plan.why;
        return p;
    }
    p.key = hex(h) + "|" + plan.same;
    p.task = c;
    p.task.config = false;
    p.task.action.clear();  // (what runs is the tool: its command)
    p.task.choices.clear();
    p.task.name = std::string(plan.kind == "program" ? "Program " : "Flash ") + who;
    p.task.kind = plan.kind;
    p.task.cmd = plan.command;
    p.task.cwd.clear();
    p.task.lasting = false;
    return p;
}

}  // namespace

struct App::FlashUi {
    std::mutex mu;
    std::optional<Prepared> ready;  // (mu)
    bool busy = false;              // an image being found
    // The flash running: its terminal, its board, and what it is remembered
    // as once it has gone well (keep: a write that stays).
    struct Running {
        u64 doc = 0;
        std::string board, key;
        bool keep = false;
    } run;
    std::unordered_map<std::string, std::string> flashed;  // by board: what it was last flashed with, well
    // What waits for the flash (F5's debugging, appdebug.cpp): told once it
    // has gone well or was not needed (unchanged), or that it failed.
    std::function<void(bool)> then;
    // Last, so that it is joined first, while all it uses is still there.
    std::jthread worker;
};

void App::flash_board(FlashMode mode, bool force, std::function<void(bool)> then, const BuildTask* config) {
    const std::string root = build_root();
    if (root.empty()) return notify("Open a folder first");
    if (!flashui_) flashui_ = std::make_shared<FlashUi>();
    if (flashui_->busy) {
        if (then) then(false);
        return;
    }
    // (A folder whose run configurations are still being read, on another
    // machine: the board is looked for once they have been.)
    if (!build_ready_for(root)) {
        std::optional<BuildTask> given;
        if (config) given = *config;
        return build_when_ready([this, mode, force, then, given] { flash_board(mode, force, then, given ? &*given : nullptr); });
    }
    flashui_->then = std::move(then);
    // (A task of the Build tab's own, a finder's Flash: its firmware, on
    // the run configuration's board if one has one: firmware_board.)
    const BuildTask* found = config ? config : board_config();
    if (!found) return notify("No run configuration has a board");
    const BuildTask c = config ? firmware_board(*config) : *found;
    const std::string who = c.firmware.empty() ? c.name : c.firmware;
    std::string tool = flash_tool_for(c.board);
    const Board& b = c.board;
    const bool guessed = tool.empty() && b.probe.empty() && b.probe_kind.empty();  // (prepare looks for ESP-IDF's build, or the Pico SDK's)
    if (tool.empty() && !guessed) return notify(who + "'s board has no flash tool, and its probe says none", 1);
    if (!guessed && !flash_tool(tool)) return notify("No flash tool called " + tool, 1);
    const bool fpga = !guessed && fpga_tool(tool);
    if (mode != FlashMode::kFlash && !fpga) return notify(who + "'s board is not an FPGA's" + (tool.empty() ? std::string() : " (it is flashed with " + tool + ")"), 1);
    build_first(std::string(fpga ? "programming " : "flashing ") + who, [this, root, c, tool, guessed, mode, force] {
        FlashUi& u = *flashui_;
        if (u.busy) return;
        if (u.worker.joinable()) u.worker.join();
        u.busy = true;
        u.worker = std::jthread([&u, root, c, tool, guessed, mode, force](std::stop_token stop) {
            Prepared p = prepare(root, c, tool, guessed, mode, force, stop);
            {
                std::lock_guard lock(u.mu);
                u.ready = std::move(p);
            }
            wake();
        });
    }, &c);
}

void App::flash_sync() {
    if (!flashui_) return;
    FlashUi& u = *flashui_;
    std::optional<Prepared> p;
    {
        std::lock_guard lock(u.mu);
        p.swap(u.ready);
    }
    if (!p) return;
    u.busy = false;
    if (!p->why.empty()) {
        if (std::function<void(bool)> then = std::exchange(u.then, nullptr)) then(false);
        return notify(p->why, 1);
    }
    if (p->keep && !p->force) {
        const auto it = u.flashed.find(p->board);
        if (it != u.flashed.end() && it->second == p->key) {
            build_note(p->row, p->task.kind == "program" ? "Not programmed: unchanged since the last flash" : "Not flashed: unchanged since the last flash");
            if (std::function<void(bool)> then = std::exchange(u.then, nullptr)) then(true);
            return;
        }
    }
    // (Serial tabs on the board's port let go of it while the tool runs, and
    // have it back once it has ended: appserial.cpp. The board flashed is
    // the one prepared, which the Run button may no longer name.)
    serial_release(p->task.board);
    const u64 doc = build_run_on(p->row, build_root(), p->task);
    if (!doc) {
        serial_reclaim();
        if (std::function<void(bool)> then = std::exchange(u.then, nullptr)) then(false);
        return notify("Cannot run " + p->task.name, 1);
    }
    u.run = {doc, p->board, p->key, p->keep};
}

void App::flash_ended(u64 doc, bool well) {
    if (!flashui_ || flashui_->run.doc != doc || !doc) return;
    serial_reclaim();  // (the serial tabs let go of for it: appserial.cpp)
    FlashUi& u = *flashui_;
    if (u.run.keep) {
        if (well) u.flashed[u.run.board] = u.run.key;
        else u.flashed.erase(u.run.board);  // (half written, or not at all: what is on it is not known)
    }
    u.run = {};
    if (std::function<void(bool)> then = std::exchange(u.then, nullptr)) then(well);
}

bool App::flash_busy() const { return flashui_ && (flashui_->busy || flashui_->run.doc); }

void App::flash_settle() {
    const u64 until = now_ns() + 60'000'000'000ull;
    while (now_ns() < until) {
        if (build_waiting()) build_settle();  // (a flash waiting for the folder's run configurations: read now)
        build_first_settle();
        if (!flashui_ || !flashui_->busy) {
            if (!build_first_busy()) return;
            continue;
        }
        flash_sync();
        if (flashui_->busy) SDL_Delay(2);
    }
}

std::string App::flash_dump() {
    const BuildTask* c = board_config();
    if (!c) return "no board";
    const Board& b = c->board;
    return c->name + ": tool " + flash_tool_for(b) + " | board " + b.flash_board + " | cable " + b.flash_cable + " | interface " + b.flash_interface + " | target " +
           b.flash_target + " | device " + b.flash_device + " | address " + b.flash_address + " | bitstream " + b.bitstream;
}

}  // namespace blit
