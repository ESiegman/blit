#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "app.hpp"
#include "remote.hpp"

namespace {

void usage() {
    std::puts(
        "usage: blit [options] [file]\n"
        "       blit [options] ssh://host/path    (a file or a folder on another machine)\n"
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

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    // Run by ssh to ask a question (a password, a passphrase): the blit that
    // started that ssh shows it, and the answer is printed here (remote.cpp).
    if (const char* sock = std::getenv("BLIT_ASKPASS_SOCKET"); sock && *sock && argc == 2) return blit::remote_askpass(sock, argv[1]);
#endif
    blit::Options opt;
    opt.t_main = blit::now_ns();
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
            opt.file = a;
        }
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
