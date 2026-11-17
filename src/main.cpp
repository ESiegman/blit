#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app.hpp"
#include "instance.hpp"
#include "loginpath.hpp"
#include "remote.hpp"
#if defined(__APPLE__)
#include "macapp.h"
#endif

namespace {

void usage() {
    std::puts(
        "usage: blit [options] [file...]\n"
        "       blit [options] file:LINE[:COL]    (a file at a line, as compilers and grep write it)\n"
        "       blit [options] folder             (a folder to work in: its session, if it has one)\n"
        "       blit [options] ssh://host/path    (a file or a folder on another machine)\n"
        "With a blit already running, the files open in its window and this one goes.\n"
        "  -n, --new          a blit of its own, even with one running\n"
        "  -w, --wait         wait until the files' tabs are closed (git's editor: blit -w)\n"
        "  --config FILE      init.lua to run: settings, mappings, plugins (default: ~/.config/blit/init.lua)\n"
        "  --font NAME        font family or font file; overrides the config\n"
        "  --size PT          font size in points; overrides the config\n"
        "  --line N           open at line N\n"
        "  --debug-gpu        enable GPU validation (Metal's debug layer)\n"
        "  --info             print the settings and font in use, then exit\n"
        "  --grammars         list the tree-sitter grammars blit can install\n"
        "  --install-grammar NAME[,NAME...]   fetch and build grammars (\"all\" for every one)"
#if BLIT_DEV
        "\n"
        "benchmarks and tests (Debug builds only):\n"
        "  --bench NAME       startup | scroll | select | jump | hscroll | type | assist | popups | pace | keypress | motionkeys | idle\n"
        "  --frames N         length of the benchmark\n"
        "  --offscreen WxH    render to a texture, no window\n"
        "  --scale S          pixels per point (default: from the display)\n"
        "  --shot FILE.bmp    render one frame to an image and exit\n"
        "  --script EVENTS    input to replay before --shot, e.g. \"text:hi;key:shift+Left;wheel:-2\"\n"
        "  --select-all       start with everything selected\n"
        "  --fps-cap HZ       hold offscreen drawing to HZ frames a second (a window is held to its display's rate anyway)"
#endif
    );
}

// file:LINE and file:LINE:COL, as compilers and grep print them, when no
// file has that whole name.
blit::OpenAsk place_of(const char* a) {
    blit::OpenAsk f;
    f.path = a;
#if !defined(_WIN32)
    if (blit::is_remote(f.path) || ::access(a, F_OK) == 0) return f;
    auto number_at_end = [](std::string& s, blit::u64* out) {
        const size_t colon = s.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == s.size()) return false;
        blit::u64 v = 0;
        for (size_t i = colon + 1; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + static_cast<blit::u64>(s[i] - '0');
        }
        s.resize(colon);
        *out = v;
        return true;
    };
    std::string rest = f.path;
    blit::u64 first = 0, second = 0;
    if (!number_at_end(rest, &first)) return f;
    if (number_at_end(rest, &second)) {  // file:LINE:COL
        f.line = second;
        f.col = first;
    } else {
        f.line = first;
    }
    f.path = rest;
#endif
    return f;
}

#if !defined(_WIN32)
// A blit apart from this terminal (its own session, no output here), for
// `blit -w` when none was running: the files go to it, and this one waits.
bool start_apart() {
    const std::string self = blit::program_path();
    if (self.empty()) return false;
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        if (const int null = ::open("/dev/null", O_RDWR); null >= 0) {
            ::dup2(null, 0);
            ::dup2(null, 1);
            ::dup2(null, 2);
        }
        ::execl(self.c_str(), self.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
}

void bring_front() {
#if defined(__APPLE__)
    blit_mac_front_bundle(blit::program_path().c_str());
#endif
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    // Run by ssh to ask a question (a password, a passphrase): the blit that
    // started that ssh shows it, and the answer is printed here (remote.cpp).
    if (const char* sock = std::getenv("BLIT_ASKPASS_SOCKET"); sock && *sock && argc == 2) return blit::remote_askpass(sock, argv[1]);
#endif
    blit::Options opt;
    opt.t_main = blit::now_ns();
    std::vector<blit::OpenAsk> named;
    bool alone = false, wait = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "blit: %s needs a value\n", a);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--config")) SDL_setenv_unsafe("BLIT_INIT", value(), 1);  // (init.lua is found by that name everywhere)
        else if (!std::strcmp(a, "--font")) opt.font = value();
        else if (!std::strcmp(a, "--size")) opt.pt = static_cast<float>(std::atof(value()));
        else if (!std::strcmp(a, "--line")) opt.goto_line = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(a, "--col")) opt.goto_col = static_cast<blit::u32>(std::atoi(value()));
        else if (!std::strcmp(a, "--debug-gpu")) opt.debug = true;
        else if (!std::strcmp(a, "--info")) opt.info = true;
        else if (!std::strcmp(a, "--grammars")) opt.list_grammars = true;
        else if (!std::strcmp(a, "--install-grammar")) {
            std::string_view names = value();
            while (!names.empty()) {
                const size_t comma = names.find(',');
                if (comma != 0) opt.install.emplace_back(names.substr(0, comma));
                names = comma == std::string_view::npos ? std::string_view{} : names.substr(comma + 1);
            }
        }
#if BLIT_DEV
        else if (!std::strcmp(a, "--bench")) opt.bench = value();
        else if (!std::strcmp(a, "--frames")) opt.frames = std::atoi(value());
        else if (!std::strcmp(a, "--offscreen")) std::sscanf(value(), "%dx%d", &opt.off_w, &opt.off_h);
        else if (!std::strcmp(a, "--scale")) opt.scale = static_cast<float>(std::atof(value()));
        else if (!std::strcmp(a, "--shot")) opt.shot = value();
        else if (!std::strcmp(a, "--script")) opt.script = value();
        else if (!std::strcmp(a, "--select-all")) opt.select_all = true;
        else if (!std::strcmp(a, "--frames-in-flight")) opt.frames_in_flight = std::atoi(value());
        else if (!std::strcmp(a, "--present")) opt.present = value();
        else if (!std::strcmp(a, "--launch-wall")) opt.launch_wall = std::atof(value());
        else if (!std::strcmp(a, "--fps-cap")) opt.fps_cap = static_cast<float>(std::atof(value()));
#endif
        else if (!std::strcmp(a, "-n") || !std::strcmp(a, "--new")) alone = true;
        else if (!std::strcmp(a, "-w") || !std::strcmp(a, "--wait")) wait = true;
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (!std::strncmp(a, "-NS", 3) || !std::strncmp(a, "-Apple", 6)) {
            ++i;  // macOS launch services pass "-NSSomething value" pairs; not ours
        } else if (a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "blit: unknown option %s\n", a);
            usage();
            return 2;
        } else {
            named.push_back(place_of(a));
        }
    }
    if (opt.goto_line > 0 && !named.empty() && named[0].line == 0) named[0].line = opt.goto_line;

#if !defined(_WIN32)
    // A blit of this program already running takes the files, unless this
    // run is one of the tools (or a test, or asked to be a blit of its own).
    bool tool = opt.info || opt.list_grammars || !opt.install.empty();
#if BLIT_DEV
    tool = tool || opt.off_w > 0 || opt.bench || opt.shot || opt.script;
#endif
    if (!tool && !alone) {
        blit::InstanceAsk ask;
        ask.wait = wait && !named.empty();
        for (const blit::OpenAsk& f : named) ask.files.push_back({blit::is_remote(f.path) ? f.path : blit::full_path(f.path), f.line, f.col});
        int rc = blit::instance_hand_over(ask, bring_front);
        if (rc >= 0) return rc;
        if (ask.wait && start_apart()) {
            for (int tries = 0; tries < 200 && rc < 0; ++tries) {  // (ten seconds for it to start listening)
                ::usleep(50'000);
                rc = blit::instance_hand_over(ask, bring_front);
            }
            if (rc >= 0) return rc;
        }
    }
#endif
#if defined(__APPLE__)
    if (!tool) blit::adopt_login_path();  // (started from the Dock: a login shell's PATH)
#endif
    static std::string first;  // (opt.file points into it)
    if (!named.empty()) {
        first = named[0].path;
        opt.file = first.c_str();
        if (named[0].line > 0) opt.goto_line = named[0].line;
        opt.more.assign(named.begin() + 1, named.end());
    }
    if (opt.pt < 4 || opt.pt > 200) opt.pt = 0;  // unset or nonsense: the config decides
    if (opt.info && opt.off_w == 0) opt.off_w = opt.off_h = 64;  // no window needed
    int rc;
    {
        blit::App app;
        rc = app.run(opt);
    }
    SDL_Quit();
    return rc;
}
