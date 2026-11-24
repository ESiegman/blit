// "Capture a frame": under a Metal capture preset (run.json's environments,
// build.hpp), a frame of the program the Run button runs, taken by Apple's
// gpucapture (`gpucapture start -p PID -o FILE`, which a program run with
// MTL_CAPTURE_ENABLED=1 lets it do; `gpucapture list` names those that
// can be), into the folder's .blit/captures, on a worker. (The Capture tab
// opens what it took, when it comes.)
#include <SDL3/SDL.h>

#include <ctime>
#include <mutex>
#include <optional>
#include <thread>

#include "app.hpp"
#include "fsio.hpp"
#include "missing.hpp"
#include "remote.hpp"

namespace blit {

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 71;
    SDL_PushEvent(&ev);
}

}  // namespace

struct App::CaptureUi {
    std::mutex mu;
    std::optional<std::string> said;  // (mu) what came of it, to say
    bool busy = false;
    std::jthread worker;  // (last: joined first)
};

void App::capture_frame() {
    const RunEnvironment* e = run_environment();
    if (!e || e->capture != "metal") return notify("The environment chosen in the Build tab is not a Metal capture (\"capture\": \"metal\")");
    const std::string root = build_root();
    if (is_remote(root)) return notify("Metal's captures are of programs on this Mac");
    const i64 shell = run_config_pid();
    if (shell <= 0) return notify("The program is not running: Run it first, in this environment");
    if (!captureui_) captureui_ = std::make_shared<CaptureUi>();
    CaptureUi& u = *captureui_;
    if (u.busy) return;
    if (u.worker.joinable()) u.worker.join();
    // Named for the configuration and the time.
    std::string name;
    if (const BuildTask* c = run_config())
        for (const char ch : c->name) name += std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ? ch : '-';
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y%m%d-%H%M%S", std::localtime(&now));
    const std::string file = std::string(kCaptures) + "/" + name + "-" + when + ".gputrace";
    // The program: the one gpucapture can capture whose parents lead to the run's shell.
    const std::string sh = "command -v gpucapture >/dev/null 2>&1 || { echo blit-no-gpucapture; exit 1; }; "
                           "for p in $(gpucapture list 2>/dev/null | awk 'NR > 1 { print $1 }'); do q=$p; "
                           "while [ \"${q:-0}\" -gt 1 ]; do if [ \"$q\" = " + std::to_string(shell) + " ]; then "
                           "mkdir -p " + sh_word(kCaptures) + " && exec gpucapture start -p \"$p\" -o " + sh_word(file) + " 2>&1; fi; "
                           "q=$(ps -o ppid= -p \"$q\" | tr -d ' '); done; done; echo blit-no-capturable; exit 1";
    u.busy = true;
    notify("Capturing a frame\xE2\x80\xA6");
    u.worker = std::jthread([&u, root, sh, file] {
        std::string out;
        const bool ok = fs_run(root, sh, &out, 60'000);
        std::string said;
        if (out.find("blit-no-gpucapture") != std::string::npos) said = not_installed("gpucapture", "");
        else if (out.find("blit-no-capturable") != std::string::npos) said = "gpucapture does not list the program: it has no Metal device yet";
        else if (ok && out.find("capture finished") != std::string::npos) said = "Captured " + file;
        else {  // (in its own words: its last line)
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            said = "gpucapture could not capture it" + (out.empty() ? std::string() : ": " + out.substr(out.rfind('\n') == std::string::npos ? 0 : out.rfind('\n') + 1));
        }
        {
            std::lock_guard lock(u.mu);
            u.said = std::move(said);
        }
        wake();
    });
}

void App::capture_sync() {
    if (!captureui_) return;
    CaptureUi& u = *captureui_;
    std::optional<std::string> said;
    {
        std::lock_guard lock(u.mu);
        said.swap(u.said);
    }
    if (!said) return;
    u.busy = false;
    notify(*said, said->starts_with("Captured ") ? 0 : 1);
}

void App::capture_settle() {
    const u64 until = now_ns() + 60'000'000'000ull;
    while (captureui_ && captureui_->busy && now_ns() < until) {
        capture_sync();
        SDL_Delay(5);
    }
}

}  // namespace blit
