// The probe and port picker: "Choose the probe…" lists the debug probes,
// FPGA programmers, boot ROMs and serial ports plugged in (probes.cpp names
// them from what usb.cpp reads), a probe and its serial port as one row;
// Enter writes the one chosen into the `board` object of the run
// configuration the Run button runs (else the folder's first), in
// .blit/run.json, which is made if the folder has none. The Boards view's
// rows are among them: the FPGA boards and cables openFPGALoader sees
// (boards.cpp), each after its USB device's. On a project on another
// machine the devices are that machine's (blit-agent --usb, and
// openFPGALoader run there). The
// devices are read, and the file written, on a thread of the picker's own:
// IOKit, sysfs, the agent and a file on another machine all take a while.
#include <SDL3/SDL.h>

#include <mutex>
#include <optional>
#include <thread>

#include "app.hpp"
#include "boards.hpp"
#include "probes.hpp"
#include "remote.hpp"

namespace blit {

namespace {

struct Found {
    std::string root;  // the folder it was asked for
    std::vector<ProbeRow> rows;
    std::string why;  // (the devices could not be listed)
};

struct Wrote {
    std::string said;  // what was written, or why it could not be
    bool ok = false;
};

}  // namespace

struct App::ProbeUi {
    std::mutex mu;
    std::optional<Found> found;  // (mu)
    std::optional<Wrote> wrote;  // (mu)
    bool busy = false;           // listing, or writing
    std::vector<ProbeRow> rows;  // the last listing's, for the tests
    // Last, so that it is joined first, while all it uses is still there.
    std::jthread worker;
};

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 52;
    SDL_PushEvent(&ev);
}

// The devices on the machine `root` is on, as rows (a worker's call: on
// another machine the agent there is asked, which waits for its connection).
Found list_for(const std::string& root) {
    Found f;
    f.root = root;
    std::string host, there;
    if (!split_remote(root, &host, &there)) {
        f.rows = probe_rows(usb_list());
        f.rows = ofl_merge(f.rows, fpga_boards({}, f.rows));  // (the Boards view: what openFPGALoader sees, where it is installed)
        return f;
    }
    const std::shared_ptr<Remote> r = remote_for(root, &there, kRemoteWaitMs);
    const std::string agent = r ? r->agent_path() : std::string();
    if (agent.empty()) {
        f.why = "No connection to " + host;
        return f;
    }
    std::string out;
    std::vector<UsbDevice> devices;
    if (!fs_run(root, sh_word(agent) + " --usb", &out, 20'000) || !usb_parse(out, &devices)) {
        f.why = "Cannot list the USB devices on " + host;
        return f;
    }
    f.rows = probe_rows(devices, host);
    f.rows = ofl_merge(f.rows, fpga_boards(host, f.rows));
    return f;
}

// The row written into the board of the configuration named `config` (else
// the file's first; else one made for the folder, as "Edit the run
// configurations" makes it). A worker's call.
Wrote write_board(const std::string& root, std::string config, const ProbeRow& row) {
    const std::string path = root + "/" + kRunConfigs;
    std::string text, why;
    if (fs_kind(path) == 1) {
        if (!fs_read(path, &text)) return {"Cannot read " + path};
        if (config.empty()) {
            std::string problem;
            const std::vector<BuildTask> configs = read_run_configs(root, &problem);
            if (configs.empty()) return {std::string(kRunConfigs) + " has no configuration to give the board to"};
            config = configs.front().name;
        }
    } else {
        config = root.substr(root.find_last_of('/') + 1);
        std::string name, program;
        json::quote(name, config);
        json::quote(program, "build/" + config);
        text = "{\n  \"configurations\": [\n    {\n      \"name\": " + name + ",\n      \"program\": " + program + "\n    }\n  ]\n}\n";
        if (!fs_mkdirs(root + "/.blit", &why)) return {"Cannot write " + path + ": " + why};
    }
    if (!board_set_probe(text, config, row, &why)) return {"Cannot give " + config + " the board: " + std::string(kRunConfigs) + ": " + why};
    if (!fs_write(path, text, &why)) return {"Cannot write " + path + ": " + why};
    return {config + "'s board: " + row.name + (row.serial.empty() ? "" : " " + row.serial) + (row.port.empty() ? "" : ", " + row.port), true};
}

}  // namespace

void App::probe_choose() {
    const std::string root = build_root();
    if (root.empty()) return notify("Open a folder first");
    if (!probeui_) probeui_ = std::make_shared<ProbeUi>();
    ProbeUi& u = *probeui_;
    if (u.busy) return;
    if (u.worker.joinable()) u.worker.join();
    u.busy = true;
    u.worker = std::jthread([&u, root] {
        Found f = list_for(root);
        {
            std::lock_guard lock(u.mu);
            u.found = std::move(f);
        }
        wake();
    });
}

void App::probe_sync() {
    if (!probeui_) return;
    ProbeUi& u = *probeui_;
    std::optional<Found> f;
    std::optional<Wrote> w;
    {
        std::lock_guard lock(u.mu);
        f.swap(u.found);
        w.swap(u.wrote);
    }
    if (w) {
        u.busy = false;
        if (w->ok) build_changed();  // (the run configurations read again: the board is theirs)
        notify(w->said, w->ok ? 0 : 1);
    }
    if (!f) return;
    u.busy = false;
    u.rows = f->rows;
    if (!f->why.empty()) return notify(f->why, 1);
    if (f->rows.empty()) return notify("No USB devices are plugged in");
    std::vector<std::string> items;
    for (const ProbeRow& r : f->rows) items.push_back(r.text);
    find_choose_item("Probe", std::move(items), [this, rows = std::move(f->rows), root = f->root](int i) {
        if (i >= 0 && static_cast<size_t>(i) < rows.size()) probe_take(root, rows[static_cast<size_t>(i)]);
    });
}

void App::probe_take(const std::string& root, const ProbeRow& row) {
    ProbeUi& u = *probeui_;
    if (u.busy) return;
    std::string config;  // (the Run button's, when it is a run configuration of this folder)
    if (const BuildTask* c = run_config(); c && build_root() == root) config = c->name;
    if (u.worker.joinable()) u.worker.join();
    u.busy = true;
    u.worker = std::jthread([&u, root, config, row] {
        Wrote w = write_board(root, config, row);
        {
            std::lock_guard lock(u.mu);
            u.wrote = std::move(w);
        }
        wake();
    });
}

void App::probe_settle() {
    if (!probeui_) return;
    const u64 until = now_ns() + 60'000'000'000ull;
    while (probeui_->busy && now_ns() < until) {
        probe_sync();
        if (probeui_->busy) SDL_Delay(2);
    }
}

std::string App::probe_dump() {
    probe_settle();
    if (!probeui_) return "none";
    static const char* const kRole[] = {"probe", "fpga", "boot", "serial", "other"};
    std::string out;
    for (const ProbeRow& r : probeui_->rows)
        out += (out.empty() ? "" : " | ") + std::string(kRole[static_cast<int>(r.role)]) + " " + r.kind + " " + r.usb + ": " + r.text;
    return out.empty() ? "none" : out;
}

}  // namespace blit
