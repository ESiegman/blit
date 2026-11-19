#include "app.hpp"
#include "notebook.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "picture.h"
#include "qos.hpp"
#include "remote.hpp"
#endif

namespace blit {

namespace {

constexpr u64 kPreciseWindowNs = 200'000'000;

// What a new init.lua starts as: every setting, commented out.
const char kInitTemplate[] = {
#embed "../init.example.lua" suffix(, )
    0};
constexpr float kThumbOpacity = 0.55f;

constexpr SDL_Keymod kCmd = kPrimaryMod;
constexpr SDL_Keymod kWord = kWordMod;
#if defined(__APPLE__)
constexpr bool kMac = true;
#else
constexpr bool kMac = false;
#endif

Theme make_theme(const ThemeColors& c) {
    auto v = [](const Rgb& x, float a) { return std::array<float, 4>{x.r, x.g, x.b, a}; };
    Theme t{};
    std::ranges::copy(v(c.background, 1), t.bg);
    std::ranges::copy(v(c.foreground, 1), t.fg);
    std::ranges::copy(v(c.selection, 1), t.sel);
    std::ranges::copy(v(c.cursor, 1), t.cursor);
    std::ranges::copy(v(c.scrollbar, kThumbOpacity), t.thumb);
    std::ranges::copy(v(c.foreground, 1), t.palette[0]);
    std::ranges::copy(v(c.popup, 1), t.popup_bg);
    std::ranges::copy(v(c.popup_border, 1), t.popup_border);
    std::ranges::copy(v(c.error, 1), t.palette[kInkDiagnostic]);
    std::ranges::copy(v(c.warning, 1), t.palette[kInkDiagnostic + 1]);
    std::ranges::copy(v(c.info, 1), t.palette[kInkDiagnostic + 2]);
    std::ranges::copy(v(c.hint, 1), t.palette[kInkDiagnostic + 3]);
    std::ranges::copy(v(c.muted, 1), t.palette[kInkMuted]);
    std::ranges::copy(v(c.accent, 1), t.palette[kInkAccent]);
    std::ranges::copy(v(c.accent, 0.22f), t.tint);
    // Under the mouse in a popup: a little of the way from its background to the text's colour.
    const Rgb& pb = c.popup;
    std::ranges::copy(std::array<float, 4>{pb.r + (c.foreground.r - pb.r) * 0.09f, pb.g + (c.foreground.g - pb.g) * 0.09f, pb.b + (c.foreground.b - pb.b) * 0.09f, 1}, t.hover);
    // A shadow has to be darker to show on a dark background than on a light one.
    t.shadow = 0.299f * c.background.r + 0.587f * c.background.g + 0.114f * c.background.b < 0.5f ? 0.6f : 0.24f;
    // Lines added and changed since the last commit are in the theme's
    // colours for diffs where it has them.
    auto same = [](const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
    const Rgb plus = c.syntax_color("diff.plus"), delta = c.syntax_color("diff.delta");
    std::ranges::copy(v(same(plus, c.foreground) ? c.info : plus, 1), t.palette[kInkAdded]);
    std::ranges::copy(v(same(delta, c.foreground) ? c.warning : delta, 1), t.palette[kInkChanged]);
    for (u32 i = 0; i < kInkGuides && !c.guides.empty(); ++i)
        std::ranges::copy(v(c.guides[i % c.guides.size()], 1), t.palette[kInkGuide + i]);
    return t;
}

// The last change time of a file, or 0 if it does not exist.
i64 file_stamp(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) ? static_cast<i64>(info.modify_time) : 0;
}

void print_warnings(const char* about, const std::string& warnings) {
    size_t at = 0;
    while (at < warnings.size()) {
        const size_t nl = warnings.find('\n', at);
        const std::string line = warnings.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!line.empty()) std::fprintf(stderr, "blit: %s: %s\n", about, line.c_str());
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
}

#if !defined(_WIN32)
// Ctrl-C and `kill` should close the editor like the window's close button
// does, including the "save changes?" question. The idle loop is blocked in
// the OS, so a signal has to wake it: the handler writes a byte to a pipe
// (all a signal handler may safely do) and a small thread turns that into a
// quit event.
int g_signal_pipe[2] = {-1, -1};

void on_quit_signal(int) {
    const char byte = 1;
    [[maybe_unused]] ssize_t n = write(g_signal_pipe[1], &byte, 1);
}

void watch_quit_signals() {
    if (pipe(g_signal_pipe) != 0) return;
    std::thread([] {
        char byte;
        while (read(g_signal_pipe[0], &byte, 1) == 1) {
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
        }
    }).detach();
    struct sigaction sa{};
    sa.sa_handler = on_quit_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}
#else
void watch_quit_signals() {}
#endif

}  // namespace

std::string_view init_template() { return {kInitTemplate, sizeof kInitTemplate - 1}; }

// ---------------------------------------------------------------- setup

int App::run(const Options& opt) {
    opt_ = opt;
    if (opt_.list_grammars || !opt_.install.empty()) {
        // No window for these: the settings (init.lua may say where a
        // grammar comes from), then the job, in the terminal.
        cfg_ = make_config();
        init_path_ = default_init_path();
        script_start();
        const std::vector<GrammarSource> list = grammar_list(cfg_.grammars);
        if (!opt_.install.empty()) return install_grammars_command(list, opt_.install);
        print_grammars(list);
        return 0;
    }
    std::string err;
    if (!init(&err)) {
        std::fprintf(stderr, "blit: %s\n", err.c_str());
        if (win_) tell(SDL_MESSAGEBOX_ERROR, err.c_str());
        return 1;
    }
    if (opt_.info) {
        const bool dark = App::dark();
        std::printf("init.lua    %s%s\n", init_path_.c_str(), file_stamp(init_path_) ? "" : "  (not there: the defaults)");
        std::printf("theme       %s  (dark: %s, light: %s)\n", (dark ? cfg_.theme_dark : cfg_.theme_light).c_str(),
                    cfg_.theme_dark.c_str(), cfg_.theme_light.c_str());
        std::printf("font        %s\n            %s\n", font_->family().c_str(), font_->path().c_str());
        std::printf("size        %g pt = %g px at scale %g, cell %d x %d px\n", pt_, font_->px(), scale_, font_->cell_w(),
                    font_->cell_h());
        std::printf("ligatures   %s", font_->ligatures() ? "on, between: " : cfg_.ligatures ? "asked for, but unavailable" : "off");
        std::printf("%s\n", font_->joining_characters().c_str());
        static const char* const style_names[] = {"", "italic", "bold", "bold italic"};
        for (u32 fs = 1; fs < Font::kFaces; ++fs)
            if (font_->has_style(fs)) std::printf("%-11s %s\n", style_names[fs], font_->style_source(fs).c_str());
        settle_syntax();
        const std::string lang = Syntax::language_for(pane_->doc->ed.path, "", cfg_.grammars);
        std::printf("syntax      %s\n", pane_->doc->syntax.active() ? (pane_->doc->syntax.language() + ", " + std::to_string(pane_->doc->syntax.style_names().size() - 1) + " kinds of token").c_str()
                                        : !cfg_.syntax       ? "off"
                                        : pane_->doc->ed.path.empty()   ? "no file"
                                        : lang.empty()       ? "unknown file type"
                                        : find_grammar(grammar_list(cfg_.grammars), lang)
                                              ? ("no grammar installed for " + lang + "; `blit --install-grammar " + lang + "` gets it").c_str()
                                              : ("no grammar installed for " + lang).c_str());
        std::string program;
        const std::vector<LspServer> servers = lsp_server_list(cfg_.lsp_servers);
        const LspServer* server = cfg_.lsp && !pane_->doc->ed.path.empty() ? Lsp::pick(servers, lang, &program) : nullptr;
        std::printf("lsp         %s\n", server             ? (server->name + "  (" + program + ")").c_str()
                                        : !cfg_.lsp          ? "off"
                                        : pane_->doc->ed.path.empty()   ? "no file"
                                        : lang.empty()       ? "unknown file type"
                                                             : ("no language server installed for " + lang).c_str());
        std::printf("gpu         %s\n", gpu_.driver());
        const char* readers = blit_picture_readers();
        std::printf("pictures    %s\n", *readers ? readers : "none on this system yet");
        return 0;
    }
#if BLIT_DEV
    if (opt_.shot) return run_shot();
    if (opt_.bench) return run_bench();
#endif
    watch_quit_signals();
    loop();
    instance_stop();  // (those waiting on their tabs are told)
    session_save();  // (written on its own thread, which is waited for as blit goes)
    script_stop();  // (VimLeavePre for the scripts' handlers)
    return 0;
}

bool App::init(std::string* err) {
    cfg_ = make_config();  // the defaults; init.lua sets the rest
    init_path_ = default_init_path();
    const u64 t_script = now_ns();
    script_start();  // init.lua and the plugins, if there are any: settings, keys, commands (applua.cpp)
    if (script_) t_script_ns_ = now_ns() - t_script;
    session_create();  // (the start screen's Recent: how many tabs each folder brings back, read meanwhile)
    pane_->doc->ed.tab_width = cfg_.tab_width;
    pt_ = opt_.pt > 0 ? opt_.pt : cfg_.font_size;

    // The file loads on its own thread while the window system starts up.
    std::string file_err;
    bool file_ok = true;
    std::jthread loader;
    remote_start();  // (a file on another machine is opened once the window is up)
    prof_start();
    debug_create();
    build_create();
    test_create();
    commands_create();
    jumps_create();
    // (A file on another machine, a picture or a PDF is opened once the window is up.)
    const bool later_file = opt_.file && (is_remote(opt_.file) || is_picture_path(opt_.file) || is_pdf_path(opt_.file));
    bool folder = false;  // blit DIR: a folder to work in, and its session (see appsession.cpp)
    if (opt_.file && !later_file)
        loader = std::jthread([&] {
            if (struct stat st; ::stat(opt_.file, &st) == 0 && S_ISDIR(st.st_mode)) {  // (a folder reads as an empty file)
                folder = true;
                return;
            }
            file_ok = pane_->doc->ed.open(opt_.file, &file_err);
            // Parsing starts right away, so colours are usually ready before
            // the window is.
            if (file_ok) start_syntax(opt_.goto_line > 0 ? opt_.goto_line - 1 : 0, true);
        });

    // A flick on a touchpad should glide to a stop, as it does everywhere
    // else on a Mac. SDL turns that off unless asked.
    SDL_SetHint(SDL_HINT_MAC_SCROLL_MOMENTUM, cfg_.scroll_momentum ? "1" : "0");
    const bool offscreen = opt_.off_w > 0 && opt_.off_h > 0;
#if !defined(__APPLE__)
    // A run with no window (the tests, a benchmark) needs no display, and
    // may be where there is none (over ssh): SDL's driver without one.
    if (offscreen) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        *err = std::string("SDL failed to start: ") + SDL_GetError();
        return false;
    }
    t_sdl_ = now_ns();

    // The font is found, loaded and rasterized on another thread while the
    // window is being created. That needs the display scale before there is
    // a window, so it is guessed from the main display and checked later.
    float guess = 2.0f;
    if (!offscreen) {
        // What SDL_GetWindowDisplayScale will report for a window there.
        const SDL_DisplayID display = SDL_GetPrimaryDisplay();
        guess = SDL_GetDisplayContentScale(display);
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display)) guess *= mode->pixel_density;
    }
    if (opt_.scale > 0) guess = opt_.scale;
    if (guess <= 0) guess = 1;
    std::unique_ptr<Font> early;
    std::string early_warnings;
    std::jthread font_thread([&] {
        auto f = std::make_unique<Font>();
        std::string ignored;
        if (f->init(font_spec(guess), &ignored, &early_warnings)) early = std::move(f);
    });

    fx_live_ = !offscreen;  // (a scripted run draws things where they end up)
    if (!offscreen) {
#if defined(__APPLE__)
        const SDL_WindowFlags gpu_flag = SDL_WINDOW_METAL;
#else
        const SDL_WindowFlags gpu_flag = SDL_WINDOW_VULKAN;
#endif
        win_ = SDL_CreateWindow("blit", cfg_.window_width, cfg_.window_height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | gpu_flag);
        if (!win_) {
            *err = std::string("cannot create window: ") + SDL_GetError();
            return false;
        }
    }
    t_window_ = now_ns();
    if (!gpu_.init(win_, opt_.frames_in_flight, opt_.debug, err)) return false;
    t_gpu_ = now_ns();
    apply_present_mode();
    update_refresh();
    if (win_) {
        density_ = SDL_GetWindowPixelDensity(win_);
        scale_ = SDL_GetWindowDisplayScale(win_);
        SDL_GetWindowSizeInPixels(win_, &px_w_, &px_h_);
    } else {
        px_w_ = opt_.off_w;
        px_h_ = opt_.off_h;
        scale_ = density_ = 2;
        if (!gpu_.set_offscreen(px_w_, px_h_)) {
            *err = "cannot create offscreen target";
            return false;
        }
    }
    if (density_ <= 0) density_ = 1;
    if (scale_ <= 0) scale_ = 1;
    if (opt_.scale > 0) scale_ = opt_.scale;
    font_thread.join();
    if (early && early->px() == pt_ * scale_) {
        print_warnings("font", early_warnings);
        if (!adopt_font(std::move(early), err)) return false;
    } else if (!load_font(err)) {  // the guess was wrong, or loading failed: do it here
        return false;
    }
    t_font_ = now_ns();

    if (loader.joinable()) loader.join();
    if (!file_ok && !folder) {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(opt_.file, &info)) {
            *err = file_err;
            return false;
        }
        pane_->doc->ed.path = opt_.file;  // new file: created on first save
    }
    t_file_ = now_ns();
    if (!pane_->doc->ed.path.empty()) pane_->doc->disk_stamp = file_stamp(pane_->doc->ed.path);
    lsp_pending_ = !pane_->doc->ed.path.empty();  // a language server starts once the first frame is out
    grammars_pending_ = true;

    pick_theme();
    panel_apply_config(true);
    place_panes(px_w_, px_h_);  // so that the view has a size before its first frame
    if (opt_.goto_line > 0) {
        pane_->doc->ed.move_to(pane_->doc->ed.buf.line_start(opt_.goto_line - 1), false);
        pane_->target_y = pane_->scroll_y = static_cast<double>(opt_.goto_line - 1) * font_->cell_h();
    }
    pane_->target_x = pane_->scroll_x = static_cast<double>(opt_.goto_col) * font_->cell_w();
    if (opt_.select_all) pane_->doc->ed.select_all();
    if (win_) {
        SDL_StartTextInput(win_);
        SDL_AddEventWatch(event_watch, this);
    }
    start_init();  // (shown if no file was given)
    if (file_ok && !pane_->doc->ed.path.empty()) recent_add(pane_->doc->ed.path, false);
    script_started(!file_ok && !folder);
    if (later_file) open_path(opt_.file);
    if (folder && !project_open(opt_.file) && !panel_px()) panel_toggle();  // (the folder, shown)
    for (const OpenAsk& f : opt_.more) open_place(f);
    instance_start();  // (files from a later `blit` come here)
    update_check(false);  // (a newer release, once a day: appupdate.cpp)
    return true;
}

// How finished frames reach the screen. Vsync is the safe default. On macOS
// a window is always composited in step with the display, and measured there
// "immediate" keeps a steady refresh-rate cadence where the vsync path drops
// frames, so windows use it and only fullscreen keeps vsync.
void App::apply_present_mode() {
    if (!win_) return;
    int mode = Renderer::kVsync;
    if (opt_.present) {
        const std::string_view pm = opt_.present;
        if (pm == "immediate") mode = Renderer::kImmediate;
        if (pm == "mailbox") mode = Renderer::kMailbox;
    } else {
#if defined(__APPLE__)
        if (!(SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN)) mode = Renderer::kImmediate;
#endif
    }
    if (!gpu_.set_present_mode(mode)) gpu_.set_present_mode(Renderer::kVsync);
}

// The font the settings ask for, at a given display scale. --font and --size
// on the command line win over the config file.
FontSpec App::font_spec(float scale) const {
    FontSpec s;
    s.family = opt_.font ? opt_.font : cfg_.font_family;
    s.style = cfg_.font_style;
    s.px = pt_ * scale;
    s.line_height = cfg_.line_height;
    s.ligatures = cfg_.ligatures;
    s.features = cfg_.font_features;
    s.styles = cfg_.font_styles();  // the italic and bold faces the themes use
#if BLIT_DEV
    // The tests' cell, whatever fonts the machine has (CMakeLists.txt sets it
    // where macOS's own font does not give it).
    if (const char* cell = std::getenv("BLIT_CELL"); cell && std::sscanf(cell, "%dx%d", &s.cell_w26, &s.cell_h26) != 2) s.cell_w26 = s.cell_h26 = 0;
#endif
    return s;
}

bool App::adopt_font(std::unique_ptr<Font> f, std::string* err) {
    if (!gpu_.set_font(*f, err)) return false;
    font_ = std::move(f);
    pad_x_ = std::min(font_->cell_w(), font_->cell_w() / 2 + 2);  // at most one cell, see layout()
    return true;
}

bool App::load_font(std::string* err) {
    auto f = std::make_unique<Font>();
    std::string warnings;
    if (!f->init(font_spec(scale_), err, &warnings)) return false;
    print_warnings("font", warnings);
    return adopt_font(std::move(f), err);
}

// Sets the colours from the current theme, including the ink palette and
// which palette entry and which italic or bold face each syntax style uses.
// A cell can pick one of 32 inks, of which 24 are for syntax; themes use far
// fewer distinct colours than that. Must run again after the font changes: the faces belong to it.
void App::pick_theme() {
    const ThemeColors& tc = cfg_.theme(dark());
    theme_ = make_theme(tc);
    ++theme_gen_;
    guide_inks_ = static_cast<u32>(std::min<size_t>(tc.guides.size(), kInkGuides));
    // The documents on show (the one in front in each split) share the
    // palette: each colour any of them uses gets one ink.
    u32 inks = 1;
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc) continue;
        Doc& d = *p->doc;
        if (d.marks) {  // a diff: added, removed, a hunk's heading, the rest of the headings
            d.style_bits[1] = kInkAdded << kCellInkShift;
            d.style_bits[2] = kInkDiagnostic << kCellInkShift;
            d.style_bits[3] = kInkAccent << kCellInkShift;
            d.style_bits[4] = kInkMuted << kCellInkShift;
            continue;
        }
        const std::vector<std::string>& names = d.syntax.style_names();
        d.styles_known = names.size();
        std::fill(std::begin(d.style_bits), std::end(d.style_bits), 0u);
        for (size_t s = 1; s < names.size() && s < 256; ++s) {
            // A bracket by its depth (the worker's "bracket.N"): the theme's
            // colour for that depth, else what punctuation is.
            TokenStyle style;
            if (names[s].starts_with("bracket.") && cfg_.syntax_brackets && !tc.brackets.empty()) style.color = tc.brackets[static_cast<size_t>(std::atoi(names[s].c_str() + 8)) % tc.brackets.size()];
            else style = tc.syntax_style(names[s].starts_with("bracket.") ? std::string_view("punctuation.bracket") : std::string_view(names[s]));
            const Rgb c = style.color;
            u32 face = cfg_.font_style_of(style);
            if (!font_->ensure_style(face)) face = 0;
            u32 ink = 0;
            while (ink < inks && (theme_.palette[ink][0] != c.r || theme_.palette[ink][1] != c.g || theme_.palette[ink][2] != c.b)) ++ink;
            if (ink == inks) {
                if (inks == kInkSyntaxEnd) ink = 0;  // out of inks: plain text
                else std::ranges::copy(std::array<float, 4>{c.r, c.g, c.b, 1}, theme_.palette[inks++]);
            }
            d.style_bits[s] = (ink << kCellInkShift) | (face << Font::kStyleShift);
        }
    }
    dirty_ = true;
}

// ---------------------------------------------------------------- syntax

// Begins highlighting the open file, if a grammar for it is installed.
// Nothing here waits: loading the grammar, parsing and querying all happen
// on the syntax worker.
void App::start_syntax(u64 first_line, bool at_launch) {
    if (!cfg_.syntax || (pane_->doc->ed.path.empty() && pane_->doc->lang.empty())) return;
    char head[128];
    const u64 n = pane_->doc->ed.buf.read(0, head, sizeof head);
    const std::string_view first(head, static_cast<size_t>(std::min<u64>(n, std::string_view(head, n).find('\n'))));
    SyntaxOptions so;
    so.parser_dirs = cfg_.parser_dirs;
    so.query_dirs = cfg_.query_dirs;
    so.max_bytes = static_cast<u64>(cfg_.syntax_max_mb * 1048576.0f);
    so.hold_full_parse = at_launch;  // released by the first frame
#if BLIT_DEV
    if (const char* mode = SDL_getenv("BLIT_SYNTAX_COMPILE"))  // tests: "whole" or "parts"
        so.compile = *mode == 'p' ? SyntaxOptions::kPartsOnly : SyntaxOptions::kWholeOnly;
#endif
    pane_->doc->syntax.set_notify([] {  // on the worker thread: wake the event loop
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        ev.user.code = 3;
        SDL_PushEvent(&ev);
    });
    const std::string& chosen = pane_->doc->lang;  // (one chosen from the status bar, over the file's name)
    if (!pane_->doc->syntax.attach(pane_->doc->ed.buf, chosen.empty() ? Syntax::language_for(pane_->doc->ed.path, first, cfg_.grammars) : chosen == "-" ? std::string() : chosen, so)) return;
    pane_->doc->ed.listen(&pane_->doc->syntax);
    pane_->doc->syntax.want(pane_->doc->ed.buf, first_line, first_line + 200);
}

// The language of the open file, by its name or first line; "" if unknown.
std::string App::file_language() const {
    if (!pane_->doc->lang.empty()) return pane_->doc->lang == "-" ? std::string() : pane_->doc->lang;  // (chosen from the status bar)
    if (pane_->doc->ed.path.empty()) return {};
    char head[128];
    const u64 n = pane_->doc->ed.buf.read(0, head, sizeof head);
    const std::string_view first(head, static_cast<size_t>(std::min<u64>(n, std::string_view(head, n).find('\n'))));
    return Syntax::language_for(pane_->doc->ed.path, first, cfg_.grammars);
}

// Asks for the grammars the settings want installed and that are not: the
// ones listed under [syntax] install and, with auto_install, the open
// file's. Fetching and building happen on the installer's thread;
// sync_grammars() hears when one is done.
void App::want_grammars() {
    if (!cfg_.syntax || (cfg_.syntax_install.empty() && !cfg_.syntax_auto_install)) return;
    const std::vector<GrammarSource> list = grammar_list(cfg_.grammars);
    std::vector<std::string> wanted = cfg_.syntax_install;
    if (cfg_.syntax_auto_install) {
        const std::string lang = file_language();
        if (!lang.empty() && find_grammar(list, lang)) wanted.push_back(lang);
    }
    bool asked = false;
    for (const std::string& name : wanted) {
        const GrammarSource* g = find_grammar(list, name);
        if (g && grammar_state(*g) == GrammarState::kInstalled) continue;
        if (!asked) {
            grammars_.set_notify([] {  // on the installer's thread: wake the event loop
                SDL_Event ev{};
                ev.type = SDL_EVENT_USER;
                ev.user.code = 5;
                SDL_PushEvent(&ev);
            });
            asked = true;
        }
        grammars_.request(list, name);
    }
}

void App::sync_grammars() {
    for (const GrammarInstaller::Done& d : grammars_.poll()) {
        if (!d.ok) {
            notify("The " + language_name(d.name) + " grammar was not installed: " + d.message, 2);
            continue;
        }
        notify("Installed the " + language_name(d.name) + " grammar");
        ++grammars_gen_;
        // An open file may have been waiting for it.
        for_each_doc([&] {
            if (d.name != file_language() || (pane_->doc->syntax.active() && pane_->doc->syntax.language() == d.name)) return;
            stop_syntax();
            start_syntax(static_cast<u64>(std::max(0.0, pane_->doc->scroll_y)) / static_cast<u64>(font_->cell_h()));
        });
        pick_theme();
    }
}

// Whether the file in front's language has no grammar installed (its text
// is plain for want of one). Looks at the disk: asked when the status bar
// is made again, not at every frame.
bool App::grammar_missing() {
    const Doc& d = *pane_->doc;
    if (!cfg_.syntax || d.syntax.active() || d.term || d.prof || d.md || d.image || d.pdf) return false;
    const std::string lang = file_language();
    if (lang.empty()) return false;
    SyntaxOptions so;
    so.parser_dirs = cfg_.parser_dirs;
    so.query_dirs = cfg_.query_dirs;
    return !Syntax::installed(lang, so);
}

// The grammar of the file in front's language, fetched and built by the
// installer on its thread, as the settings' syntax.install would have it;
// the bottom line says when it is in (sync_grammars), and the file is
// coloured then.
void App::grammar_install() {
    const std::string lang = file_language();
    if (lang.empty()) return notify("This file has no language");
    const std::string name = language_name(lang);
    const std::vector<GrammarSource> list = grammar_list(cfg_.grammars);
    const GrammarSource* g = find_grammar(list, lang);
    if (!g || g->repo.empty()) return notify("No grammar is known for " + name, 1);
    if (!grammar_missing() && grammar_state(*g) == GrammarState::kInstalled) return notify("The " + name + " grammar is installed");
    grammars_.set_notify([] {  // on the installer's thread: wake the event loop
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        ev.user.code = 5;
        SDL_PushEvent(&ev);
    });
    grammars_.forgive();  // (one that failed before may be tried again)
    grammars_.request(list, lang);
    notify("Installing the " + name + " grammar\xE2\x80\xA6");
}

// Must run before the buffer is replaced: the worker reads its memory.
void App::stop_syntax() {
    pane_->doc->ed.unlisten(&pane_->doc->syntax);
    pane_->doc->syntax.detach();
    pane_->doc->styles_known = 0;
}

// Once per frame: tell the worker what is on screen and take what it has
// finished. Both are a few comparisons when nothing changed.
void App::sync_syntax() {
    if (!pane_->doc->syntax.active()) return;
    const u64 top_row = static_cast<u64>(std::max(0.0, pane_->scroll_y)) / static_cast<u64>(font_->cell_h());
    const u64 top = line_of_row(top_row), bottom = line_of_row(top_row + static_cast<u64>(visible_rows()) + 1);
    pane_->doc->syntax.want(pane_->doc->ed.buf, top, bottom);
    if (pane_->doc->syntax.poll()) {
        const std::string msg = pane_->doc->syntax.take_message();
        if (!msg.empty()) std::fprintf(stderr, "blit: %s\n", msg.c_str());
        if (!pane_->doc->syntax.active()) pane_->doc->ed.unlisten(&pane_->doc->syntax);
        if (pane_->doc->syntax.style_names().size() != pane_->doc->styles_known) pick_theme();
        ++pane_->doc->hl_gen;
        dirty_ = true;
    }
}

// For screenshots, tests and benchmarks: wait until the colours for the
// current view are in.
void App::settle_syntax() {
    for_each_pane([this] {  // every split on show
        pane_->doc->syntax.release();
        for (int i = 0; i < 3 && pane_->doc->syntax.active(); ++i) {
            sync_syntax();
            pane_->doc->syntax.wait_idle();
        }
        sync_syntax();
    });
}

// Re-reads the config file if it changed on disk and applies what differs.
// New settings: whatever they change is made again (the font, colours,
// language servers, grammars, the side panel).
void App::apply_config(Config next) {
    const bool font_changed = next.font_family != cfg_.font_family || next.font_style != cfg_.font_style ||
                              next.font_size != cfg_.font_size || next.line_height != cfg_.line_height ||
                              next.ligatures != cfg_.ligatures || next.font_features != cfg_.font_features;
    const float old_size = cfg_.font_size;
    // Which file endings the settings give to which language: a change can
    // make an open file another language, for its colours and its server.
    auto endings = [](const Config& c) {
        std::vector<std::pair<std::string, std::vector<std::string>>> out;
        for (const GrammarSource& g : c.grammars)
            if (!g.extensions.empty()) out.emplace_back(g.name, g.extensions);
        return out;
    };
    const bool types_changed = endings(next) != endings(cfg_);
    const bool syntax_changed = next.syntax != cfg_.syntax || next.syntax_max_mb != cfg_.syntax_max_mb ||
                                next.parser_dirs != cfg_.parser_dirs || next.query_dirs != cfg_.query_dirs || types_changed;
    const bool grammars_changed = next.syntax_install != cfg_.syntax_install || next.syntax_auto_install != cfg_.syntax_auto_install ||
                                  next.grammars != cfg_.grammars;
    const bool lsp_changed =
        next.lsp != cfg_.lsp || next.lsp_max_mb != cfg_.lsp_max_mb || next.lsp_servers != cfg_.lsp_servers || types_changed;
    cfg_ = std::move(next);
    for (const std::unique_ptr<Pane>& p : panes_) {
        for (const std::shared_ptr<Doc>& d : p->docs) d->ed.tab_width = cfg_.tab_width;
        p->cache_rev = ~0ull;  // columns depend on the tab width
    }
    if (syntax_changed)
        for_each_doc([this] {
            stop_syntax();
            start_syntax(0);
        });
    if (lsp_changed) {
        stop_lsp();
        lsp_forgive();
        lsp_pending_ = true;
    }
    if (grammars_changed) {
        grammars_.forgive();
        grammars_pending_ = true;
    }
    panel_apply_config(false);
    if (font_changed) {
        if (cfg_.font_size != old_size && opt_.pt <= 0) pt_ = cfg_.font_size;
        const int old_cw = font_->cell_w(), old_ch = font_->cell_h();
        std::string err;
        if (load_font(&err)) rescale_scroll(old_cw, old_ch);
        else std::fprintf(stderr, "blit: %s\n", err.c_str());
    }
    pick_theme();
    dirty_ = true;
}

void App::zoom(float dpt) {
    const float old_pt = pt_;
    const int old_cw = font_->cell_w(), old_ch = font_->cell_h();
    pt_ = std::clamp(dpt == 0 ? (opt_.pt > 0 ? opt_.pt : cfg_.font_size) : pt_ + dpt, 6.0f, 96.0f);
    std::string err;
    if (!load_font(&err)) {
        pt_ = old_pt;
        return;
    }
    rescale_scroll(old_cw, old_ch);
    pick_theme();
    dirty_ = true;
}

// Scroll positions are in pixels: when the cells change size, every view
// keeps its place in the text (the rows stay whole).
void App::rescale_scroll(int old_cw, int old_ch) {
    if (old_cw <= 0 || old_ch <= 0) return;
    const double kx = static_cast<double>(font_->cell_w()) / old_cw;
    const int ch = font_->cell_h();
    for (const std::unique_ptr<Pane>& p : panes_) {
        p->scroll_y = p->target_y = std::floor(p->scroll_y / old_ch) * ch;
        p->scroll_x *= kx;
        p->target_x *= kx;
        for (auto& [id, at] : p->places) {
            at.scroll_y = std::floor(at.scroll_y / old_ch) * ch;
            at.scroll_x *= kx;
        }
    }
    for (Doc* d : open_docs()) {  // (once each, however many splits show it)
        d->scroll_y = std::floor(d->scroll_y / old_ch) * ch;
        d->scroll_x *= kx;
    }
}

// ---------------------------------------------------------------- files

// Shows a file in the focused split: in the tab there that has it; else,
// if another split shows it, as a second view of the same text (a picture,
// a PDF or the like is gone to where it is instead); else in a new tab next
// to the one in front (or in that one, if it is a new file nobody has
// typed in). False if it could not be opened; nothing has changed then.
bool App::open_path(const char* p) {
    const std::string full = full_path(p);
    Pane* where = nullptr;
    size_t have = 0;
    if (find_open(full, &where, &have)) {
        if (where != pane_ && shareable(*where->docs[have])) {
            show_also(where, have);
            return true;
        }
        focus_pane(where);
        tab_show(have);
        return true;
    }
    if (is_picture_path(full)) return image_open(full);  // a picture: in a tab of its own (appimage.cpp)
    if (is_pdf_path(full)) return pdf_open(full);        // a PDF too (apppdf.cpp)
    auto fresh = std::make_shared<Doc>();
    fresh->ed.tab_width = cfg_.tab_width;
    std::string err;
    const bool remote = is_remote(full);
    if (remote) {  // another machine's: its tab now, and the file when it comes (appremote.cpp)
        fresh->ed.path = full;
        fresh->loading = fresh->read_only = true;
    } else if (fresh->ed.open(p, &err)) {
        if (is_notebook_path(full)) nb_load(*fresh);  // (a notebook: the script of its cells)
    } else {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(p, &info)) {  // it is there, and cannot be read
            tell(SDL_MESSAGEBOX_ERROR, err.c_str());
            return false;
        }
        fresh->ed.path = p;  // a new file: created on first save
    }
    if (!remote && SDL_GetPathInfo(p, nullptr)) recent_add(p, false);  // (one that is there: a new one is not, yet)
    if (!remote) fresh->disk_stamp = file_stamp(p);
    fresh->id = next_doc_id_++;
    const bool reuse = pane_->doc->ed.path.empty() && !pane_->doc->ed.modified() && pane_->doc->ed.buf.size() == 0 && !pane_->doc->read_only;  // (not a profile's tab)
    size_t at = pane_->tab + 1;
    if (reuse) {
        stop_lsp();
        forget_doc(pane_->doc);  // (before it goes: nothing may still point at it)
        at = pane_->tab;
        pane_->docs[pane_->tab] = std::move(fresh);
        pane_->doc = nullptr;
    } else {
        pane_->docs.insert(pane_->docs.begin() + static_cast<std::ptrdiff_t>(at), std::move(fresh));
    }
    if (!pane_->doc) {  // the tab in front was replaced: bring its new contents up
        pane_->doc = pane_->docs[at].get();
        pane_->tab = at;
        pane_->scroll_x = pane_->scroll_y = pane_->target_x = pane_->target_y = 0;
        pane_->cache_rev = ~0ull;
        animating_ = false;
        lsp_pending_ = true;
        grammars_pending_ = true;
    }
    if (!remote) {
        // Colours start for it now, wherever it is.
        Doc* front = pane_->doc;
        pane_->doc = pane_->docs[at].get();
        start_syntax(0);
        pane_->doc = front;
    }
    tab_show(at);
    pick_theme();
    dirty_ = true;
    if (remote) remote_load(*pane_->docs[at], false);  // (the scripts hear of it when it is here)
    else if (script_) script_opened(pane_->doc->id, pane_->doc->disk_stamp == 0);  // (BufReadPost or BufNewFile, and FileType, at the next tick)
    return true;
}

// Whether the system is in its dark appearance. A Debug build's tests pin
// it with BLIT_APPEARANCE=dark or light, so that what they see does not
// depend on the machine's setting.
bool App::dark() const {
#if BLIT_DEV
    if (const char* a = std::getenv("BLIT_APPEARANCE"); a && *a) return std::strcmp(a, "light") != 0;
#endif
    return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT;
}

// A dialog with one thing to say.
void App::tell(SDL_MessageBoxFlags kind, const char* what) {
#if BLIT_DEV
    if (!win_) {  // a scripted run draws into a texture: nobody to show a dialog to
        std::fprintf(stderr, "blit: %s\n", what);
        return;
    }
#endif
    SDL_ShowSimpleMessageBox(kind, "blit", what, win_);
}

// Opens init.lua in the editor, making it from the commented template the
// first time (which changes nothing: every line of it is a comment). Saving
// it runs it again (see script_saved()).
void App::open_config() {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(init_path_.c_str(), &info)) {
        const size_t slash = init_path_.find_last_of("/\\");
        if (slash != std::string::npos) SDL_CreateDirectory(init_path_.substr(0, slash).c_str());
        if (!SDL_SaveFile(init_path_.c_str(), kInitTemplate, sizeof kInitTemplate - 1)) {
            tell(SDL_MESSAGEBOX_ERROR, SDL_GetError());
            return;
        }
    }
    open_path(init_path_.c_str());
}

void App::open_setting(const std::string& key) {
    remember_place();  // (Ctrl-O comes back from it)
    open_config();
    Editor& ed = pane_->doc->ed;
    if (pane_->doc->loading || full_path(ed.path) != full_path(init_path_)) return;
    // The last line that sets it (the one that counts), else the first
    // that is a comment of it, as the template has them.
    const std::string want = "blit.opt." + key;
    u64 live = ~0ull, commented = ~0ull;
    size_t live_at = 0, comment_at = 0, comment_name = 0;
    for (u64 line = 0; line < ed.buf.line_count(); ++line) {
        const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
        if (end - start > 4096) continue;
        const std::string t = ed.buf.text(start, end - start);
        size_t i = t.find_first_not_of(" \t");
        const size_t dashes = i;
        const bool comment = i != std::string::npos && t.compare(i, 2, "--") == 0;
        if (comment) i = t.find_first_not_of(" \t", i + 2);
        if (i == std::string::npos || t.compare(i, want.size(), want) != 0) continue;
        size_t eq = i + want.size();
        while (eq < t.size() && (t[eq] == ' ' || t[eq] == '\t')) ++eq;
        if (eq >= t.size() || t[eq] != '=' || (eq + 1 < t.size() && t[eq + 1] == '=')) continue;  // ("editor.tab_width_x", or a comparison)
        if (!comment) live = line, live_at = eq + 1;
        else if (commented == ~0ull) commented = line, comment_at = dashes, comment_name = i;
    }
    u64 line = live;
    if (line == ~0ull && commented != ~0ull) {  // the template's comment made a setting
        const u64 start = ed.buf.line_start(commented);
        ed.replace(start + comment_at, start + comment_name, "");
        line = commented;
    } else if (line == ~0ull) {  // a line of its own at the end, as it is now
        const u64 size = ed.buf.size();
        char last = '\n';
        if (size) ed.buf.read(size - 1, &last, 1);
        ed.replace(size, size, std::string(last == '\n' ? "" : "\n") + want + " = " + config_lua(config_get(cfg_, key)) + "\n");
        line = ed.buf.line_of(size + (last == '\n' ? 0 : 1));
    }
    // The value, from after the "=" to a comment or the end of the line.
    const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
    const std::string t = ed.buf.text(start, end - start);
    if (live == ~0ull) live_at = t.find('=') + 1;
    size_t v0 = t.find_first_not_of(" \t", live_at), v1 = t.size();
    if (v0 == std::string::npos) v0 = t.size();
    char quote = 0;
    for (size_t k = v0; k + 1 < t.size(); ++k) {
        if (quote) {
            if (t[k] == '\\') ++k;
            else if (t[k] == quote) quote = 0;
        } else if (t[k] == '"' || t[k] == '\'') {
            quote = t[k];
        } else if (t[k] == '-' && t[k + 1] == '-') {
            v1 = k;
            break;
        }
    }
    while (v1 > v0 && (t[v1 - 1] == ' ' || t[v1 - 1] == '\t')) --v1;
    show_place(line, v0);
    if (!cfg_.modal && v1 > v0) ed.move_to(start + v1, true);  // (selected: typing replaces it)
    dirty_ = true;
}

void App::save() {
    if (pane_->doc->read_only) return;  // a diff is not a file
    if (pane_->doc->ed.path.empty()) {  // a new file: a name for it is asked for (appfind.cpp), and it is saved when there is one
        find_open(kFindSave);
        find_save_for(pane_->doc->id);
        return;
    }
    const u64 id = pane_->doc->id;
    if (script_ && !script_write(false, id)) return;  // (BufWritePre: a handler may change it first, or close it)
    if (find_doc(id) != pane_->doc) {
        size_t at = 0;
        Pane* where = nullptr;
        if (!find_doc(id, &where, &at)) return;
        focus_pane(where);
        tab_show(at);
    }
    lsp_format_on_save();  // (lsp.format_on_save: its server formats it first)
    if (is_remote(pane_->doc->ed.path)) {  // to another machine: written on a thread, and saved once it has been (appremote.cpp)
        remote_save(*pane_->doc);
        return;
    }
    history_before_save(*pane_->doc);  // (the first save this session: the text on disk kept first)
    std::string err;
    const bool notebook = pane_->doc->nb || (is_notebook_path(pane_->doc->ed.path) && nb_new(*pane_->doc));
    const bool saved = notebook ? nb_save(*pane_->doc, &err) : pane_->doc->ed.save(&err);  // (a notebook: written as one)
    if (!saved) tell(SDL_MESSAGEBOX_ERROR, err.c_str());
    else if (lsp_.attached()) lsp_.did_save();
    else lsp_pending_ = true;  // a new file has a name now: a server can have it
    if (saved) pane_->doc->disk_stamp = file_stamp(pane_->doc->ed.path);
    if (cfg_.git) git_.refresh();  // git has something new to say about it
    const std::string path = pane_->doc->ed.path;
    if (saved) {
        if (path.ends_with(kRunConfigs)) build_changed();  // (the run configurations: the Build tab reads them again)
        script_saved(path);  // or one of the scripts (they run again)
        asm_saved(path);     // (its assembly made again)
        lens_saved();        // (and what refers to what counted again)
        history_saved(*pane_->doc);  // (a version kept in its local history)
        if (script_) script_write(true, id);
    }
    dirty_ = true;
}

// Cmd+Shift+S: the file in front, under a name asked for in the box that
// finds things, which starts at the name it has (save_named() saves it).
void App::save_as() {
    const Doc& d = *pane_->doc;
    if (d.read_only || d.term || d.prof || d.md || d.image || d.pdf) return;  // (no text of its own to write)
    std::string typed = d.ed.path;
    if (const char* home = SDL_getenv("HOME"); home && *home && !is_remote(typed)) {  // (shown as the box shows paths: ~ for the home folder)
        const std::string h = home;
        if (typed.starts_with(h + "/")) typed = "~" + typed.substr(h.size());
    }
    find_open(kFindSave, typed);
    find_save_for(d.id);
}

// Cmd+Option+S: every file with changes that has a name (a new one is left
// alone: each would ask for its own). The tab in front of each split stays
// in front.
void App::save_all() {
    std::vector<u64> wanted, fronts;
    for (const std::unique_ptr<Pane>& p : panes_) {
        fronts.push_back(p->doc ? p->doc->id : 0);
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (d->ed.modified() && !d->ed.path.empty() && !d->read_only && std::ranges::find(wanted, d->id) == wanted.end()) wanted.push_back(d->id);
    }
    if (wanted.empty()) return;
    const u64 focused = pane_->doc->id;
    Pane* where = nullptr;
    size_t at = 0;
    // (By id, not by place: a script's handler may close a tab or open one while a file is saved.)
    for (const u64 id : wanted)
        if (find_doc(id, &where, &at)) {
            focus_pane(where);
            tab_show(at);
            save();
        }
    for (const u64 id : fronts)
        if (find_doc(id, &where, &at)) {
            focus_pane(where);
            tab_show(at);
        }
    if (find_doc(focused, &where, &at)) {
        focus_pane(where);
        tab_show(at);
    }
}

// (Files being saved to another machine, and changed by nothing else, are
// not asked about: quitting waits for them, in a box that says so.)
void App::request_quit() {
    size_t unsaved = 0, unnamed = 0;
    std::vector<std::string> names;  // of the files with changes, in the order of their tabs
    Pane* first_pane = nullptr;
    size_t first = 0;
    auto unsaved_doc = [&](const Doc& d) { return d.ed.modified() && !remote_saving_all(d); };
    for (size_t k = panes_.size(); k-- > 0;)
        for (size_t i = panes_[k]->docs.size(); i-- > 0;)
            if (unsaved_doc(*panes_[k]->docs[i])) ++unsaved, first_pane = panes_[k].get(), first = i;
    // Programs still running in terminals end with blit: the question counts them.
    const size_t busy = term_busy_count();
    const std::string running = busy == 1 ? "1 program is still running in a terminal."
                                          : std::to_string(busy) + " programs are still running in terminals.";
    if (!unsaved) {
        if (busy && !confirm(running + (busy == 1 ? " Quit and end it?" : " Quit and end them?"), "Quit")) return;
        if (remote_hold_quit(true)) return;
        running_ = false;
        return;
    }
    for (Doc* d : open_docs()) {
            if (!unsaved_doc(*d)) continue;
            if (d->ed.path.empty()) {
                ++unnamed;
                continue;
            }
            const size_t slash = d->ed.path.find_last_of('/');
            names.push_back(d->ed.path.substr(slash == std::string::npos ? 0 : slash + 1));
        }
    focus_pane(first_pane);
    tab_show(first);  // what the question is about
    // The question names the files, as closing a tab does ("a.txt, b.cpp
    // and 3 more files"); new files are counted.
    constexpr size_t kNamed = 3;
    std::vector<std::string> parts(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(std::min(names.size(), kNamed)));
    if (names.size() > kNamed) parts.push_back(std::to_string(names.size() - kNamed) + " more file" + (names.size() - kNamed == 1 ? "" : "s"));
    if (unnamed) parts.push_back(unnamed == 1 ? std::string("a new file") : std::to_string(unnamed) + " new files");
    std::string list;
    for (size_t i = 0; i < parts.size(); ++i) list += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    const std::string what = "Save the changes to " + list + " before quitting?" + (busy ? " " + running : "");
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, unsaved == 1 ? "Save" : "Save All"},
        {0, 2, "Don't Save"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING;
    box.window = win_;
    box.title = "blit";
    box.message = what.c_str();
    box.numbuttons = 3;
    box.buttons = buttons;
    int hit = 0;
#if BLIT_DEV
    if (!win_) hit = 2, asked_ = what;  // scripted runs have nobody to ask (the tests read what was asked)
    else
#endif
    SDL_ShowMessageBox(&box, &hit);
    if (hit == 2) {
        if (remote_hold_quit(false)) return;  // (saves already under way are still waited for)
        running_ = false;
    } else if (hit == 1) {
        // Files with names are saved here and now (one on another machine
        // has its save started). One without a name needs the save dialog,
        // which answers later: quitting carries on from there (see the
        // handling of user event 1).
        for (size_t k = 0; k < panes_.size(); ++k)
            for (size_t i = 0; i < panes_[k]->docs.size(); ++i) {
                if (!unsaved_doc(*panes_[k]->docs[i]) || panes_[k]->docs[i]->ed.path.empty()) continue;
                focus_pane(panes_[k].get());
                tab_show(i);
                save();
                if (pane_->doc->ed.modified() && !remote_saving(pane_->doc->id)) return;  // it would not save: stay
            }
        for (size_t k = 0; k < panes_.size(); ++k)
            for (size_t i = 0; i < panes_[k]->docs.size(); ++i) {
                if (!panes_[k]->docs[i]->ed.modified() || remote_saving(panes_[k]->docs[i]->id)) continue;
                focus_pane(panes_[k].get());
                tab_show(i);
                quit_after_save_ = true;
                save();
                return;
            }
        if (remote_hold_quit(true)) return;  // (once they are written, what is still not saved is asked about again)
        running_ = false;
    }
}

// The window's title: the file in front and the project folder
// ("render.cpp — blit (~/Dev/blit)"), with a dot first while the file has
// unsaved changes. The folder is the side panel's, or before it has one,
// the one it would open at for the file. Set only when it changes: a frame
// that changes nothing compares a flag and two strings.
void App::update_title() {
    if (!win_) return;
    const std::string& shown = pane_->doc->title.empty() ? pane_->doc->ed.path : pane_->doc->title;
    const std::string* root = panel_root();
    if (pane_->doc->ed.modified() == title_modified_ && shown == title_path_ && (root ? *root : std::string_view()) == title_root_) return;
    SDL_SetWindowTitle(win_, window_title().c_str());
}

std::string App::window_title() {
    const std::string& shown = pane_->doc->title.empty() ? pane_->doc->ed.path : pane_->doc->title;
    const std::string* root = panel_root();
    if (shown != title_path_ && !root) title_base_ = !pane_->doc->ed.path.empty() && pane_->doc->title.empty() ? default_workdir() : std::string();  // (looks at the disk: once a file)
    title_modified_ = pane_->doc->ed.modified();
    title_path_ = shown;
    title_root_ = root ? *root : std::string();
    std::string folder = root ? *root : title_base_;
    if (const char* home = SDL_getenv("HOME"); home && *home && !is_remote(folder)) {
        const std::string h = home;
        if (folder == h || folder.starts_with(h + "/")) folder = "~" + folder.substr(h.size());
    }
    const size_t cut = shown.find_last_of("/\\");
    const std::string name = shown.empty() ? std::string(start_shown() ? "" : "untitled") : shown.substr(cut == std::string::npos ? 0 : cut + 1);
    return (title_modified_ ? "\xE2\x97\x8F " : "") + (name.empty() ? "" : name + " \xE2\x80\x94 ") + "blit" + (folder.empty() ? "" : " (" + folder + ")");
}

// ---------------------------------------------------------------- jumps

// One list of the places jumps started from, across files: Go to file,
// Search in files and its F4, a problem, a definition, and Vim's own long
// moves (G, a search, a mark, %) all go into it. Ctrl+- walks back through
// it and Ctrl+Shift+- forward again (Alt+Left and Alt+Right elsewhere), as
// do Vim's Ctrl-O and Ctrl-I, with or without a language server. As in
// Vim, a line is in the list once, and setting out back from the end
// first notes where the cursor is, for forward to come back to.
struct App::Jumps {
    struct Place {
        std::string path;  // the file; empty: a new one, known by its document
        u64 doc = 0;
        u64 line = 0, col = 0;  // (a byte in the line: a place survives the lines around it being edited better than an offset)
    };
    std::vector<Place> list;
    size_t at = 0;         // the place walked back to; list.size() while not walking
    bool going = false;    // the cursor is being put at one of them: no jump of its own
};

void App::jumps_create() {
    jumps_ = std::make_shared<Jumps>();
    vim_.on_jump = [this](u64 off) { jump_note(*pane_->doc, off); };  // (Vim's long moves; Ctrl-O and Ctrl-I come back as requests)
}

void App::jump_record() { jump_note(*pane_->doc, pane_->doc->ed.cursor()); }

bool App::jump_note(const Doc& d, u64 off) {
    if (!jumps_ || jumps_->going) return false;
    if (d.term || d.prof || d.md || d.image || d.pdf || (d.ed.path.empty() && d.ed.buf.size() == 0)) return false;  // (no text to come back to)
    Jumps& j = *jumps_;
    const Buffer& b = d.ed.buf;
    Jumps::Place p;
    p.path = d.ed.path.empty() ? std::string() : full_path(d.ed.path);
    p.doc = d.id;
    p.line = b.line_of(std::min(off, b.size()));
    p.col = std::min(off, b.size()) - b.line_start(p.line);
    std::erase_if(j.list, [&](const Jumps::Place& q) { return q.line == p.line && (p.path.empty() ? q.path.empty() && q.doc == p.doc : q.path == p.path); });
    j.list.push_back(std::move(p));
    if (j.list.size() > 100) j.list.erase(j.list.begin());
    j.at = j.list.size();
    return true;
}

// The cursor to place i: its file brought to the front (opened again if it
// was closed), at its line. False if the file is gone.
bool App::jump_go(size_t i) {
    Jumps& j = *jumps_;
    const Jumps::Place p = j.list[i];
    j.going = true;
    struct Done {
        Jumps& j;
        ~Done() { j.going = false; }
    } done{j};
    Pane* where = nullptr;
    size_t index = 0;
    if (!p.path.empty()) {
        if (!find_open(p.path, &where, &index)) {
            if (!is_remote(p.path) && !SDL_GetPathInfo(p.path.c_str(), nullptr)) return false;  // (opening it would make a new, empty one)
            if (!open_path(p.path.c_str())) return false;
        } else {
            focus_pane(where);
            tab_show(index);
        }
    } else {
        if (!find_doc(p.doc, &where, &index)) return false;
        focus_pane(where);
        tab_show(index);
    }
    panel_focus(false);
    Doc& d = *pane_->doc;
    if (d.loading) {  // (on its way from another machine: there when it comes)
        d.goto_line = p.line;
        d.goto_col = static_cast<u32>(std::min<u64>(p.col, ~0u));
        return true;
    }
    Editor& ed = d.ed;
    const u64 line = std::min<u64>(p.line, ed.buf.line_count() - 1), start = ed.buf.line_start(line);
    ed.move_to(std::min(start + p.col, ed.line_end(line)), false);
    vim_mouse();  // (Vim takes the cursor's new place as it would a click's)
    scroll_to_cursor();
    dirty_ = true;
    return true;
}

void App::jump_walk(i64 by) {
    if (!jumps_ || by == 0) return;
    Jumps& j = *jumps_;
    if (j.at >= j.list.size()) {
        if (by > 0) return;  // (not walking back: there is nothing ahead)
        // Setting out: here is where forward comes back to (if here is a place).
        if (jump_note(*pane_->doc, pane_->doc->ed.cursor())) j.at = j.list.size() - 1;
    }
    if (j.list.empty()) return;
    const i64 n = static_cast<i64>(j.list.size());
    i64 to = std::clamp<i64>(static_cast<i64>(j.at) + by, 0, n - 1);
    if (to == static_cast<i64>(j.at)) return;
    // A place whose file has gone is passed over, and goes from the list.
    while (to >= 0 && to < static_cast<i64>(j.list.size()) && to != static_cast<i64>(j.at)) {
        if (jump_go(static_cast<size_t>(to))) {
            j.at = static_cast<size_t>(to);
            return;
        }
        j.list.erase(j.list.begin() + to);
        if (to < static_cast<i64>(j.at)) --j.at;
        if (by < 0) --to;
    }
}

// (Two numbers at every key: the lines are looked up only when the cursor has moved.)
App::JumpFrom App::jump_from() const { return {pane_->doc->id, pane_->doc->ed.cursor()}; }

void App::jump_if_moved(const JumpFrom& from) {
    const Editor& ed = pane_->doc->ed;
    if (pane_->doc->id == from.doc && (ed.cursor() == from.off || ed.buf.line_of(ed.cursor()) == ed.buf.line_of(std::min(from.off, ed.buf.size())))) return;
    if (const Doc* d = find_doc(from.doc)) jump_note(*d, from.off);
}

// Ctrl+- and Ctrl+Shift+- (by where the key is: with Shift a Mac reports
// "_"), or Alt+Left and Alt+Right where Ctrl is the shortcut key.
bool App::jump_key(const SDL_KeyboardEvent& k) {
    const bool ctrl = k.mod & SDL_KMOD_CTRL, alt = k.mod & SDL_KMOD_ALT, shift = k.mod & SDL_KMOD_SHIFT, gui = k.mod & SDL_KMOD_GUI;
    if (kMac) {
        const bool minus = k.scancode == SDL_SCANCODE_MINUS || k.key == SDLK_MINUS || k.key == SDLK_UNDERSCORE;
        if (!minus || !ctrl || alt || gui) return false;
        jump_walk(shift ? 1 : -1);
        return true;
    }
    if (!alt || ctrl || gui || shift || (k.key != SDLK_LEFT && k.key != SDLK_RIGHT)) return false;
    jump_walk(k.key == SDLK_LEFT ? -1 : 1);
    return true;
}

// The list, for the tests: each place as file:line, ">" before the one walked back to.
std::string App::jump_dump() const {
    if (!jumps_) return "no jumps";
    std::string out;
    for (size_t i = 0; i < jumps_->list.size(); ++i) {
        const Jumps::Place& p = jumps_->list[i];
        const size_t slash = p.path.rfind('/');
        out += (out.empty() ? "" : " ") + std::string(i == jumps_->at ? ">" : "") + (p.path.empty() ? "untitled" : p.path.substr(slash + 1)) + ":" + std::to_string(p.line + 1);
    }
    return (out.empty() ? std::string("none") : out) + (jumps_->at >= jumps_->list.size() ? " >" : "");
}

// ---------------------------------------------------------------- view

double App::max_scroll_y() const {
    const double ch = font_->cell_h();
    const double content = static_cast<double>(view_line_count()) * ch;
    return content <= view_h() ? 0 : std::max(0.0, content - 3 * ch);
}

// How far right the view may go: until the end of the widest line is a few
// cells inside the right edge (and always far enough to show the cursor).
// The widest line is measured the first time the view is scrolled sideways
// by hand; until then there is no limit to give.
double App::max_scroll_x() const {
    if (wrap_on()) return 0;  // (lines wrapped: nothing goes past the right edge)
    const Doc& d = *pane_->doc;
    if (d.widest_rev == ~0ull) return 1e300;
    const double cw = font_->cell_w(), view_w = pane_->x1 - pane_->text_x - pad_x_;
    return std::max(0.0, static_cast<double>(std::max<u64>(d.widest_cols, pane_->cur_col)) * cw + 4 * cw - view_w);
}

// Finds the widest line of the document in front, unless that is known for
// the text as it is now. A small file is measured here (30 microseconds for
// 100 KB of code); a bigger one on a thread of its own (20 ms for 100 MB),
// and until that reports back the old answer stands.
void App::measure_widest() {
    Doc& d = *pane_->doc;
    if (d.measured) take_measured();  // an answer that is in, whose wake-up has not come round yet
    const u64 rev = d.ed.revision();
    const u32 tab = d.ed.tab_width;
    if (d.widest_rev == rev && d.widest_tab == tab) return;
    if (d.measurer.joinable()) {
        if (d.measuring_rev == rev && d.measuring_tab == tab && !d.measured) return;  // at it already
        d.measurer = {};
        d.measured = false;
    }
    std::vector<Piece> pieces;
    d.ed.buf.snapshot(&pieces);
    constexpr u64 kMeasureHere = 256u << 10;
    if (d.ed.buf.size() <= kMeasureHere) {
        d.widest_cols = Editor::widest_columns(pieces, tab);
        d.widest_rev = rev;
        d.widest_tab = tab;
        return;
    }
    d.measuring_rev = rev;
    d.measuring_tab = tab;
    Doc* doc = &d;  // documents do not move while they are open
    d.measurer = std::jthread([doc, tab, pieces = std::move(pieces)](std::stop_token stop) {
        thread_soon(Soon::kBackground);
        const u64 cols = Editor::widest_columns(pieces, tab, stop);
        if (stop.stop_requested()) return;
        doc->measured_cols = cols;
        doc->measured = true;
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        ev.user.code = 8;
        SDL_PushEvent(&ev);
    });
}

// Takes in what a measuring thread has found.
void App::take_measured() {
    for (const std::unique_ptr<Pane>& p : panes_)
      for (const std::shared_ptr<Doc>& d : p->docs) {
        if (!d->measured) continue;
        d->measurer = {};
        d->measured = false;
        d->widest_cols = d->measured_cols;
        d->widest_rev = d->measuring_rev;
        d->widest_tab = d->measuring_tab;
        dirty_ = true;
    }
}

// Columns per level of indentation in the document in front, for the indent
// lines: the setting, or a guess from the text. The guess is made when a
// file is first shown; a short file is looked at again when it gains or
// loses lines (it may have started empty), a long one is not. (Not at every
// key: typing along a line seldom changes the answer.)
u32 App::indent_step() {
    Doc& d = *pane_->doc;
    if (d.indent_chosen) return d.indent_step ? d.indent_step : std::max<u32>(1, d.ed.tab_width);  // (this file's, from the status bar, over the setting)
    if (cfg_.indent_width) return cfg_.indent_width;
    const u64 lines = d.ed.buf.line_count();
    if (d.indent_rev == ~0ull || (lines <= 256 && lines != d.indent_lines)) {
        d.indent_rev = d.ed.revision();
        d.indent_lines = lines;
        bool known = false;
        d.indent_step = d.ed.guess_indent(&d.indent_tabs, &known);
        if (d.indent_tabs) d.indent_step = 0;  // (indented with tabs: a level is a tab, however wide tabs are made later)
        if (!known && file_language() == "python") d.indent_tabs = false, d.indent_step = 4;  // (nothing indented yet: Python's own way)
    }
    return d.indent_step ? d.indent_step : std::max<u32>(1, d.ed.tab_width);
}

// Keeps the view, and where it is headed, inside the text (it never goes
// past an end), and sets it on its way there if it is not there yet.
void App::clamp_scroll() {
    const double my = max_scroll_y(), mx = max_scroll_x();
    pane_->target_y = std::clamp(pane_->target_y, 0.0, my);
    pane_->target_x = std::clamp(pane_->target_x, 0.0, mx);
    pane_->scroll_y = std::clamp(pane_->scroll_y, 0.0, my);
    pane_->scroll_x = std::clamp(pane_->scroll_x, 0.0, mx);
    if ((pane_->scroll_x != pane_->target_x || pane_->scroll_y != pane_->target_y) && !animating_) {
        last_step_ns_ = now_ns() - 8'000'000;
        animating_ = true;
    }
}

// Eases the scroll position toward its target. Returns true while moving.
bool App::step_scroll(double dt) {
    bool moving = false;
    auto ease = [&](double& pos, double target) {
        const double d = target - pos;
        if (d == 0) return;
        const double tau = cfg_.scroll_ease_ms * 1e-3;
        const double k = tau > 0 ? 1.0 - std::exp(-dt / tau) : 1.0;
        if (std::fabs(d) < 0.5) {
            pos = target;
        } else {
            pos += d * k;
            moving = true;
        }
        dirty_ = true;
    };
    for_each_pane([&] {
        ease(pane_->scroll_x, pane_->target_x);
        ease(pane_->scroll_y, pane_->target_y);
    });
    animating_ = moving;
    return moving;
}

void App::refresh_cursor_cache() {
    Pane& p = *pane_;
    const Editor& ed = p.doc->ed;
    // What is selected: Visual mode's selection if that is on in this
    // document, else the editor's own (a drag of the mouse, Shift and arrows).
    u64 lo = ed.sel_min(), hi = ed.sel_max();
    const bool visual = vim_selection(&lo, &hi);
    const u64 anchor = visual ? (lo * 0x9E3779B97F4A7C15ull) ^ hi : ed.anchor();
    const u64 folds = p.doc->folds.generation();
    if (p.cache_rev == ed.revision() && p.cache_cursor == ed.cursor() && p.cache_anchor == anchor && p.cache_visual == visual && p.cache_folds == folds) return;
    p.cache_folds = folds;
    p.cache_rev = ed.revision();
    p.cache_cursor = ed.cursor();
    p.cache_anchor = anchor;
    p.cache_visual = visual;
    const Buffer& b = ed.buf;
    p.cur_line = b.line_of(ed.cursor());
    p.cur_col = ed.col_between(b.line_start(p.cur_line), ed.cursor());
    if (const Wrap* w = wrap_on()) {  // (lines wrapped: the row of its part of the line, along it)
        p.cur_row = w->row_at(p.cur_line, p.cur_col);
        p.cur_x = p.cur_col - std::min(p.cur_col, w->start_col(p.cur_row));
    } else {
        p.cur_row = row_of_line(p.cur_line);
        p.cur_x = p.cur_col;
    }
    p.has_sel = visual || ed.has_selection();
    if (p.has_sel) {
        p.sel_line0 = b.line_of(lo);
        p.sel_col0 = ed.col_between(b.line_start(p.sel_line0), lo);
        p.sel_line1 = b.line_of(hi);
        p.sel_col1 = ed.col_between(b.line_start(p.sel_line1), hi);
    }
}

void App::scroll_to_cursor() {
    refresh_cursor_cache();
    const double cw = font_->cell_w(), ch = font_->cell_h(), vh = view_h();
    const double y0 = static_cast<double>(pane_->cur_row) * ch;
    if (y0 < pane_->target_y) {
        pane_->target_y = y0;
    } else if (y0 + ch > pane_->target_y + vh) {
        pane_->target_y = y0 + ch - vh;
    }
    const double x = pane_->cur_x * cw, margin = 4 * cw, view_w = pane_->x1 - pane_->text_x - pad_x_;
    if (x - margin < pane_->target_x) {
        pane_->target_x = std::max(0.0, x - margin);
    } else if (x + margin > pane_->target_x + view_w) {
        pane_->target_x = x + margin - view_w;
    }
    pane_->target_y = std::clamp(pane_->target_y, 0.0, std::max(max_scroll_y(), y0));
    // A long jump starts close to its target so the ease stays short.
    if (std::fabs(pane_->target_y - pane_->scroll_y) > 2.0 * vh)
        pane_->scroll_y = std::max(0.0, pane_->target_y + (pane_->scroll_y < pane_->target_y ? -vh : vh));
    if (std::fabs(pane_->target_x - pane_->scroll_x) > 2.0 * view_w)
        pane_->scroll_x = std::max(0.0, pane_->target_x + (pane_->scroll_x < pane_->target_x ? -view_w : view_w));
    if (pane_->scroll_x != pane_->target_x || pane_->scroll_y != pane_->target_y) {
        if (!animating_) last_step_ns_ = now_ns() - 8'000'000;
        animating_ = true;
    }
    dirty_ = true;
}

u64 App::offset_at_point(float x, float y) const {
    u64 line = 0;
    u32 col = 0;
    point_line_col(x, y, &line, &col);
    return pane_->doc->ed.off_at(line, col);
}

// The line and the display column under a point (past a line's end: the
// column there would be).
void App::point_line_col(float x, float y, u64* line_out, u32* col_out) const {
    const int cw = font_->cell_w(), ch = font_->cell_h();
    const i64 doc_y = std::llround(pane_->scroll_y) + static_cast<i64>(y * density_) - pane_->text_y;
    const u64 last = pane_->doc->ed.buf.line_count() - 1;
    const u64 row = doc_y <= 0 ? 0 : static_cast<u64>(doc_y) / static_cast<u64>(ch);
    const u64 line = doc_y <= 0 ? 0 : std::min<u64>(last, line_of_row(row));
    const double tx = x * density_ - pane_->text_x + static_cast<double>(std::llround(pane_->scroll_x)) - pad_x_;
    u32 col = tx <= 0 ? 0 : static_cast<u32>(tx / cw + 0.5);
    if (const Wrap* w = wrap_on(); w && row < w->rows()) {  // (a wrapped line's row: from where it starts, and not past its end)
        const u32 start = w->start_col(row), end = w->end_col(row);
        col += start;
        if (end != ~0u && col >= end) col = end > start ? end - 1 : start;
    }
    *line_out = line;
    *col_out = col;
}

// Lays out a frame: where each split is, then each split's text, tab bar
// and line numbers, then what lies over them.
void App::layout(int w, int h, FrameDesc& fd) {
    px_w_ = w;
    px_h_ = h;
    place_panes(w, h);
    peek_layout();  // (a peek's box: placed over its split, laid out last)
    for (const std::unique_ptr<Pane>& p : panes_) wrap_sync(*p);  // (each split's soft wrap, for its width now)
    build_measure();  // (the Run button: the tab bar under it ends before it)
    Pane* const focused = pane_;
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (fd.view_count >= kMaxViews) break;
        if (!p->doc) continue;
        pane_ = p.get();
        place_view(pane_);  // (a file two splits show: this one's cursor and selection)
        pane_->view = fd.view_count;
        if (pane_ == focused) focus_view_ = fd.view_count;
        layout_pane(fd, pane_ == focused);
    }
    pane_ = focused;
    place_view(pane_);
    fd.theme = theme_;
    fd.scale = scale_;
    if (cfg_.indent_guides && guide_inks_) fd.guide_px = static_cast<float>(std::max<long>(1, std::lround(scale_)));
    // Later ones are drawn over earlier ones (and a shadow falls on what
    // was drawn before the box that casts it: the side panel's on the
    // bottom line, a menu's on everything).
    fx_begin_layout();
    fold_reveal();  // (the cursor gone into folded lines: they open)
    splits_layout(w, h, fd);
    start_layout(fd);
    prof_layout(fd);  // (a profile's tab, over its pane's text area)
    term_layout(fd);  // (and a terminal's)
    md_layout(fd);    // (and a Markdown preview's)
    image_layout(fd);  // (and an image file's)
    pdf_layout(fd);    // (and a PDF's)
    minimap_layout(fd);  // (and each split's minimap, down its right)
    build_layout(w, fd);  // the Run button, over the top right tab bar's end
    vim_layout(w, h, fd);
    panel_layout(w, std::max(1, h - status_px()), fd);  // (above the status bar)
    status_layout(w, h, fd);
    lsp_layout(w, h, fd);
    shape_marks(fd);  // (the bracket that pairs with the cursor's)
    fold_notes(fd);   // (how many lines each folded one has under it)
    ruler_notes(fd);  // (the marks along the scroll bar)
    git_note(fd);
    vim_marks(fd);
    caret_marks(fd);  // the other carets, and what each has selected
    snippet_marks(fd);  // a snippet's stops still to come
    asm_marks(fd);      // the lines of a file and of its assembly that go together
    nb_marks(fd);       // a notebook's saved outputs
    lens_marks(fd);     // how many references each definition on show has
    pr_marks(fd);       // a pull request's comments, after their lines
    history_tick();     // (typed in for a while: a version of it kept)
    spell_marks(fd);    // words not known, underlined
    if (script_) script_layout(w, h, fd);  // marks, boxes and messages of the scripts'
    prof_marks(fd);  // inline performance labels (after a server's and the scripts' notes)
    debug_marks(fd);  // and where the program being debugged has stopped
    panel_menu_layout(w, h, fd);
    if (find_open_) find_layout(w, h, fd);
    splits_carry_layout(w, h, fd);
    fx_end_layout();
}

// One split (the one pane_ points at for now): its text view, and its tab
// bar and line numbers over that.
void App::layout_pane(FrameDesc& fd, bool focused) {
    Pane& p = *pane_;
    TextView& v = fd.views[fd.view_count++];
    const int tw = std::max(1, p.x1 - p.text_x), th = std::max(1, p.y1 - p.text_y);
    refresh_cursor_cache();
    clamp_scroll();
    const i64 cw = font_->cell_w(), ch = font_->cell_h();
    const i64 sx = std::llround(p.scroll_x), sy = std::llround(p.scroll_y);
    const i64 tx = sx - pad_x_;  // text-space x of the text area's left edge

    v.buf = &p.doc->ed.buf;
    v.x = p.text_x;
    v.y = p.text_y;
    v.w = tw;
    v.h = th;
    v.anchors = &p.anchors;
    v.guides = &p.guides;
    GridSpec& g = v.grid;
    // Left of the text there is padding, and more than that while the view
    // is pulled past the left end: blank grid columns stand in for it, which
    // keeps the grid origin non-negative. The same goes for rows above the
    // first line while the view is pulled down past the top.
    g.lead = tx < 0 ? static_cast<u32>((-tx + cw - 1) / cw) : 0;
    g.first_col = tx > 0 ? static_cast<u32>(tx / cw) : 0;
    g.cols = static_cast<u32>(tw / cw + 2) + g.lead;
    g.rows = static_cast<u32>(th / ch + 2);
    const i64 blank = sy < 0 ? std::min<i64>((-sy + ch - 1) / ch, g.rows) : 0;
    g.top_line = sy < 0 ? 0 : static_cast<u64>(sy / ch);
    if (const Wrap* w = wrap_on()) {  // (lines wrapped: each row's line, and where in it the row starts and its part ends)
        const u64 top_row = g.top_line, lines = p.doc->ed.buf.line_count(), rows = w->rows();
        p.row_lines.resize(g.rows);
        p.row_cols.resize(g.rows);
        p.row_ends.resize(g.rows);
        w->walk(top_row, g.rows, p.row_lines.data(), p.row_cols.data(), p.row_ends.data());  // (once down the rows on show)
        (void)rows;
        g.top_line = p.row_lines[0] == ~0ull ? lines : p.row_lines[0];
        g.row_lines = p.row_lines.data();
        g.row_cols = p.row_cols.data();
        g.row_ends = p.row_ends.data();
        g.first_col = 0;
        g.rows_key = ((w->rows() * 1000003 + w->width()) * 1000003 + top_row) * 1000003 + p.doc->ed.revision() + 7;
    } else if (const Folds* f = folds_on()) {  // (lines folded away: each row's line from the map)
        const u64 top_row = g.top_line, lines = p.doc->ed.buf.line_count();
        p.row_lines.resize(g.rows);
        for (u32 r = 0; r < g.rows; ++r) {
            const u64 line = f->line_at(top_row + r);
            p.row_lines[r] = line < lines ? line : ~0ull;
        }
        g.top_line = p.row_lines[0] == ~0ull ? lines : p.row_lines[0];
        g.row_lines = p.row_lines.data();
        g.rows_key = (f->generation() * 1000003 + top_row) * 1000003 + p.doc->ed.revision() + 1;
    }
    v.blank_rows = static_cast<u32>(blank);
    v.origin[0] = static_cast<float>(tx < 0 ? tx + static_cast<i64>(g.lead) * cw : tx - static_cast<i64>(g.first_col) * cw);
    v.origin[1] = static_cast<float>(sy < 0 ? sy + blank * ch : sy % ch);

    g.doc = p.doc->id;
    g.revision = p.doc->ed.revision();
    g.edits = &p.doc->ed.edit_log();
    g.tab_width = p.doc->ed.tab_width;
    if (p.doc->syntax.active()) {
        g.highlights = &p.doc->syntax.highlights();
        g.style_bits = p.doc->style_bits;
    } else if (p.doc->marks) {  // a diff, coloured by its own marks
        g.highlights = p.doc->marks.get();
        g.style_bits = p.doc->style_bits;
    }
    if (frames_ > 1) git_want(*p.doc);  // (not before the first frame is out)
    if (cfg_.indent_guides && guide_inks_ && !p.doc->marks) {  // (not in a diff: its first column is +, - or a space)
        g.guide_step = indent_step();
        g.guide_inks = guide_inks_;
    }
    // How round things are. (A radius for what is selected only while
    // something is: the shader that rounds it costs a little more.)
    const Look lk = look();
    v.sel_radius = p.has_sel ? lk.sel : 0;
    v.cursor_radius = lk.cursor;
    v.match_radius = lk.match;
    v.tint_radius = lk.box;
    v.dim = focused || panes_.size() < 2 ? 0.0f : std::min(cfg_.look_dim_inactive, 0.6f);
    if (Pane* peek = peek_pane(); peek && peek->host == p.id) v.dim = std::max(v.dim, 0.35f);  // (under a peek's box: the box stands out)
    g.has_sel = p.has_sel;
    g.sel_line0 = p.sel_line0;
    g.sel_line1 = p.sel_line1;
    g.sel_col0 = p.sel_col0;
    g.sel_col1 = p.sel_col1;

    // The cursor shows in the split that has the keyboard: a bar between
    // characters; with Vim outside Insert mode a block on one, or a line
    // under it while an operator waits or in Replace mode. Where it belongs
    // is worked out in the text's own pixels (so that scrolling does not
    // come into it); it is drawn where it has got to on its way there.
    if (focused && !panel_has_keyboard()) {
        const i64 bar = std::max<i64>(1, std::lround(scale_));
        const bool on_char = cfg_.modal && vim_.mode() != Vim::kInsert;
        const bool under = on_char && (vim_.mode() == Vim::kReplace || vim_.pending());
        const double x = static_cast<double>(static_cast<i64>(p.cur_x) * cw), y = static_cast<double>(static_cast<i64>(p.cur_row) * ch);
        const double want[4] = {x, under ? y + static_cast<double>(ch - std::max<i64>(2, ch / 6)) : y, on_char ? x + static_cast<double>(cw) : x + static_cast<double>(bar),
                                y + static_cast<double>(ch)};
        double at[4];
        fx_cursor(want, at);
        const double left = static_cast<double>(p.text_x + pad_x_ - sx), top = static_cast<double>(p.text_y - sy);
        const double cx = left + at[0], cy = top + at[1];
        if (cy > p.text_y - ch && cy < p.y1 && cx >= p.text_x && cx < p.x1) {
            v.cursor_block = on_char && !under;
            v.cursor[0] = static_cast<float>(cx);
            v.cursor[1] = static_cast<float>(cy);
            v.cursor[2] = static_cast<float>(std::min(left + at[2], static_cast<double>(p.x1)));
            v.cursor[3] = static_cast<float>(top + at[3]);
        }
    }

    const double my = max_scroll_y();
    if (my > 0 && (v.thumb_alpha = fx_thumb(p, sx, sy)) > 0) {
        const double content = my + th;
        const double bar = std::max(24.0 * scale_, th * (static_cast<double>(th) / content));
        const double ty = p.text_y + std::clamp(static_cast<double>(sy) / my, 0.0, 1.0) * (th - bar);
        v.thumb[0] = static_cast<float>(p.x1 - std::lround(6 * scale_));
        v.thumb[1] = static_cast<float>(std::floor(ty));
        v.thumb[2] = static_cast<float>(p.x1 - std::lround(2 * scale_));
        v.thumb[3] = static_cast<float>(std::floor(ty + bar));
    }
    p.has_thumb = my > 0;
    if (!p.has_thumb) {  // (nothing to fade: no frames are spent on it; it starts again when there is something to scroll)
        p.thumb_alpha = 0;
        p.thumb_x = p.thumb_y = -1;
    }
    gutter_layout(px_w_, px_h_, fd);
    tabs_layout(px_w_, px_h_, fd, focused);
    crumbs_layout(fd, focused);
    sticky_layout(fd, v);
    ruler_layout(v);
}

// The display's refresh rate decides how often a frame may be drawn.
void App::update_refresh() {
    float hz = opt_.fps_cap;
    if (hz <= 0 && win_) {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win_));
        hz = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0f;
    }
    // (Without a window nothing is shown, and the benchmarks that draw
    // into a texture are there to find out how fast drawing can go.)
    frame_period_ns_ = hz > 0 ? static_cast<u64>(1e9 / static_cast<double>(hz)) : 0;
}

// Never more frames than the display can show. Normally the window system
// sees to that: asking for the next image to draw into waits for the
// display. Where it does not (some present modes, some systems), the wait
// happens here instead. A frame the window system did hold back is not held
// back a second time, which would only make it late.
void App::pace() {
    if (frame_period_ns_ && !last_frame_paced_) {
        const u64 now = now_ns(), earliest = last_frame_ns_ + frame_period_ns_;
        if (now < earliest) SDL_DelayPrecise(earliest - now);
    }
    last_frame_ns_ = now_ns();
}

// Two images of the window under way, or three. With two, a frame drawn
// after a pause (a key typed, the cursor moved) is on the screen about 24 ms
// sooner than with three (16 against 40 ms from being handed over, at the
// p50, measured); but frames drawn one after another, as when the view
// scrolls, then miss the display's pace now and then (up to a third of them
// late in some runs). So: three while the view scrolls, two otherwise.
void App::choose_images() {
    if (win_ && !opt_.present) gpu_.set_images(animating_ ? 3 : 2);
}

Renderer::Result App::frame(FrameStats* st) {
    if (!gpu_.acquired()) {
        choose_images();
        pace();  // (else these were done before the frame was acquired)
    }
    FrameStats own;
    if (!st) st = &own;

    for_each_pane([this] { sync_syntax(); });
    sync_lsp();
    vim_attach();  // Vim is with whichever document is in front
    dirty_ = false;
    ++frames_;
    Renderer::Result r = gpu_.draw(*font_, [this](int w, int h, FrameDesc& fd) { layout(w, h, fd); }, st);
    last_frame_paced_ = st->wait_ns > frame_period_ns_ / 4;
    pane_->doc->syntax.release();  // the window is up: a big file's full parse may start
    if (lsp_pending_) {  // and so may a language server
        lsp_pending_ = false;
        start_lsp();
    }
    if (grammars_pending_) {
        grammars_pending_ = false;
        want_grammars();
    }
    git_follow();  // and git, about the file in front
    build_follow();  // and what the working folder builds with
    if (r != Renderer::kDrawn) {
        // Nothing to present to (minimized): settle instead of spinning.
        for (const std::unique_ptr<Pane>& p : panes_) {
            p->scroll_x = p->target_x;
            p->scroll_y = p->target_y;
        }
        animating_ = false;
        fx_settle();
    }
    update_title();
    return r;
}

// ---------------------------------------------------------------- input

bool App::event_watch(void* self, SDL_Event* e) {
    // The OS runs its own loop while a window is being resized; this keeps
    // the contents drawn during it.
    App* a = static_cast<App*>(self);
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && a->running_) a->frame();
    return true;
}

void App::handle(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
            request_quit();
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_EXPOSED:
            dirty_ = true;
            break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            apply_present_mode();
            dirty_ = true;
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            update_refresh();  // another monitor, maybe another refresh rate
            dirty_ = true;
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
            update_refresh();
            density_ = std::max(1.0f, SDL_GetWindowPixelDensity(win_));
            float s = SDL_GetWindowDisplayScale(win_);
            if (s > 0 && s != scale_ && opt_.scale == 0) {
                scale_ = s;
                std::string err;
                const int old_cw = font_->cell_w(), old_ch = font_->cell_h();
                if (load_font(&err)) {
                    rescale_scroll(old_cw, old_ch);
                    pick_theme();
                }
            }
            dirty_ = true;
            break;
        }
        case SDL_EVENT_SYSTEM_THEME_CHANGED:
            pick_theme();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            panel_refresh();  // files that came or went
            find_stale();
            reload_changed(); // and open files that were written
            if (cfg_.git) git_.refresh();
            script_focus(true);  // and init.lua, if it was changed in another program
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            drag_cancel();
            text_press_ = false;
            if (script_) script_focus(false);
            break;
        case SDL_EVENT_KEY_DOWN:
            on_key(e.key);
            break;
        case SDL_EVENT_TEXT_INPUT:
            typed_text(e.text.text);  // the finder box, the side panel, a mapping, or the text (applua.cpp)
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            on_wheel(e.wheel);
            break;
        case SDL_EVENT_PINCH_UPDATE:
            if (!image_pinch(e.pinch.scale)) pdf_pinch(e.pinch.scale);  // (a picture or a PDF is zoomed)
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT) text_press_ = false;
            mouse_x_ = e.button.x;
            mouse_y_ = e.button.y;
            if (find_open_ && find_mouse_down(e.button)) break;  // on the box that finds things, or beside it (which shuts it)
            if (status_mouse_down(e.button)) break;
            if (const JumpFrom from = jump_from(); panel_mouse_down(e.button)) {  // (a problem, a file: Ctrl+- comes back from it)
                jump_if_moved(from);
                break;
            }
            if (start_mouse_down(e.button)) break;
            if (build_mouse_down(e.button)) break;  // the Run button
            if (prof_mouse_down(e.button)) break;  // a profile's tab
            if (md_mouse_down(e.button)) break;    // a Markdown preview
            if (image_mouse_down(e.button)) break;  // an image file's tab
            if (pdf_mouse_down(e.button)) break;    // a PDF's
            if (term_mouse_down(e.button)) break;  // a terminal's
            if (splits_mouse_down(e.button)) break;  // a divider
            // The split under the mouse gets the click, and with it the keyboard.
            if (Pane* over = pane_at(static_cast<int>(e.button.x * density_), static_cast<int>(e.button.y * density_))) focus_pane(over);
            if (tabs_mouse_down(e.button)) break;
            if (crumbs_mouse_down(e.button)) break;
            if (sticky_mouse_down(e.button)) break;
            if (fold_mouse_down(e.button)) break;
            on_mouse_down(e.button);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) dragging_ = dragging_thumb_ = column_drag_ = false, minimap_drag_ = 0;
            splits_mouse_up(e.button);  // lets go of whatever was being carried
            prof_mouse_up();  // a stretch of a profile's timeline
            panel_mouse_up();
            tabs_mouse_up();
            term_mouse_up();
            image_mouse_up();
            pdf_mouse_up();
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (text_press_) vim_mouse();  // a click or a drag in the text, as Vim sees it
                text_press_ = false;
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            hover_update(-1, -1);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            mouse_x_ = e.motion.x;
            mouse_y_ = e.motion.y;
            if (find_open_ && find_mouse_motion(e.motion)) break;
            if (term_mouse_motion(e.motion)) break;  // choosing text in a terminal
            if (image_mouse_motion(e.motion) || pdf_mouse_motion(e.motion)) break;  // a picture or a PDF's pages being dragged
            hover_update(static_cast<int>(e.motion.x * density_), static_cast<int>(e.motion.y * density_));
            if (!dragging_ && !dragging_thumb_ &&
                (splits_mouse_motion(e.motion) || tabs_mouse_motion(e.motion) || panel_mouse_motion(e.motion) || start_mouse_motion(e.motion) ||
                 prof_mouse_motion(e.motion)))
                break;
            on_mouse_motion(e.motion);
            break;
        case SDL_EVENT_DROP_FILE:  // (and a file opened from the Finder, or with "Open With")
            if (e.drop.data) open_place({e.drop.data, 0, 0});
            break;
        case SDL_EVENT_USER: {
#if BLIT_DEV
            if (e.user.code == 99) {  // benchmark timer
                running_ = false;
                break;
            }
#endif
            if (e.user.code == 3) {  // the syntax worker has new colours
                dirty_ = true;
                break;
            }
            if (e.user.code == 4) {  // the language server has sent something
                sync_lsp();
                break;
            }
            if (e.user.code == 5) {  // a grammar has finished installing, or failed to
                sync_grammars();
                break;
            }
            if (e.user.code == 6) {  // the file tree has read a folder
                sync_panel();
                break;
            }
            if (e.user.code == 10) {  // the worker that finds files and text has something
                find_sync();
                break;
            }
            if (e.user.code == 11) {  // a program a script started has said something, or ended
                script_jobs();
                break;
            }
            if (e.user.code == 9) {  // git has finished something
                sync_git();
                break;
            }
            if (e.user.code == 8) {  // a file's widest line has been found
                take_measured();
                break;
            }
            if (e.user.code == 12) {  // a connection to another machine, or a file from one, has news
                remote_sync();
                break;
            }
            if (e.user.code == 13) {  // a recording of a program has news (or has run a while longer)
                prof_sync();
                break;
            }
            if (e.user.code == 14) {  // a terminal's program has written something, or ended
                term_sync();
                break;
            }
            if (e.user.code == 15) {  // the debugger has said something
                debug_sync();
                break;
            }
            if (e.user.code == 16) {  // what the working folder builds with has been found
                build_sync();
                break;
            }
            if (e.user.code == 25) {  // something changed on disk in the folder worked in
                watch_take();
                break;
            }
            if (e.user.code == 24) {  // the spelling of the words on show has been checked
                spell_take();
                break;
            }
            if (e.user.code == 23) {  // a file's assembly has been read
                asm_take();
                break;
            }
            if (e.user.code == 22) {  // the project's tests have been found
                test_sync();
                break;
            }
            if (e.user.code == 17) {  // a picture has been read
                picture_sync();
                break;
            }
            if (e.user.code == 18) {  // a PDF has been read, or a page of one drawn
                pdf_sync();
                break;
            }
            if (e.user.code == 19) {  // the start screen's facts, or PyPI's list, have been found
                start_sync();
                break;
            }
            if (e.user.code == 21) {  // a later `blit` has handed files over
                instance_take();
                break;
            }
            if (e.user.code == 20) {  // a Markdown preview has been drawn
                md_sync();
                break;
            }
            break;
        }
        default:
            break;
    }
}

// Typed text into the document: Vim's, or typed as it is.
void App::on_text(const char* text) {
    if (term_text(text)) return;  // (to a terminal in front)
    if (pdf_text(text)) return;   // (a PDF's password, or what to find in it: apppdf.cpp)
    const Editor& ed = pane_->doc->ed;
    const u64 cursor = ed.cursor(), size = ed.buf.size(), rev = ed.revision();
    if (cfg_.modal) {  // text, or commands: Vim knows which
        if (vim_.mode() == Vim::kInsert && !ed.multi() && pair_text(text)) return;
        vim_text(text);
        pairs_follow(cursor, size, rev);
        return;
    }
    if (pane_->doc->read_only) return;  // a diff is for reading
    if (ed.multi()) {  // (at every caret; brackets are not closed for them)
        pane_->doc->ed.each_caret([&] { pane_->doc->ed.insert(text); });
        scroll_to_cursor();
        lsp_typed(text);
        return;
    }
    if (pair_text(text)) return;
    pane_->doc->ed.insert(text);
    pairs_follow(cursor, size, rev);
    scroll_to_cursor();
    lsp_typed(text);
}

void App::on_key(const SDL_KeyboardEvent& k) {
    script_drop_text_ = false;
    term_drop_text_ = false;  // (only the text a key itself brings is dropped)
    if (start_key(k)) return;  // the start screen is showing (appstart.cpp)
    const bool boxed = find_open_ || panel_has_keyboard();  // the finder box or the side panel has the keyboard: a tab in front does not
    if (!boxed) {
        if (prof_key(k)) return;   // a profile's tab is in front (appprofile.cpp)
        if (md_key(k)) return;     // a Markdown preview's (appmarkdown.cpp)
        if (image_key(k)) return;  // an image file's (appimage.cpp)
        if (pdf_key(k)) return;    // a PDF's (apppdf.cpp)
        // Ctrl+` and Ctrl+Shift+`, the terminals' split (appcommands.cpp), are never a terminal's own keys.
        if (k.key == SDLK_GRAVE && (k.mod & SDL_KMOD_CTRL) && !(k.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI)) && command_key(k)) {
            term_drop_text_ = true;
            return;
        }
        // Ctrl+Shift+G, the Git tab, is not the terminal's either (appcommands.cpp).
        if (kMac && k.key == SDLK_G && (k.mod & SDL_KMOD_CTRL) && (k.mod & SDL_KMOD_SHIFT) && !(k.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI)) && pane_->doc->term &&
            command_key(k))
            return;
        if (term_key(k)) return;   // a terminal's tab is in front (appterm.cpp)
    }
    if (script_keys_ && script_key(k)) return;  // a mapping of a script's has it (applua.cpp)
    if (!find_open_ && !panel_has_keyboard() && pair_key(k)) return;  // Backspace or Enter between a pair
    if (!boxed && peek_key(k)) return;  // Esc in a peek's box: it closes (apppeek.cpp)
    if (vim_owns_key(k)) {  // (Ctrl+W, Ctrl+V, Ctrl+O ... in the text, with modes on: Vim's before the editor's, where Ctrl is both)
        vim_key(k);
        return;
    }
    if (const JumpFrom from = jump_from(); !find_open_ && command_key(k)) {  // the keys of the commands added in appcommands.cpp
        if (k.key == SDLK_F8) jump_if_moved(from);  // (a problem gone to: Ctrl+- comes back from it)
        return;
    }
    const bool shift = k.mod & SDL_KMOD_SHIFT;
    const bool cmd = k.mod & kCmd;
    const i64 page = std::max(1, visible_rows() - 1);
    if (k.key == SDLK_ESCAPE && script_) script_close_floats();

    if ((find_open_ || cmd || (kCtrlShared && (k.mod & SDL_KMOD_ALT))) && find_key(k)) return;  // the box that finds things is open, or this opens it
    if (const JumpFrom from = jump_from(); panel_key(k)) {  // the side panel had the keyboard, or it was one of its shortcuts
        jump_if_moved(from);
        return;
    }
    if (splits_key(k)) return; // a new split, or the keyboard to another one
    if (tabs_key(k)) return;   // a new tab, another tab, this tab closed
    if (k.key == SDLK_ESCAPE && snippet_) snippet_end();  // (Esc ends a snippet, and does what it does)
    // Esc with more carets than one leaves the primary alone (with Vim, in Normal mode).
    if (k.key == SDLK_ESCAPE && pane_->doc->ed.multi() && (!cfg_.modal || (vim_.mode() == Vim::kNormal && !vim_.pending() && vim_.showcmd().empty()))) {
        pane_->doc->ed.clear_carets();
        dirty_ = true;
        return;
    }
    // With Vim, Esc is Vim's whatever else is open: it shuts the popups too.
    if (cfg_.modal && k.key == SDLK_ESCAPE) {
        lsp_close(kPopAll);
        if (vim_key(k)) return;
    }
    if (jump_key(k)) return;   // back or forward through the jumps, in any mode
    if (lsp_key(k)) return;    // a popup, or a language-server command, took it
    if (snippet_key(k)) return;  // Tab: a snippet's next stop, or one's own put in (appsnippet.cpp)
    if (nb_key(k)) return;       // Shift+Return in a notebook: its cell run (appnotebook.cpp)
    if (cmd && (k.mod & SDL_KMOD_ALT) && k.key == SDLK_B) return git_command("blame", {});
    if (cfg_.modal) {
        // Tab on a ":set" line completes the setting's name, then its value (applua.cpp).
        if (k.key == SDLK_TAB && vim_.mode() == Vim::kCommand && !(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) && set_complete(shift ? -1 : 1)) return;
        // The application's shortcuts for the clipboard, undo and select
        // all are Vim commands when Vim is not in Insert mode, so that they
        // act on what Visual mode has selected and leave the cursor where
        // Vim would.
        vim_attach();
        const bool typing = vim_.mode() == Vim::kInsert || vim_.mode() == Vim::kReplace;
        if (cmd && !typing && vim_.mode() != Vim::kCommand) {
            const char* keys = nullptr;
            switch (k.key) {
                case SDLK_C: keys = vim_.visual() ? "\"+y" : "\"+yy"; break;
                case SDLK_X: keys = vim_.visual() ? "\"+d" : "\"+dd"; break;
                case SDLK_V: keys = vim_.visual() ? "\"+p" : "\"+P"; break;
                case SDLK_Z: keys = shift ? "<C-r>" : "u"; break;
                case SDLK_Y: keys = "<C-r>"; break;
                case SDLK_A: keys = "ggVG"; break;
                default: break;
            }
            if (keys) {
                vim_.keys(pane_->doc->ed, keys);
                return vim_after(Vim::kHandled);
            }
        }
        if (vim_key(k)) return;
    }
    // A tab that shows a diff can be read, searched with the eyes and copied from.
    const bool fixed = pane_->doc->read_only;
    if (fixed && cmd && (k.key == SDLK_V || k.key == SDLK_X || k.key == SDLK_Z || k.key == SDLK_Y)) return;
    if (fixed && (k.key == SDLK_BACKSPACE || k.key == SDLK_DELETE || k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER || k.key == SDLK_TAB)) return;
    if (cmd) {
        // Whatever these do, the popups were about the text as it was.
        if (k.key == SDLK_V || k.key == SDLK_X || k.key == SDLK_Z || k.key == SDLK_Y || k.key == SDLK_A) lsp_close(kPopAll);
        switch (k.key) {
            case SDLK_A:
                pane_->doc->ed.clear_carets();
                pane_->doc->ed.select_all();
                dirty_ = true;
                return;
            case SDLK_C:
            case SDLK_X:
                if (pane_->doc->ed.multi()) {  // what each caret has selected, a line each
                    SDL_SetClipboardText(carets_text().c_str());
                    if (k.key == SDLK_X) pane_->doc->ed.each_caret([&] {
                        if (pane_->doc->ed.has_selection()) pane_->doc->ed.backspace();
                    });
                    scroll_to_cursor();
                } else if (pane_->doc->ed.has_selection()) {
                    SDL_SetClipboardText(pane_->doc->ed.selection_text().c_str());
                    if (k.key == SDLK_X) {
                        pane_->doc->ed.backspace();
                        scroll_to_cursor();
                    }
                }
                return;
            case SDLK_V: {
                char* t = SDL_GetClipboardText();
                if (t && *t) {
                    if (pane_->doc->ed.multi()) carets_paste(t);
                    else pane_->doc->ed.insert(t);
                }
                SDL_free(t);
                scroll_to_cursor();
                return;
            }
            case SDLK_Z:
                if (shift ? pane_->doc->ed.redo() : pane_->doc->ed.undo()) scroll_to_cursor();
                return;
            case SDLK_Y:
                if (pane_->doc->ed.redo()) scroll_to_cursor();
                return;
            case SDLK_S: save(); return;
            case SDLK_N:  // a new file (the file tree, with the keyboard, has had Cmd+N first: a new file there)
                if (shift || (k.mod & SDL_KMOD_ALT)) break;
                tab_new();
                return;
            case SDLK_Q: request_quit(); return;
            case SDLK_COMMA: settings_open(); return;
            case SDLK_EQUALS: zoom(1); return;
            case SDLK_MINUS: zoom(-1); return;
            case SDLK_0: zoom(0); return;
            default: break;
        }
    }

    switch (k.key) {  // (the keys below, at every caret)
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN: case SDLK_HOME: case SDLK_END: case SDLK_PAGEUP: case SDLK_PAGEDOWN:
        case SDLK_BACKSPACE: case SDLK_DELETE: case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_TAB: break;
        default: return;
    }
    // (Cmd+Up and Cmd+Down, the text's ends, leave one caret there.)
    if (kMac && cmd && (k.key == SDLK_UP || k.key == SDLK_DOWN)) pane_->doc->ed.clear_carets();
    pane_->doc->ed.each_caret([&] { edit_key(k, page); });
    scroll_to_cursor();
    lsp_after_key(k.key);
}

// A key that moves or edits, at the caret (on_key runs it at each).
void App::edit_key(const SDL_KeyboardEvent& k, i64 page) {
    const bool shift = k.mod & SDL_KMOD_SHIFT;
    const bool cmd = k.mod & kCmd;
    const bool word = k.mod & kWord;
    if (pane_->doc->ed.multi() == false && pair_key(k)) return;
    switch (k.key) {
        case SDLK_LEFT:
            if (kMac && cmd) pane_->doc->ed.home(shift);
            else if (word) pane_->doc->ed.word_left(shift);
            else pane_->doc->ed.left(shift);
            break;
        case SDLK_RIGHT:
            if (kMac && cmd) pane_->doc->ed.end(shift);
            else if (word) pane_->doc->ed.word_right(shift);
            else pane_->doc->ed.right(shift);
            break;
        case SDLK_UP:
            if (kMac && cmd) pane_->doc->ed.doc_start(shift);
            else pane_->doc->ed.up(shift);
            break;
        case SDLK_DOWN:
            if (kMac && cmd) pane_->doc->ed.doc_end(shift);
            else pane_->doc->ed.down(shift);
            break;
        case SDLK_HOME:
            if (!kMac && cmd) pane_->doc->ed.doc_start(shift);
            else pane_->doc->ed.home(shift);
            break;
        case SDLK_END:
            if (!kMac && cmd) pane_->doc->ed.doc_end(shift);
            else pane_->doc->ed.end(shift);
            break;
        case SDLK_PAGEUP: pane_->doc->ed.vertical(-page, shift); break;
        case SDLK_PAGEDOWN: pane_->doc->ed.vertical(page, shift); break;
        case SDLK_BACKSPACE:
            if (kMac && cmd) {
                pane_->doc->ed.home(true);
                pane_->doc->ed.backspace();
            } else if (word) {
                pane_->doc->ed.backspace_word();
            } else {
                pane_->doc->ed.backspace();
            }
            break;
        case SDLK_DELETE:
            if (word) pane_->doc->ed.delete_word();
            else pane_->doc->ed.del();
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: pane_->doc->ed.newline(); break;
        case SDLK_TAB: pane_->doc->ed.insert("\t"); break;
        default: break;
    }
}

// The wheel scrolls what the mouse is over, whichever split has the keyboard.
void App::on_wheel(const SDL_MouseWheelEvent& w) {
    if (find_open_ && find_wheel(w)) return;   // the box that finds things is open: its list turns
    if (panel_wheel(w)) return;  // the mouse is over the side panel
    if (prof_wheel(w)) return;   // or over a profile's tab
    if (md_wheel(w)) return;     // or a Markdown preview
    if (image_wheel(w)) return;  // or an image file's tab
    if (pdf_wheel(w)) return;    // or a PDF's
    if (term_wheel(w)) return;   // or a terminal's
    Pane* const focused = pane_;
    if (mouse_x_ >= 0)
        if (Pane* over = pane_at(static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_))) pane_ = over;
    wheel_pane(w);
    pane_ = focused;
}

void App::wheel_pane(const SDL_MouseWheelEvent& w) {
    if (tabs_wheel(w)) return;   // over the tab bar
    lsp_close(kPopHover);        // a hover popup belongs to where the mouse was
    if (w.x != 0) measure_widest();  // sideways: how far is there to go?
    // SDL reports whole notches for wheels and fractions for touchpads.
    // A fraction puts us in 1:1 pixel mode for a moment, so the odd whole
    // value inside a touchpad gesture is not mistaken for a notch.
    const u64 now = now_ns();
    if (w.x != std::floor(w.x) || w.y != std::floor(w.y)) precise_until_ns_ = now + kPreciseWindowNs;
    if (now < precise_until_ns_) {
        // The view follows the fingers exactly (and, on a Mac, the glide
        // the system adds after they lift), and stops at the text's edges.
        const double unit = 10.0 * scale_;
        auto move = [&](double& pos, double& target, double delta, double limit) {
            if (delta != 0) pos = target = std::clamp(target + delta, 0.0, limit);
        };
        move(pane_->scroll_y, pane_->target_y, -w.y * unit, max_scroll_y());
        move(pane_->scroll_x, pane_->target_x, w.x * unit, max_scroll_x());
        animating_ = pane_->scroll_x != pane_->target_x || pane_->scroll_y != pane_->target_y;
        if (animating_) last_step_ns_ = now;
    } else {
        pane_->target_y -= w.y * cfg_.scroll_lines * font_->cell_h();
        pane_->target_x += w.x * cfg_.scroll_lines * font_->cell_w();
        clamp_scroll();
        if (!animating_) last_step_ns_ = now - 8'000'000;
        animating_ = true;
    }
    dirty_ = true;
}

// Puts the middle of the scroll thumb at window y (window coordinates).
void App::drag_thumb(float y) {
    const double my = max_scroll_y();
    if (my <= 0) return;
    const double h = view_h();
    const double th = std::max(24.0 * scale_, h * (h / (my + h)));
    const double frac = std::clamp((y * density_ - pane_->text_y - th / 2) / std::max(1.0, h - th), 0.0, 1.0);
    pane_->scroll_y = pane_->target_y = frac * my;
    animating_ = false;
    dirty_ = true;
}

void App::on_mouse_down(const SDL_MouseButtonEvent& b) {
    if (b.button != SDL_BUTTON_LEFT) return;
    if (lsp_click(b.x, b.y, SDL_GetModState() & kCmd)) return;
    if (minimap_mouse(b.x, b.y, true)) return;  // the minimap: the view goes there (appminimap.cpp)
    // The strip along the right edge is the scroll bar.
    if (max_scroll_y() > 0 && b.x * density_ >= pane_->x1 - 12 * scale_) {
        dragging_thumb_ = true;
        drag_thumb(b.y);
        return;
    }
    text_press_ = true;
    const u64 off = offset_at_point(b.x, b.y);
    const SDL_Keymod mods = SDL_GetModState();
    if ((mods & SDL_KMOD_ALT) && !(mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_SHIFT)) && b.clicks == 1 && b.x * density_ >= pane_->text_x) {
        caret_click(b.x, b.y, false);  // a caret more (or one fewer); a drag from here, a column of them
        column_drag_ = true;
        return;
    }
    if (!(mods & SDL_KMOD_SHIFT)) pane_->doc->ed.clear_carets();
    if (b.x * density_ < pane_->text_x) {  // on a line's number: the whole line
        pane_->doc->ed.select_line(off);
    } else if (b.clicks == 2) {
        pane_->doc->ed.select_word(off);
    } else if (b.clicks >= 3) {
        pane_->doc->ed.select_line(off);
    } else {
        pane_->doc->ed.move_to(off, SDL_GetModState() & SDL_KMOD_SHIFT);
        dragging_ = true;
    }
    dirty_ = true;
}

void App::on_mouse_motion(const SDL_MouseMotionEvent& m) {
    if (dragging_thumb_) return drag_thumb(m.y);
    if (minimap_drag_) {
        minimap_mouse(m.x, m.y, false);
        return;
    }
    if (column_drag_) {
        caret_click(m.x, m.y, true);
        scroll_to_cursor();
        return;
    }
    if (!dragging_) {
        // Hover text is for the split the language server is in.
        const int px = static_cast<int>(m.x * density_), py = static_cast<int>(m.y * density_);
        if (pane_at(px, py) == pane_ && py >= pane_->text_y) lsp_mouse(m.x, m.y);
        else lsp_close(kPopHover);
        return;
    }
    pane_->doc->ed.move_to(offset_at_point(m.x, m.y), true);
    scroll_to_cursor();
}

void App::loop() {
    while (running_) {
        SDL_Event e;
        if (dirty_ || animating_ || fx_moving_) {
            while (SDL_PollEvent(&e)) handle(e);
        } else {
            // Idle: block here, using no CPU, until something happens, or
            // until the mouse has rested long enough for a hover popup, or a
            // scroll thumb has been left alone long enough to fade.
            int wait = -1;
            for (const int ms : {lsp_wait_ms(), git_wait_ms(), fx_wait_ms(), minimap_wait_ms(), spell_wait_ms(), watch_wait_ms(), script_ ? script_wait_ms() : -1})
                if (ms >= 0 && (wait < 0 || ms < wait)) wait = ms;
            if (wait < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, wait)) {
                handle(e);
                while (SDL_PollEvent(&e)) handle(e);
            }
            lsp_tick();
            git_tick();
            fx_tick();
            if (minimap_due_ns_ && now_ns() >= minimap_due_ns_) dirty_ = true;  // (typing has paused: the minimap made again)
            if (spell_wait_ms() == 0) dirty_ = true;  // (and the words on show checked)
            watch_tick();  // (what changed on disk has settled: the open files and git looked at)
        }
        if (script_) script_tick();  // timers, events, keys of a mapping that waited long enough
        if (animating_ || fx_moving_) {
            const u64 now = now_ns();
            const double dt = std::min(static_cast<double>(now - last_step_ns_) * 1e-9, 0.05);
            if (animating_) step_scroll(dt);
            if (fx_moving_) step_fx(dt);
            last_step_ns_ = now;
        }
        if (dirty_ && running_ && win_) {
            // The wait for the display comes first, and then whatever input
            // came in during it is taken in, so that it is in this frame
            // and not the next one (a key typed while the view glides or
            // scrolls shows a frame sooner).
            choose_images();
            pace();
            if (gpu_.acquire() == Renderer::kDrawn)
                while (running_ && SDL_PollEvent(&e)) handle(e);
            if (!running_) gpu_.release();
        }
        if (dirty_ && running_) frame();
        else gpu_.release();
        if (!instance_waits_.empty()) instance_check_waits();
    }
}

}  // namespace blit
