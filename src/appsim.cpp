// Simulation runs and their waves (native-fpga.md 7). A task whose run is
// a simulation (its kind "simulation", or a command that runs Verilator,
// Icarus or SymbiYosys: simrun.hpp) has, as it ends, the dump it wrote
// found on a worker (here, or on the project's machine by its find: the
// run's output and command read first, then the folder it ran in), and the
// wave tab opens it beside the run's terminal, or reads it again if it is
// open. "Open the last run's waves" (gW in Normal mode) opens that dump
// again, or with no run yet the newest dump in the project.
#include <SDL3/SDL.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "fsio.hpp"
#include "remote.hpp"
#include "simrun.hpp"

namespace blit {

namespace {

// (The wave tab's event: wave_sync takes what was found.)
void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 60;
    SDL_PushEvent(&ev);
}

i64 wall_now() { return static_cast<i64>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

// A dump looked for: after a run in `dir` (what its output said and its
// command named; how long ago it started), or the project's newest.
struct Seek {
    std::string dir;
    std::vector<std::string> said, named, sources;
    i64 ago = -1;  // (-1: any time: the newest in the project)
    bool chord = false;
};
struct Found {
    std::string path;  // (whole: "" none)
    bool chord = false;
    std::string dir;
};

// What a look comes to: the dump's whole path, or "" (a worker's call:
// files read, here or there).
std::string look_for(const Seek& l) {
    std::string host, there;
    const bool remote = split_remote(l.dir, &host, &there);
    const std::string prefix = remote ? l.dir.substr(0, l.dir.size() - there.size()) : std::string();
    std::vector<SimDump> found;
    if (remote) {
        std::string out;
        if (!fs_run(l.dir, sim_find_command(l.ago), &out, 20'000)) return {};
        found = sim_find_read(out, l.ago);
    } else {
        found = sim_find_here(l.dir, l.ago >= 0 ? wall_now() - l.ago - 2 : 0);
    }
    std::vector<std::string> named = l.named;
    for (const std::string& s : l.sources) {
        std::string text;
        if (fs_read(s.starts_with('/') ? prefix + s : l.dir + "/" + s, &text, 4u << 20))
            for (std::string& n : sim_dumpfiles(text)) named.push_back(std::move(n));
    }
    const std::string chosen = sim_choose(found, l.said, named);
    if (!chosen.empty()) return l.dir + "/" + chosen;
    // (A dump the output names outside the folder, by its whole path: here, if it was written during the run.)
    if (!remote)
        for (const std::string& s : l.said) {
            struct stat st{};
            if (s.starts_with('/') && ::stat(s.c_str(), &st) == 0 && (l.ago < 0 || st.st_mtime >= wall_now() - l.ago - 2)) return s;
        }
    return {};
}

}  // namespace

struct App::SimUi {
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<Seek> seeks;    // (mu)
    std::vector<Found> found;  // (mu)
    bool busy = false;         // (mu) a look being made
    std::string last;          // the dump the last run wrote (its whole path)
    std::vector<std::pair<u64, Seek>> ended;  // runs ended, by their terminals, their output still to be read
    std::jthread worker;       // (last: it stops first)

    SimUi() {
        worker = std::jthread([this](std::stop_token st) {
            for (;;) {
                Seek l;
                {
                    std::unique_lock lock(mu);
                    cv.wait(lock, st, [&] { return !seeks.empty(); });
                    if (st.stop_requested()) return;
                    l = std::move(seeks.front());
                    seeks.pop_front();
                    busy = true;
                }
                Found f{look_for(l), l.chord, l.dir};
                {
                    std::lock_guard lock(mu);
                    found.push_back(std::move(f));
                    busy = false;
                }
                wake();
            }
        });
    }
    ~SimUi() {
        worker.request_stop();
        cv.notify_all();
    }
    void ask(Seek l) {
        {
            std::lock_guard lock(mu);
            seeks.push_back(std::move(l));
        }
        cv.notify_all();
    }
};

// A run of a task has ended (build_ended): a simulation's dump looked for,
// once what its terminal says can be read (sim_sync: its terminal is
// being looked at now).
void App::sim_ended(u64 term, const std::string& dir, const std::string& cmd, i64 started) {
    if (!simui_) simui_ = std::make_shared<SimUi>();
    Seek l;
    l.dir = dir;
    sim_named(cmd, &l.named, &l.sources);
    l.ago = std::max<i64>(0, wall_now() - started);
    simui_->ended.emplace_back(term, std::move(l));
    wake();
}

// What the worker found: a run's dump opened, or read again; the chord's
// (with no run yet) opened, or said to be none.
void App::sim_sync() {
    if (!simui_) return;
    for (auto& [term, l] : std::exchange(simui_->ended, {})) {
        TermLines lines;
        if (const Doc* d = find_doc(term); d && d->term) term_lines(*d, &lines);
        l.said = sim_dumps_said(lines.text);
        simui_->ask(std::move(l));
    }
    std::vector<Found> found;
    {
        std::lock_guard lock(simui_->mu);
        found.swap(simui_->found);
    }
    for (const Found& f : found) {
        if (f.path.empty()) {
            if (f.chord) notify("No waves (.vcd or .fst) in " + f.dir);
            continue;
        }
        if (!f.chord) simui_->last = f.path;
        wave_show(f.path, f.chord, !f.chord);
    }
}

// "Open the last run's waves": the dump the last simulation run wrote, or
// the newest in the project.
void App::sim_last_waves() {
    if (!simui_) simui_ = std::make_shared<SimUi>();
    if (!simui_->last.empty()) {
        wave_show(simui_->last, true, false);
        return;
    }
    const std::string root = build_root();
    if (root.empty()) return notify("Open a folder first");
    Seek l;
    l.dir = root;
    l.chord = true;
    simui_->ask(std::move(l));
}

bool App::sim_busy() const {
    if (!simui_) return false;
    if (!simui_->ended.empty()) return true;
    std::lock_guard lock(simui_->mu);
    return simui_->busy || !simui_->seeks.empty() || !simui_->found.empty();
}

}  // namespace blit
