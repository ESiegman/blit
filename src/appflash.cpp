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
#include "remote.hpp"

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
// ("" none): walked here, asked of `find` there (a worker's call).
std::string newest_bitstream(const std::string& root) {
    std::string host, there;
    if (split_remote(root, &host, &there)) {
        std::string out;
        if (!fs_run(root, "find . -maxdepth 6 -path '*/.*' -prune -o -type f \\( -name '*.bit' -o -name '*.bin' \\) -exec ls -t {} + 2>/dev/null", &out, 20'000)) return {};
        std::string bin;
        for (size_t at = 0; at < out.size();) {
            const size_t nl = std::min(out.find('\n', at), out.size());
            std::string line = out.substr(at, nl - at);
            at = nl + 1;
            if (line.starts_with("./")) line = line.substr(2);
            if (line.ends_with(".bit")) return line;
            if (bin.empty() && line.ends_with(".bin")) bin = line;
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
        const int kind = name.ends_with(".bit") ? 0 : name.ends_with(".bin") ? 1 : -1;
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

// What a board flashes (a worker's call: files read, here or there).
bool find_image(const std::string& root, const BuildTask& c, const std::string& tool, FlashImage* img, std::string* why) {
    const Board& b = c.board;
    if (fpga_tool(tool)) {
        const std::string bit = !b.bitstream.empty() ? b.bitstream : newest_bitstream(root);
        if (bit.empty()) return *why = "No bitstream to load: the board's bitstream, or a .bit the build made", false;
        img->parts.push_back({{}, bit});
        return true;
    }
    const std::string elf = elf_of(root, c);
    // ESP-IDF's: the files and addresses its build wrote beside the ELF.
    const bool esp = tool == "esptool" || (tool == "openocd" && (b.probe_kind.starts_with("esp-") || b.chip.starts_with("esp") || b.chip.starts_with("ESP")));
    if (esp) {
        const std::string dir = esp_args_dir(elf);
        std::string text;
        if (fs_read(root + "/" + dir + "/flasher_args.json", &text)) return esp_flasher_args(text, dir, img, why);
    }
    if (elf.empty()) return *why = c.name + "'s board has no image to flash (its elf)", false;
    const bool bin = elf.ends_with(".bin");
    if (bin && b.flash_address.empty() && tool != "esptool") return *why = elf + " is a .bin: the board's flash.address says where it goes", false;
    img->parts.push_back({bin ? b.flash_address : std::string(), elf});
    return true;
}

std::string hex(u64 v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

Prepared prepare(const std::string& root, const BuildTask& c, const std::string& tool, bool guessed, FlashMode mode, bool force, std::stop_token stop) {
    Prepared p;
    // A board that is only a serial port (a USB serial bridge, no probe and
    // no chip): esptool, when an ESP-IDF build has said what it writes.
    if (guessed) {
        std::string text;
        if (!fs_read(root + "/" + esp_args_dir(elf_of(root, c)) + "/flasher_args.json", &text)) {
            p.why = c.name + "'s board is a serial port with no flash tool or chip, and no ESP-IDF build says one (board.flash, or board.chip)";
            return p;
        }
    }
    p.board = root + "\n" + c.name;
    p.row = c.system + "\t" + c.name;
    p.force = force;
    p.keep = !(fpga_tool(tool) && mode != FlashMode::kFpgaFlash);
    std::string host, there;
    split_remote(root, &host, &there);
    // The tool's program, by its path on the machine it runs on.
    std::string program;
    const std::vector<std::string> programs = flash_programs(c.board, tool);
    for (const std::string& name : programs)
        if (program.empty()) program = Engine::which(host, name);
    // (Vivado, seldom on the PATH unless its settings64.sh was sourced: where AMD installs it.)
    if (program.empty() && tool == "vivado") program = vivado_installed(host.empty() ? std::string() : root.substr(0, root.size() - there.size()));
    if (program.empty()) {
        p.why = not_installed(programs.empty() ? tool : programs.back(), host) + (tool == "vivado" ? " (nor in /tools/Xilinx or /opt/Xilinx)" : "");
        return p;
    }
    FlashImage img;
    if (!find_image(root, c, tool, &img, &p.why)) return p;
    // What it loads, hashed: with the tool, the probe and the target, what a flash is the same flash with.
    u64 h = 0;
    for (const FlashImage::Part& part : img.parts) {
        std::string bytes, why;
        if (!fs_read_whole(root + "/" + part.path, &bytes, &why, 1ull << 30, stop)) {
            p.why = "Cannot read " + part.path + (why.empty() ? std::string() : ": " + why);
            return p;
        }
        h = h * 31 + flash_hash(bytes);
        if (fpga_tool(tool)) img.spi_width = bitstream_spi_width(bytes);  // (Vivado's write_cfgmem takes no other)
    }
    const FlashPlan plan = flash_plan(c.board, tool, program, img, mode);
    if (!plan.why.empty()) {
        p.why = plan.why;
        return p;
    }
    p.key = hex(h) + "|" + plan.same;
    p.task = c;
    p.task.config = false;
    p.task.name = std::string(plan.kind == "program" ? "Program " : "Flash ") + c.name;
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

void App::flash_board(FlashMode mode, bool force, std::function<void(bool)> then) {
    const std::string root = build_root();
    if (root.empty()) return notify("Open a folder first");
    if (!flashui_) flashui_ = std::make_shared<FlashUi>();
    if (flashui_->busy) {
        if (then) then(false);
        return;
    }
    // (A folder whose run configurations are still being read, on another
    // machine: the board is looked for once they have been.)
    if (!build_ready_for(root)) return build_when_ready([this, mode, force, then] { flash_board(mode, force, then); });
    flashui_->then = std::move(then);
    const BuildTask* found = board_config();
    if (!found) return notify("No run configuration has a board");
    const BuildTask c = *found;
    std::string tool = flash_tool_for(c.board);
    const Board& b = c.board;
    const bool guessed = tool.empty() && b.probe.empty() && b.probe_kind.empty() && !b.port.empty();  // (prepare checks for ESP-IDF's build)
    if (guessed) tool = "esptool";
    if (tool.empty()) return notify(c.name + "'s board has no flash tool, and its probe says none", 1);
    if (!flash_tool(tool)) return notify("No flash tool called " + tool, 1);
    const bool fpga = fpga_tool(tool);
    if (mode != FlashMode::kFlash && !fpga) return notify(c.name + "'s board is not an FPGA's (it is flashed with " + tool + ")", 1);
    build_first(std::string(fpga ? "programming " : "flashing ") + c.name, [this, root, c, tool, guessed, mode, force] {
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
    });
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
    const u64 doc = build_run_on(p->row, build_root(), p->task);
    if (!doc) {
        if (std::function<void(bool)> then = std::exchange(u.then, nullptr)) then(false);
        return notify("Cannot run " + p->task.name, 1);
    }
    u.run = {doc, p->board, p->key, p->keep};
}

void App::flash_ended(u64 doc, bool well) {
    if (!flashui_ || flashui_->run.doc != doc || !doc) return;
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
