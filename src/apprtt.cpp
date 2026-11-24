// A board's RTT channels and semihosting output as tabs (the embedded
// design's E6; rtt.hpp says what each tool offers, linktab.hpp what a tab
// is). They open two ways:
//
//   - with a board's debug session (F5, or attaching), when its board
//     object asks for them ("rtt", "semihosting" in .blit/run.json): the
//     preset's server is told to serve them (boarddebug.cpp puts in what
//     rtt.cpp plans), and the tabs connect to it, or listen for it;
//   - on their own, for a board that runs ("Open the board's RTT"): the
//     preset's tool started for RTT alone, with no gdb: OpenOCD with its
//     Tcl port, J-Link's GDB server, pyOCD's gdb server, or probe-rs's DAP
//     server, whose RTT comes as events.
//
// OpenOCD's channels are not known until the control block is read, so a
// driver (RttDriver) talks to its Tcl port: it sets RTT up, starts it again
// until the program has made its block, lists the channels and starts a
// server for each, and the main thread opens (or finds again) a tab for
// each. Tabs stay when a session ends: their links try again, and take up
// where they were when the next session serves the same ports.
#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <optional>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "boarddebug.hpp"
#include "crashscan.hpp"
#include "dap.hpp"
#include "elf.hpp"
#include "flash.hpp"
#include "fsio.hpp"
#include "json.hpp"
#include "linktab.hpp"
#include "loopback.hpp"
#include "missing.hpp"
#include "remote.hpp"
#include "serialtab.hpp"
#include "rtt.hpp"

namespace blit {

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 83;
    SDL_PushEvent(&ev);
}

std::string quoted(std::string_view s) {
    std::string out;
    json::quote(out, s);
    return out;
}

std::string hex_of(u64 v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// A port no one listens on here, and none handed out before by this blit
// (two asked for one after the other are never the same).
u16 fresh_port() {
    static std::mutex mu;
    static std::vector<u16> given;
    std::lock_guard lock(mu);
    for (int tries = 0; tries < 16; ++tries) {
        const u16 p = loopback_free_port();
        if (!p) return 0;
        if (std::ranges::find(given, p) != given.end()) continue;
        given.push_back(p);
        if (given.size() > 256) given.erase(given.begin());
        return p;
    }
    return 0;
}

// OpenOCD's RTT, through its Tcl port (rtt.hpp): on each connection (each
// OpenOCD started), `rtt setup`, then `rtt start` and `rtt channels` every
// 300 ms until the control block is found, then a server for each
// up-channel, on the board's port + its number, else a free port. What it
// found is handed to the main thread.
struct RttDriver {
    std::string board;
    u16 base = 0;  // (0: each channel's server on a free port)
    u64 addr = 0, size = 0;
    std::unique_ptr<Loopback> link;
    std::mutex mu;
    std::string buf;  // (the link's thread) answers not ended yet
    enum : u8 { kIdle, kSetup, kStart, kChannels, kWaiting, kServers, kDone } step = kIdle;  // (mu)
    u64 due = 0;              // (mu) when kWaiting asks again
    int servers_left = 0;     // (mu)
    std::vector<RttChannel> found;  // (mu) for the main thread, once
    std::vector<u16> ports;         // (mu) and each one's server
    bool news = false;              // (mu)
    std::vector<std::string> said;  // (mu) for the main thread: what went wrong
    u32 tries = 0;                  // (mu) the times `rtt start` found nothing (the tests')
    std::jthread ticker;

    ~RttDriver() {  // (its threads stopped before what they use goes)
        ticker.request_stop();
        if (ticker.joinable()) ticker.join();
        link.reset();
    }

    void send(const std::string& line) {
        if (link) link->send(tcl_command(line));
    }
    void opened() {
        std::lock_guard lock(mu);
        buf.clear();
        step = kSetup;
        tries = 0;
        send("rtt setup " + hex_of(addr) + " " + std::to_string(size ? size : 0x400) + " {SEGGER RTT}");
    }
    void closed() {
        std::lock_guard lock(mu);
        step = kIdle;
    }
    void data(const char* p, size_t n) {
        std::lock_guard lock(mu);
        buf.append(p, n);
        for (const std::string& a : tcl_answers(buf)) answer(a);
    }
    // (mu held)
    void answer(const std::string& a) {
        switch (step) {
            case kSetup:
                if (!a.empty() && a.find("rtt:") != std::string::npos) said.push_back("OpenOCD: " + a), news = true;
                step = kStart;
                send("rtt start");
                break;
            case kStart:
                step = kChannels;
                send("rtt channels");
                break;
            case kChannels: {
                std::vector<RttChannel> up, down;
                if (!tcl_channels(a, &up, &down) || up.empty()) {  // (not made yet by the program: asked again)
                    ++tries;
                    step = kWaiting;
                    due = now_ns() + 300'000'000ull;
                    break;
                }
                step = kServers;
                servers_left = static_cast<int>(up.size());
                ports.clear();
                for (const RttChannel& c : up) {
                    const u16 port = base ? static_cast<u16>(base + c.number) : fresh_port();
                    ports.push_back(port);
                    send("rtt server start " + std::to_string(port) + " " + std::to_string(c.number));
                }
                found = up;
                news = true;
                break;
            }
            case kServers:
                // (A port another program has: OpenOCD says so, "couldn't bind", and the tab waits.)
                if (!a.empty() && (a.find("rror") != std::string::npos || a.find("couldn't") != std::string::npos)) said.push_back("OpenOCD: " + a), news = true;
                if (--servers_left <= 0) step = kDone;
                break;
            default:
                break;
        }
    }
    void tick() {
        std::lock_guard lock(mu);
        if (step == kWaiting && now_ns() >= due) {
            step = kStart;
            send("rtt start");
        }
    }
};

// A tool started for RTT on a running board, with no debug session; its
// errors kept, for why it ended.
struct RttServer {
    std::string board, preset;
    Spawned proc;
    std::mutex mu;
    std::string err;  // (mu)
    std::atomic<bool> ended{false};
    int code = 0;
    std::jthread reader;
    ~RttServer() {
        reader.request_stop();
        if (reader.joinable()) reader.join();
        spawn_end(proc);
    }
};

}  // namespace

struct App::RttUi {
    std::vector<std::shared_ptr<RttDriver>> drivers;
    std::vector<std::shared_ptr<RttServer>> servers;
    // probe-rs's DAP server, for RTT on a running board: its events.
    std::string prs_board;
    std::unique_ptr<Dap> prs;
    std::map<i64, u64> prs_tabs;  // channel → its tab (the Doc's id)
    std::string prs_said;
};

// ---------------------------------------------------------------- planning

// The streams a board's session (or server) serves, from its ELF here (an
// ELF on another machine: none, for now; the agent's TCP pump will reach
// its ports later).
BoardStreams App::rtt_streams(const BuildTask& c, const std::string& elf, const std::string& preset, bool standalone) {
    RttElf re;
    std::string host, there;
    if (!split_remote(elf, &host, &there)) {
        std::string why;
        if (const auto e = Elf::read_file(elf, &why)) re = rtt_elf(*e);
    }
    // (Its files: pyOCD's config, QEMU's output; one set for each board and
    // each blit, in the cache.)
    std::string files = cache_dir() + "/rtt", why, name;
    fs_mkdirs(files, &why);
    for (const char ch : c.name) name += (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ? ch : '-';
    files += "/" + std::to_string(::getpid()) + "-" + name;
    return board_streams(c.board, preset, re, fresh_port, files, standalone);
}

// The tabs a board's streams have, opened (or found open, and pointed at
// this session's ports), the OpenOCD driver started, pyOCD's config
// written, and what this preset cannot give said.
void App::rtt_serve(const BuildTask& c, const std::string& elf, const BoardStreams& s) {
    if (!rttui_) rttui_ = std::make_shared<RttUi>();
    RttUi& u = *rttui_;
    for (const std::string& n : s.notes) notify(n, 1);
    std::string why;
    if (!s.pyocd_config.empty() && !fs_write(s.pyocd_path, s.pyocd_config, &why)) notify("Cannot write " + s.pyocd_path + " for pyOCD's RTT: " + why, 1);
    if (!s.server_out.empty() && !fs_write(s.server_out, "", &why)) notify("Cannot write " + s.server_out + " for the board's output: " + why, 1);  // (emptied: this session's alone)
    for (const StreamPlan& p : s.tabs) rtt_tab(c, elf, p);
    // OpenOCD's driver: one for each board, pointed at this OpenOCD's Tcl port.
    std::erase_if(u.drivers, [&](const std::shared_ptr<RttDriver>& d) { return d->board == c.name; });
    if (s.tcl_port) {
        auto d = std::make_shared<RttDriver>();
        d->board = c.name;
        d->base = s.rtt_base;
        RttElf re;
        std::string unread;
        if (const auto e = Elf::read_file(elf, &unread)) re = rtt_elf(*e);
        d->addr = !c.board.rtt_address.empty() ? std::strtoull(c.board.rtt_address.c_str(), nullptr, 0) : re.addr;
        d->size = re.size;
        RttDriver* raw = d.get();
        Loopback::Options o;
        o.end.port = s.tcl_port;
        Loopback::Events e;
        e.opened = [raw] { raw->opened(); };
        e.closed = [raw](const std::string&) { raw->closed(); };
        e.data = [raw](const char* p, size_t n) {
            raw->data(p, n);
            bool news;
            {
                std::lock_guard lock(raw->mu);
                news = raw->news;
            }
            if (news) wake();
        };
        d->link = std::make_unique<Loopback>(std::move(o), std::move(e));
        d->ticker = std::jthread([raw](std::stop_token stop) {
            while (!stop.stop_requested()) {
                raw->tick();
                SDL_Delay(50);
            }
        });
        u.drivers.push_back(std::move(d));
    }
}

// A stream's tab: the open one of that board, kind and channel (pointed at
// this plan's port), else a new one.
u64 App::rtt_tab(const BuildTask& c, const std::string& elf, const StreamPlan& p) {
    for (const auto& [d, t] : term_links())
        if (t->is(c.name, p.kind, p.channel)) {
            t->retarget(p);
            if (t->spec().plan.name != p.name && p.kind == "rtt") t->rename(p.name);
            t->retry_now();
            return d->id;
        }
    LinkSpec spec;
    spec.board = c.name;
    spec.plan = p;
    spec.pushed = p.pushed;
    spec.elf = elf;
    spec.dir = build_root();
    return term_open_link(std::move(spec), crash_options_of(&c), false);
}



// The board's session's streams, as its session starts: the plan made
// again with the ELF the build just made (its block's address may have
// moved), the tabs opened before the debugger starts (QEMU connects to
// blit's port as it starts).
void App::rtt_session(const BuildTask& c, const std::string& elf, bool attach, BoardDebug* plan) {
    const Board& b = c.board;
    if (!b.rtt && !b.semihosting) return;
    std::string host, there;
    if (split_remote(elf, &host, &there)) return notify(c.name + "'s board is on " + host + ": its RTT and semihosting ports there are not reached from here yet", 1);
    const BoardStreams s = rtt_streams(c, elf, plan->preset, false);
    *plan = board_debug(b, elf, attach, flash_tool_for(b).empty(), &s);
    rtt_serve(c, elf, s);
}

// ---------------------------------------------------------------- on its own

// "Open the board's RTT": the tabs of a board whose session serves them
// already shown; else its tool started for RTT alone (the board as it
// runs: not halted, not reset), and its tabs.
void App::rtt_open() {
    if (!build_root().empty() && !build_ready_for(build_root())) return build_when_ready([this] { rtt_open(); });
    const BuildTask* found = board_config();
    if (!found) return notify("No run configuration has a board");
    const BuildTask c = *found;
    const Board& b = c.board;
    const std::string root = build_root();
    if (is_remote(root)) return notify(c.name + "'s board is on another machine: its RTT is not reached from here yet", 1);
    const std::string elf = b.elf.empty() ? c.program : b.elf[0] == '/' ? b.elf : root + "/" + b.elf;
    const std::string preset = board_preset(b);
    if (preset.empty()) return notify(c.name + "'s board names no debug preset (board.debug), and its probe says none", 1);
    if (!rttui_) rttui_ = std::make_shared<RttUi>();
    RttUi& u = *rttui_;
    if (debug_running() && debug_board_name() == c.name) {  // (its session serves them)
        bool any = false;
        for (const auto& [d, t] : term_links())
            if (t->spec().board == c.name) any = true, rtt_show(d->id);
        if (!any) notify(c.name + "'s session serves no RTT: its board object has no \"rtt\"", 1);
        return;
    }
    Board rb = b;
    rb.rtt = true;
    BuildTask rc = c;
    rc.board = rb;
    if (preset == "probe-rs") return rtt_probe_rs(rc, elf);
    if (preset == "qemu" || preset == "renode" || preset == "blackmagic") return notify("The " + preset + " preset has no RTT blit reaches", 1);
    const BoardStreams s = rtt_streams(rc, elf, preset, true);
    if (!s.any()) {
        for (const std::string& n : s.notes) notify(n, 1);
        return;
    }
    // The tool: as the debug preset starts its server, but for RTT alone.
    std::string sh = "pick() { for p; do command -v \"$p\" >/dev/null 2>&1 && { printf '%s' \"$p\"; return 0; }; done; return 1; }; ";
    std::string tool;
    if (preset == "openocd" || preset == "esp") {
        const std::string iface = openocd_interface(b), tgt = openocd_target(b);
        if (iface.empty() || tgt.empty()) return notify("OpenOCD needs the probe's interface and the chip's target script (flash.interface, flash.target)", 1);
        tool = preset == "esp" || esp_chip(b.chip) ? "openocd-esp32 openocd" : "openocd";
        sh += "S=$(pick " + tool + ") || { echo \"blit-missing: " + tool + "\" >&2; exit 127; }; exec \"$S\" -c 'gdb_port disabled' -c 'telnet_port disabled'" + s.openocd_words.substr(0, s.openocd_words.find(" -c init")) +
              " -f " + sh_word(iface) + (b.probe.empty() ? std::string() : " -c " + sh_word("adapter serial " + b.probe)) + " -f " + sh_word(tgt) +
              (s.openocd_words.find(" -c init") != std::string::npos ? s.openocd_words.substr(s.openocd_words.find(" -c init")) : std::string(" -c init"));
    } else if (preset == "jlink") {
        const std::string device = !b.flash_device.empty() ? b.flash_device : b.chip;
        if (device.empty()) return notify("The J-Link GDB Server needs the board's chip (its device), or flash.device", 1);
        tool = "JLinkGDBServerCLExe JLinkGDBServer";
        // (-nohalt: connected without halting it; -noir: its registers left as they are.)
        sh += "S=$(pick " + tool + ") || { echo \"blit-missing: " + tool + "\" >&2; exit 127; }; exec \"$S\" -device " + sh_word(device) + " -if SWD -speed 4000 -nohalt -noir -nogui" +
              (b.probe.empty() ? std::string() : " -select " + sh_word("USB=" + b.probe)) + s.server_words;
    } else if (preset == "pyocd") {
        if (b.chip.empty()) return notify("pyOCD needs the board's chip (its target type)", 1);
        std::string type;
        for (const char ch : b.chip) type += ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
        tool = "pyocd";
        // (-M attach: connected as it runs, not halted: v0.46.1 subcommands/base.py line 95.)
        sh += "S=$(pick pyocd) || { echo \"blit-missing: pyocd\" >&2; exit 127; }; exec \"$S\" gdbserver -t " + sh_word(type) + (b.probe.empty() ? std::string() : " -u " + sh_word(b.probe)) +
              " -M attach --persist" + s.server_words;
    }
    std::erase_if(u.servers, [&](const std::shared_ptr<RttServer>& v) { return v->board == c.name; });  // (the last one for this board ended first: it has the probe)
    auto srv = std::make_shared<RttServer>();
    srv->board = c.name;
    srv->preset = preset;
    std::string why;
    srv->proc = spawn_piped({"/bin/sh", "-c", sh}, root, false, false, true, &why);
    if (srv->proc.pid < 0) return notify("Cannot start " + tool + ": " + why, 1);
    RttServer* raw = srv.get();
    srv->reader = std::jthread([raw](std::stop_token stop) {
        char buf[4096];
        while (!stop.stop_requested()) {
            pollfd p{raw->proc.err, POLLIN, 0};
            if (::poll(&p, 1, 200) <= 0) continue;
            const ssize_t k = ::read(raw->proc.err, buf, sizeof buf);
            if (k > 0) {
                std::lock_guard lock(raw->mu);
                raw->err.append(buf, static_cast<size_t>(k));
                if (raw->err.size() > 16384) raw->err.erase(0, raw->err.size() - 16384);
                continue;
            }
            if (k < 0 && errno == EAGAIN) continue;
            int st = 0;
            ::waitpid(raw->proc.pid, &st, 0);
            raw->proc.pid = -1;
            raw->code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
            raw->ended = true;
            wake();
            return;
        }
    });
    u.servers.push_back(std::move(srv));
    rtt_serve(rc, elf, s);
    notify("RTT on " + c.name + "'s board through " + preset);
}

void App::rtt_stop() {
    if (!rttui_) return notify("No RTT server is running");
    RttUi& u = *rttui_;
    const bool any = !u.servers.empty() || u.prs;
    u.servers.clear();
    if (u.prs) {
        u.prs->request("disconnect", "{\"terminateDebuggee\":false}");
        u.prs->stop(500);
        u.prs.reset();
        for (const auto& [ch, doc] : u.prs_tabs)
            for (const auto& [d, t] : term_links())
                if (d->id == doc) t->push_gone("probe-rs stopped");
    }
    u.drivers.clear();
    notify(any ? "RTT server stopped" : "No RTT server is running");
}

// probe-rs: its DAP server attached to the board as it runs, RTT on
// (probe-rs 0.32.0 cmd/dap_server/server/configuration.rs: chip, probe,
// coreConfigs[].programBinary and rttEnabled; its RTT comes as events).
void App::rtt_probe_rs(const BuildTask& c, const std::string& elf) {
    RttUi& u = *rttui_;
    const Board& b = c.board;
    if (b.chip.empty()) return notify("probe-rs needs the board's chip", 1);
    if (u.prs) {
        u.prs->stop(300);
        u.prs.reset();
    }
    u.prs = std::make_unique<Dap>();
    std::string err;
    if (!u.prs->start({"probe-rs", "dap-server"}, build_root(), [] { wake(); }, &err)) {
        u.prs.reset();
        return notify(err.find("No such file") != std::string::npos || err.empty() ? not_installed("probe-rs", {}) : "Cannot start probe-rs: " + err, 1);
    }
    u.prs_board = c.name;
    u.prs_said.clear();
    const std::string probe = b.probe_usb.size() == 9 ? (b.probe.empty() ? b.probe_usb : b.probe_usb + ":" + b.probe) : std::string();
    std::string args = "{\"chip\":" + quoted(b.chip) + (probe.empty() ? std::string() : ",\"probe\":" + quoted(probe)) + ",\"cwd\":" + quoted(build_root()) +
                       ",\"flashingConfig\":{\"flashingEnabled\":false},\"coreConfigs\":[{\"coreIndex\":0,\"programBinary\":" + quoted(elf) + ",\"rttEnabled\":true}]}";
    u.prs->request("initialize", "{\"clientID\":\"blit\",\"clientName\":\"blit\",\"adapterID\":\"probe-rs-debug\",\"linesStartAt1\":true,\"columnsStartAt1\":true,\"pathFormat\":\"path\"}",
                   [this, args](const Dap::Message& m) {
                       RttUi& v = *rttui_;
                       if (!v.prs) return;
                       if (!m.success) return notify("probe-rs would not start: " + m.message, 1);
                       v.prs->request("attach", args, [this](const Dap::Message& r) {
                           RttUi& w = *rttui_;
                           if (!w.prs) return;
                           if (!r.success) return notify("probe-rs could not attach: " + r.message, 1);
                           w.prs->request("configurationDone");
                       });
                   });
    notify("RTT on " + c.name + "'s board through probe-rs");
}

// ---------------------------------------------------------------- the main thread

// What the drivers found (a tab for each channel), the servers that ended
// (and why), probe-rs's events.
void App::rtt_sync() {
    if (!rttui_) return;
    RttUi& u = *rttui_;
    for (const std::shared_ptr<RttDriver>& d : u.drivers) {
        std::vector<RttChannel> found;
        std::vector<u16> ports;
        std::vector<std::string> said;
        {
            std::lock_guard lock(d->mu);
            if (!d->news) continue;
            d->news = false;
            found.swap(d->found);
            ports.swap(d->ports);
            said.swap(d->said);
        }
        for (const std::string& s : said) notify(s, 1);
        const std::optional<BuildTask> named = rtt_config(d->board);
        if (!named) continue;
        const BuildTask* c = &*named;
        const std::string root = build_root();
        const std::string elf = c->board.elf.empty() ? c->program : c->board.elf[0] == '/' ? c->board.elf : root + "/" + c->board.elf;
        RttElf re;
        std::string why;
        if (const auto e = Elf::read_file(elf, &why)) re = rtt_elf(*e);
        for (size_t i = 0; i < found.size() && i < ports.size(); ++i) {
            const RttChannel& ch = found[i];
            StreamPlan p;
            p.kind = "rtt";
            p.channel = ch.number;
            p.port = ports[i];
            const std::string_view mode = c->board.rtt_defmt;
            p.defmt = mode == "off" ? false : mode == "on" ? ch.name == "defmt" || (ch.name.empty() && ch.number == 0) : re.defmt && ch.name == "defmt";
            p.input = !p.defmt;
            p.name = "RTT " + std::to_string(ch.number) + (ch.name.empty() ? std::string() : ": " + ch.name);
            rtt_tab(*c, elf, p);
        }
    }
    for (auto it = u.servers.begin(); it != u.servers.end();) {
        RttServer& s = **it;
        if (!s.ended) {
            ++it;
            continue;
        }
        std::string err;
        {
            std::lock_guard lock(s.mu);
            err = s.err;
        }
        std::string why;
        if (const size_t m = err.find("blit-missing: "); m != std::string::npos) {
            std::string names = err.substr(m + 14, err.find('\n', m) - m - 14);
            for (char& ch : names)
                if (ch == ' ') ch = '|';
            why = board_missing(names, {});
        } else {
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
            why = s.preset + " ended (exit " + std::to_string(s.code) + ")" + (err.empty() ? std::string() : ": " + err.substr(err.rfind('\n') == std::string::npos ? 0 : err.rfind('\n') + 1));
        }
        notify(why, 1);
        it = u.servers.erase(it);
    }
    if (u.prs) {
        for (const Dap::Message& m : u.prs->poll()) {
            const json::Value b = m.body();
            if (m.name == "probe-rs-rtt-channel-config") {
                // (A channel, or a semihosting stream: probe-rs numbers those
                // from 1024, named "stdout" and "stderr": core_data.rs lines
                // 542-597.) Opened, then told it is: probe-rs sends a
                // channel's data only once its window is open (debugger.rs
                // lines 834-852).
                const i64 ch = b["channelNumber"].integer(-1);
                const std::string name(b["channelName"].str());
                const bool semi = ch >= 1024;
                const std::optional<BuildTask> named = rtt_config(u.prs_board);
                if (!named) continue;
                const BuildTask* c = &*named;
                StreamPlan p;
                p.kind = semi ? "semihosting" : "rtt";
                p.channel = semi ? -1 : static_cast<int>(ch);
                p.name = semi ? std::string("Board output") : "RTT " + std::to_string(ch) + (name.empty() ? std::string() : ": " + name);
                p.input = false;  // (probe-rs 0.32's DAP server takes nothing for a down-channel)
                u64 doc = 0;
                for (const auto& [d, t] : term_links())
                    if (t->is(c->name, p.kind, p.channel)) doc = d->id, t->rename(p.name), t->push_back();
                if (!doc) {
                    LinkSpec spec;
                    spec.board = c->name;
                    spec.plan = p;
                    spec.pushed = true;
                    spec.dir = build_root();
                    doc = term_open_link(std::move(spec), crash_options_of(c), false);
                }
                u.prs_tabs[ch] = doc;
                u.prs->request("rttWindowOpened", "{\"channelNumber\":" + std::to_string(ch) + ",\"windowIsOpen\":true}");
            } else if (m.name == "probe-rs-rtt-data") {
                const auto t = u.prs_tabs.find(b["channelNumber"].integer(-1));
                if (t == u.prs_tabs.end()) continue;
                for (const auto& [d, l] : term_links())
                    if (d->id == t->second) l->push(b["data"].str());
            } else if (m.name == "probe-rs-show-message" || (m.name == "output" && b["category"].str() == "important")) {
                std::string text(b["message"].exists() ? b["message"].str() : b["output"].str());
                while (!text.empty() && text.back() == '\n') text.pop_back();
                if (!text.empty()) notify("probe-rs: " + text, 1);
            }
        }
        if (u.prs->ended()) {
            std::string why = u.prs->errors();
            while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
            if (const size_t nl = why.rfind('\n'); nl != std::string::npos) why = why.substr(nl + 1);
            notify("probe-rs ended" + (why.empty() ? std::string() : ": " + why), 1);
            for (const auto& [ch, doc] : u.prs_tabs)
                for (const auto& [d, t] : term_links())
                    if (d->id == doc) t->push_gone("probe-rs ended");
            u.prs.reset();
        }
    }
    term_sync();
}

// The stream tab in front (a serial port's or a board stream's), else the
// one on show, else the only one open.
std::shared_ptr<StreamTab> App::stream_here(Doc** doc, bool say) {
    std::shared_ptr<StreamTab> s;
    Doc* at = nullptr;
    if (pane_->doc && (s = term_stream(*pane_->doc))) at = pane_->doc;
    for (const std::unique_ptr<Pane>& p : panes_)
        if (!s && p->doc && (s = term_stream(*p->doc))) at = p->doc;
    if (!s) {
        const auto serials = term_serials();
        const auto links = term_links();
        if (serials.size() + links.size() == 1) {
            if (!serials.empty()) at = serials.front().first, s = serials.front().second;
            else at = links.front().first, s = links.front().second;
        }
    }
    if (!s && say) notify("No serial or RTT tab is open");
    if (doc) *doc = at;
    return s;
}

// The run configuration of that name, as the folder has it now.
std::optional<BuildTask> App::rtt_config(const std::string& name) {
    for (const BuildTask& t : build_tasks())
        if (t.config && t.name == name) return t;
    return std::nullopt;
}

// A stream's tab brought to the front of its split (shown, if hidden).
void App::rtt_show(u64 doc) {
    Pane* p = nullptr;
    size_t i = 0;
    if (!find_doc(doc, &p, &i) && dock_show(pane_)) find_doc(doc, &p, &i);
    if (!p) return;
    Pane* const was = pane_;
    pane_ = p;
    tab_show(i);
    pane_ = was;
}

// (The tests: the link tabs, each "TITLE: dump", and the drivers' steps.)
std::string App::rtt_dump() {
    term_sync();
    std::string out;
    for (const auto& [d, t] : term_links()) out += (out.empty() ? "" : " | ") + d->title + ": " + t->dump();
    if (rttui_)
        for (const std::shared_ptr<RttDriver>& d : rttui_->drivers) {
            std::lock_guard lock(d->mu);
            static const char* const kStep[] = {"idle", "setup", "start", "channels", "waiting", "servers", "done"};
            out += std::string(out.empty() ? "" : " | ") + "driver " + d->board + " " + kStep[d->step] + " tries " + std::to_string(d->tries) + " opens " +
                   std::to_string(d->link ? d->link->opens() : 0);
        }
    return out.empty() ? "no rtt tab" : out;
}

}  // namespace blit
