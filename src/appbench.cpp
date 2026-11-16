// Benchmarks and screenshots that run inside the real app, on the real GPU
// path. Each scenario prints one JSON object on stdout.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include "app.hpp"
#include "remote.hpp"

namespace blit {

namespace {

struct Series {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double pct(double p) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
    }
    double mean() const {
        double s = 0;
        for (double x : v) s += x;
        return v.empty() ? 0 : s / static_cast<double>(v.size());
    }
    void print(const char* name) {
        std::printf(",\"%s\":{\"mean\":%.2f,\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f}", name, mean(), pct(0.5),
                    pct(0.99), pct(1.0));
    }
};

double wall_seconds() {
    timespec ts;
    std::timespec_get(&ts, TIME_UTC);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

}  // namespace

struct Bench {
    App& a;

    void header(const char* name) {
        const char* file = a.opt_.file ? a.opt_.file : "";
        if (const char* slash = std::strrchr(file, '/')) file = slash + 1;
        std::printf("{\"file\":\"%s\",\"bench\":\"%s\",\"driver\":\"%s\",\"target\":\"%s %dx%d\",\"bytes\":%llu,\"lines\":%llu",
                    file, name, a.gpu_.driver(), a.win_ ? "window" : "offscreen", a.px_w_, a.px_h_,
                    static_cast<unsigned long long>(a.pane_->doc->ed.buf.size()),
                    static_cast<unsigned long long>(a.pane_->doc->ed.buf.line_count()));
    }

    // Draws `n` frames; `step(i)` moves the view or edits before each one.
    template <class Step>
    void frames(const char* name, int n, Step&& step) {
        Series fill, lay, cpu, edit, begin, upload, pass, submit;
        a.frame();  // warm up: pipelines, atlas upload, buffers
        a.gpu_.wait_idle();
        const u64 t0 = now_ns();
        for (int i = 0; i < n; ++i) {
            const u64 e0 = now_ns();
            step(i);
            const u64 e1 = now_ns();
            FrameStats st;
            a.frame(&st);
            fill.add(static_cast<double>(st.fill_ns) * 1e-3);
            lay.add(static_cast<double>(st.layout_ns) * 1e-3);
            cpu.add(static_cast<double>(st.cpu_ns) * 1e-3);
            edit.add(static_cast<double>(e1 - e0) * 1e-3);
            begin.add(static_cast<double>(st.begin_ns) * 1e-3);
            upload.add(static_cast<double>(st.upload_ns) * 1e-3);
            pass.add(static_cast<double>(st.pass_ns) * 1e-3);
            submit.add(static_cast<double>(st.submit_ns) * 1e-3);
            if (a.win_ && (i & 63) == 0) SDL_PumpEvents();
        }
        const u64 t1 = now_ns();
        a.gpu_.wait_idle();
        const u64 t2 = now_ns();
        header(name);
        std::printf(",\"frames\":%d", n);
        edit.print("step_us");
        fill.print("fill_us");
        lay.print("layout_us");
        cpu.print("frame_cpu_us");
        std::printf(",\"parts_us\":{\"begin\":%.2f,\"upload\":%.2f,\"pass\":%.2f,\"submit\":%.2f}", begin.mean(),
                    upload.mean(), pass.mean(), submit.mean());
        std::printf(",\"fps_submit\":%.0f,\"fps_gpu_done\":%.0f}\n", n / (static_cast<double>(t1 - t0) * 1e-9),
                    n / (static_cast<double>(t2 - t0) * 1e-9));
    }

    void snap() {
        a.clamp_scroll();
        a.pane_->scroll_x = a.pane_->target_x;
        a.pane_->scroll_y = a.pane_->target_y;
        a.animating_ = false;
    }

    int startup() {
        const u64 t_before = now_ns();
        a.frame();
        const u64 t_submit = now_ns();
        const bool coloured = a.pane_->doc->syntax.active() && !a.pane_->doc->syntax.highlights().empty();
        a.gpu_.wait_idle();
        const u64 t_done = now_ns();
        const double wall = wall_seconds();
        a.settle_syntax();  // if the colours were not in yet, when do they arrive?
        const u64 t_colours = now_ns();
        auto ms = [&](u64 t) { return static_cast<double>(t - a.opt_.t_main) * 1e-6; };
        header("startup");
        std::printf(",\"ms\":{\"sdl_init\":%.2f,\"window\":%.2f,\"gpu\":%.2f,\"font\":%.2f,\"file\":%.2f,"
                    "\"first_frame_submit\":%.2f,\"first_frame_done\":%.2f}",
                    ms(a.t_sdl_), ms(a.t_window_), ms(a.t_gpu_), ms(a.t_font_), ms(a.t_file_), ms(t_submit), ms(t_done));
        std::printf(",\"first_frame_cpu_ms\":%.2f", static_cast<double>(t_submit - t_before) * 1e-6);
        if (a.pane_->doc->syntax.active()) {
            const SyntaxStats st = a.pane_->doc->syntax.stats();
            std::printf(",\"syntax\":{\"in_first_frame\":%d,\"colours_after_first_frame_ms\":%.2f,\"setup_ms\":%.2f,\"parse_ms\":%.2f}",
                        coloured ? 1 : 0, coloured ? 0.0 : static_cast<double>(t_colours - t_submit) * 1e-6, st.setup_ms, st.parse_ms);
        }
        if (a.opt_.launch_wall > 0) std::printf(",\"exec_to_first_frame_ms\":%.2f", (wall - a.opt_.launch_wall) * 1e3);
        if (a.t_script_ns_) std::printf(",\"scripts_ms\":%.3f", static_cast<double>(a.t_script_ns_) * 1e-6);
        {
            u64 lib = 0, pipes = 0;
            bool cached = false;
            a.gpu_.setup_times(&lib, &pipes, &cached);
            std::printf(",\"gpu_setup\":{\"shaders_ms\":%.2f,\"pipelines_ms\":%.2f,\"pipelines_cached\":%d}", static_cast<double>(lib) * 1e-6, static_cast<double>(pipes) * 1e-6,
                        cached ? 1 : 0);
        }
        std::printf("}\n");
        return 0;
    }

    int scroll(const char* name) {
        const double ch = a.font_->cell_h();
        const double max = a.max_scroll_y();
        frames(name, a.opt_.frames > 0 ? a.opt_.frames : 3000, [&](int i) {
            a.pane_->target_y = max > 0 ? std::fmod(i * 3.37 * ch, max) : 0;
            snap();
        });
        return 0;
    }

    int jump() {
        const double ch = a.font_->cell_h();
        const u64 lines = a.pane_->doc->ed.buf.line_count();
        u64 x = 0x9E3779B97F4A7C15ull;
        frames("jump", a.opt_.frames > 0 ? a.opt_.frames : 2000, [&](int) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            a.pane_->target_y = static_cast<double>((x >> 20) % lines) * ch;
            snap();
        });
        return 0;
    }

    int hscroll() {
        // With --col the run starts far into the line, where every frame has
        // to find its way past everything to the left of the window.
        const double cw = a.font_->cell_w();
        const double base = a.pane_->scroll_x;
        frames(base > 0 ? "hscroll_far" : "hscroll", a.opt_.frames > 0 ? a.opt_.frames : 2000, [&](int i) {
            a.pane_->target_x = base + i * 7.3 * cw;
            snap();
        });
        return 0;
    }

    // Typing as it arrives: a text event per frame through the real
    // handlers (with Vim, through Vim: start it with --script text:i to be
    // in Insert mode, or without for the keys to be Normal-mode commands;
    // BLIT_BENCH_KEYS says which characters, over and over).
    int keys() {
        const char* env = SDL_getenv("BLIT_BENCH_KEYS");
        const std::string text = env && *env ? env : "the quick brown fox jumps over the lazy dog ";
        frames("keys", a.opt_.frames > 0 ? a.opt_.frames : 3000, [&](int i) {
            const char one[2] = {text[static_cast<size_t>(i) % text.size()], 0};
            SDL_Event e{};
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.text = one;
            a.handle(e);
            if (a.script_) a.script_tick();  // (as the loop does after each batch of input: the scripts' events)
            a.pane_->scroll_x = a.pane_->target_x;
            a.pane_->scroll_y = a.pane_->target_y;
            a.animating_ = false;
        });
        return 0;
    }

    int type() {
        static const char text[] = "the quick brown fox jumps over the lazy dog; int x = 42;\n";
        if (a.opt_.goto_line == 0) a.pane_->doc->ed.move_to(a.pane_->doc->ed.buf.line_start(a.pane_->doc->ed.buf.line_count() / 2), false);
        a.scroll_to_cursor();
        snap();
        frames("type", a.opt_.frames > 0 ? a.opt_.frames : 3000, [&](int i) {
            a.pane_->doc->ed.insert(std::string_view(&text[static_cast<size_t>(i) % (sizeof text - 1)], 1));
            a.scroll_to_cursor();
            snap();
        });
        return 0;
    }

    // Typing with a language server attached, through the same path a real
    // key takes: the edit, the change message, asking for completions and
    // signatures, and frames that draw whatever popups are open.
    int assist() {
        const u64 until = now_ns() + 10'000'000'000ull;
        a.start_lsp();
        while (!a.lsp_.ready() && a.lsp_.attached() && now_ns() < until) {
            a.sync_lsp();
            SDL_Delay(1);
        }
        if (!a.lsp_.ready()) {
            std::fprintf(stderr, "assist needs a language server for this file\n");
            return 1;
        }
        static const char text[] = "alpha = getValue(beta, alphabet); gamma_ray.setValue(delta);\ncall(alpha, get_width(), 3);\n";
        if (a.opt_.goto_line == 0) a.pane_->doc->ed.move_to(a.pane_->doc->ed.buf.line_start(a.pane_->doc->ed.buf.line_count() / 2), false);
        a.scroll_to_cursor();
        snap();
        u64 popups = 0;
        frames("assist", a.opt_.frames > 0 ? a.opt_.frames : 3000, [&](int i) {
            const std::string_view ch(&text[static_cast<size_t>(i) % (sizeof text - 1)], 1);
            a.pane_->doc->ed.insert(ch);
            a.scroll_to_cursor();
            a.lsp_typed(ch);
            snap();
            popups += a.lsp_popups_open();
        });
        (void)popups;
        return 0;
    }

    // Frames with the completion list, its side panel and signature help
    // all open, the selection moving every frame: what popups cost to lay
    // out and to draw.
    int popups() {
        a.start_lsp();
        auto pump = [&](int ms, auto done) {
            const u64 until = now_ns() + static_cast<u64>(ms) * 1'000'000;
            while (!done() && now_ns() < until) {
                a.sync_lsp();
                SDL_Delay(1);
            }
            return done();
        };
        if (!pump(10000, [&] { return a.lsp_.ready() || !a.lsp_.attached(); }) || !a.lsp_.ready()) {
            std::fprintf(stderr, "popups needs a language server for this file\n");
            return 1;
        }
        if (a.opt_.goto_line == 0) a.pane_->doc->ed.move_to(a.pane_->doc->ed.buf.line_start(a.pane_->doc->ed.buf.line_count() / 2), false);
        a.scroll_to_cursor();
        snap();
        for (const char* ch : {"c", "a", "l", "l", "(", "1", ",", " ", "a"}) {
            a.pane_->doc->ed.insert(ch);
            a.scroll_to_cursor();
            a.lsp_typed(ch);
            pump(60, [] { return false; });
        }
        snap();
        a.frame();
        const u32 open = a.lsp_popups_open();
        frames("popups", a.opt_.frames > 0 ? a.opt_.frames : 3000, [&](int) {
            SDL_KeyboardEvent k{};
            k.key = SDLK_DOWN;
            a.lsp_key(k);
        });
        std::fprintf(stderr, "popups: %u open\n", open);
        return open ? 0 : 1;
    }

    // Real window, vsync on: how evenly do frames arrive during a smooth scroll?
    int pace() {
        if (!a.win_) {
            std::fprintf(stderr, "pace needs a window\n");
            return 1;
        }
        const double seconds = a.opt_.frames > 0 ? a.opt_.frames : 4;
        const double ch = a.font_->cell_h();
        for (int i = 0; i < 90; ++i) {  // let the new window settle
            SDL_PumpEvents();
            a.frame();
        }
        Series gap, cpu, wait, p_fill, p_upload, p_pass, p_submit;
        u64 last = now_ns();
        const u64 t0 = last;
        u64 next_notch = t0;
        a.last_step_ns_ = t0;
        while (static_cast<double>(now_ns() - t0) * 1e-9 < seconds) {
            SDL_PumpEvents();
            const u64 now = now_ns();
            if (now >= next_notch) {  // a wheel notch every 60 ms
                a.pane_->target_y += 3 * ch;
                a.clamp_scroll();
                a.animating_ = true;
                next_notch += 60'000'000;
            }
            a.step_scroll(std::min(static_cast<double>(now - a.last_step_ns_) * 1e-9, 0.05));
            a.last_step_ns_ = now;
            FrameStats st;
            a.frame(&st);
            const u64 done = now_ns();
            gap.add(static_cast<double>(done - last) * 1e-6);
            cpu.add(static_cast<double>(st.cpu_ns) * 1e-3);
            wait.add(static_cast<double>(st.wait_ns) * 1e-6);
            p_fill.add(static_cast<double>(st.fill_ns) * 1e-3);
            p_upload.add(static_cast<double>(st.upload_ns) * 1e-3);
            p_pass.add(static_cast<double>(st.pass_ns) * 1e-3);
            p_submit.add(static_cast<double>(st.submit_ns) * 1e-3);
            last = done;
        }
        const std::vector<double> in_order = gap.v;
        const double median = gap.pct(0.5);
        int late = 0, early = 0;
        for (double g : gap.v) {
            late += g > median * 1.5;
            early += g < median * 0.5;
        }
        if (SDL_getenv("BLIT_PACE_TRACE")) {
            for (size_t i = 0; i < in_order.size(); ++i)
                std::fprintf(stderr, "%.1f%c", in_order[i] / median, (i % 40 == 39) ? '\n' : ' ');
            std::fputc('\n', stderr);
        }
        header("pace");
        std::printf(",\"frames\":%zu,\"late_frames\":%d,\"early_frames\":%d,\"hz\":%.1f", gap.v.size(), late, early,
                    1000.0 / median);
        gap.print("frame_gap_ms");
        wait.print("swapchain_wait_ms");
        cpu.print("frame_cpu_us");
        std::printf(",\"parts_us\":{\"fill\":%.2f,\"upload\":%.2f,\"pass\":%.2f,\"submit\":%.2f}", p_fill.pct(0.5),
                    p_upload.pct(0.5), p_pass.pct(0.5), p_submit.pct(0.5));
        std::printf("}\n");
        return 0;
    }

    // Real window: one typed character after a pause, the way typing really
    // arrives. Times the path from the event to the frame being handed to the
    // GPU, and to the GPU finishing it. The pause lets the CPU and GPU drop
    // into their idle states first, which is what a real keypress meets.
    int keypress() {
        if (!a.win_) {
            std::fprintf(stderr, "keypress needs a window\n");
            return 1;
        }
        const u64 settle_until = now_ns() + 1'000'000'000;
        while (now_ns() < settle_until) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) a.handle(e);
            if (a.dirty_) a.frame();
            SDL_Delay(5);
        }
        a.pane_->doc->ed.move_to(a.pane_->doc->ed.buf.line_start(a.pane_->doc->ed.buf.line_count() / 2), false);
        a.scroll_to_cursor();
        snap();
        a.frame();
        Series to_submit, to_done, acquire, k_edit, k_begin, k_fill, k_upload, k_pass, k_submit;
        const int n = a.opt_.frames > 0 ? a.opt_.frames : 150;
        u64 x = 88172645463325252ull;
        for (int i = 0; i < n; ++i) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            SDL_Delay(static_cast<Uint32>(20 + x % 40));
            SDL_PumpEvents();
            const u64 t0 = now_ns();
            SDL_Event e{};
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.text = (i % 60 == 59) ? "\n" : "k";
            a.handle(e);
            snap();
            const u64 t_edit = now_ns();
            FrameStats st;
            a.frame(&st);
            const u64 t1 = now_ns();
            k_edit.add(static_cast<double>(t_edit - t0) * 1e-3);
            k_begin.add(static_cast<double>(st.begin_ns) * 1e-3);
            k_fill.add(static_cast<double>(st.fill_ns) * 1e-3);
            k_upload.add(static_cast<double>(st.upload_ns) * 1e-3);
            k_pass.add(static_cast<double>(st.pass_ns) * 1e-3);
            k_submit.add(static_cast<double>(st.submit_ns) * 1e-3);
            a.gpu_.wait_idle();
            const u64 t2 = now_ns();
            to_submit.add(static_cast<double>(t1 - t0) * 1e-3);
            to_done.add(static_cast<double>(t2 - t0) * 1e-3);
            acquire.add(static_cast<double>(st.wait_ns) * 1e-3);
        }
        header("keypress");
        std::printf(",\"presses\":%d", n);
        to_submit.print("event_to_submit_us");
        acquire.print("of_which_swapchain_wait_us");
        to_done.print("event_to_gpu_done_us");
        std::printf(",\"parts_p50_us\":{\"edit\":%.1f,\"begin\":%.1f,\"swapchain\":%.1f,\"fill\":%.1f,\"upload\":%.1f,"
                    "\"pass\":%.1f,\"submit\":%.1f}",
                    k_edit.pct(0.5), k_begin.pct(0.5), acquire.pct(0.5), k_fill.pct(0.5), k_upload.pct(0.5), k_pass.pct(0.5),
                    k_submit.pct(0.5));
        std::printf("}\n");
        return 0;
    }

    // Real window, frames drawn one after another as while something moves,
    // and keys typed meanwhile by another thread at moments of their own.
    // Times each key from when it was typed to when the frame with it was
    // handed to the GPU: the input read before the wait for the display (as
    // it was), and read again after it (as the editor's loop does now),
    // in turns.
    int motionkeys() {
        if (!a.win_) {
            std::fprintf(stderr, "motionkeys needs a window\n");
            return 1;
        }
        a.pane_->doc->ed.move_to(a.pane_->doc->ed.buf.line_start(a.pane_->doc->ed.buf.line_count() / 4), false);
        a.scroll_to_cursor();
        a.frame();
        std::atomic<bool> stop{false};
        std::jthread typist([&] {
            u64 x = 88172645463325252ull;
            while (!stop.load()) {
                x ^= x << 13, x ^= x >> 7, x ^= x << 17;
                SDL_DelayNS(3'000'000 + x % 11'000'000);
                SDL_Event e{};
                e.type = SDL_EVENT_TEXT_INPUT;
                e.text.text = (x >> 20) % 2 ? "j" : "k";
                e.common.timestamp = SDL_GetTicksNS();
                SDL_PushEvent(&e);
            }
        });
        Series before, after, frame_ms;
        const int n = a.opt_.frames > 0 ? a.opt_.frames : 1200;
        std::vector<u64> typed;
        auto take = [&] {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_TEXT_INPUT) typed.push_back(e.common.timestamp);
                a.handle(e);
            }
        };
        u64 last = SDL_GetTicksNS();
        for (int i = 0; i < n; ++i) {
            const bool late = (i / 50) % 2 == 1;  // turn about, 50 frames each
            take();
            a.dirty_ = true;  // (always something to draw, as while the view moves)
            if (late) {
                a.pace();
                if (a.gpu_.acquire() == Renderer::kDrawn) take();
            }
            a.frame();
            const u64 t = SDL_GetTicksNS();
            for (const u64 at : typed) (late ? after : before).add(static_cast<double>(t - at) * 1e-3);
            typed.clear();
            frame_ms.add(static_cast<double>(t - last) * 1e-6);
            last = t;
        }
        stop = true;
        header("motionkeys");
        std::printf(",\"frames\":%d", n);
        before.print("read_before_wait_us");
        after.print("read_after_wait_too_us");
        frame_ms.print("frame_ms");
        std::printf("}\n");
        return 0;
    }

    // Real window, no input: an idle editor should draw nothing and burn nothing.
    int idle() {
        if (!a.win_) {
            std::fprintf(stderr, "idle needs a window\n");
            return 1;
        }
        // A new window keeps getting events for a moment (the window manager
        // places and sizes it). Let that pass, then measure true idle.
        const u64 settle_until = now_ns() + 1'500'000'000;
        while (now_ns() < settle_until) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) a.handle(e);
            if (a.dirty_) a.frame();
            SDL_Delay(5);
        }
        const double seconds = a.opt_.frames > 0 ? a.opt_.frames : 3;
        SDL_AddTimer(
            static_cast<Uint32>(seconds * 1000),
            [](void*, SDL_TimerID, Uint32) -> Uint32 {
                SDL_Event ev{};
                ev.type = SDL_EVENT_USER;
                ev.user.code = 99;
                SDL_PushEvent(&ev);
                return 0;
            },
            nullptr);
        const u64 frames0 = a.frames_;
        const std::clock_t c0 = std::clock();
        const u64 t0 = now_ns();
        a.loop();
        const double wall = static_cast<double>(now_ns() - t0) * 1e-9;
        const double cpu = static_cast<double>(std::clock() - c0) / CLOCKS_PER_SEC;
        header("idle");
        std::printf(",\"seconds\":%.2f,\"frames_drawn\":%llu,\"cpu_ms\":%.2f,\"cpu_percent\":%.3f}\n", wall,
                    static_cast<unsigned long long>(a.frames_ - frames0), cpu * 1e3, 100.0 * cpu / wall);
        return 0;
    }
};

int App::run_bench() {
    Bench b{*this};
    const char* name = opt_.bench;
    if (win_ && std::strcmp(name, "pace") != 0 && std::strcmp(name, "idle") != 0 && std::strcmp(name, "keypress") != 0 &&
        std::strcmp(name, "motionkeys") != 0 && std::strcmp(name, "startup") != 0)
        gpu_.set_present_mode(Renderer::kImmediate);
    if (!std::strcmp(name, "startup")) return b.startup();
    // A script sets the scene first: more tabs, splits, a side panel.
    if (opt_.script) {
        run_script(opt_.script);
        frame();
    }
    if (!std::strcmp(name, "scroll")) return b.scroll("scroll");
    if (!std::strcmp(name, "select")) {
        pane_->doc->ed.select_all();
        return b.scroll("select");
    }
    if (!std::strcmp(name, "jump")) return b.jump();
    if (!std::strcmp(name, "hscroll")) return b.hscroll();
    if (!std::strcmp(name, "type")) return b.type();
    if (!std::strcmp(name, "keys")) return b.keys();
    if (!std::strcmp(name, "assist")) return b.assist();
    if (!std::strcmp(name, "popups")) return b.popups();
    if (!std::strcmp(name, "pace")) return b.pace();
    if (!std::strcmp(name, "keypress")) return b.keypress();
    if (!std::strcmp(name, "motionkeys")) return b.motionkeys();
    if (!std::strcmp(name, "idle")) return b.idle();
    std::fprintf(stderr, "unknown benchmark: %s\n", name);
    return 1;
}

// Replays input through the same handlers real events use. Events are
// separated by ';':
//   text:STRING            typed text
//   key:[shift+][cmd+][word+]NAME   a key press, NAME as SDL names it (Left, Return, A, ...)
//   wheel:N                N mouse wheel notches (negative scrolls down)
//   click:X,Y[,COUNT]      left click at window coordinates; drag:X,Y; release
//   save                   write the file
//   frame                  draw a frame now (the script otherwise draws only at the end)
//   col:N / line:N         scroll so that column N / line N is at the window edge
//   mouse:X,Y              move the mouse there
//   rclick:X,Y             right click
//   panel                  wait for the side panel's file tree to finish reading folders
//   open:PATH              open a file (in a tab of its own)
//   workdir:PATH           make a folder the working directory (the one the side panel shows)
//   vim:KEYS               keys in Vim's notation (dw, <Esc>, <C-r>); vimdump prints the mode, the bottom line,
//                          the cursor's line:column and the text of its line
//   dump[:LABEL]           print how the window is split, and the tabs of each split
//   git:ACTION[:ARG]       a git action by name (stage:PATH, commit:MESSAGE, push, switch:BRANCH, ...)
//   gitwait                wait for git to finish; gitdump[:LABEL] also prints what it has told the editor
//   pan:DX,DY              a touchpad stroke; step:MS lets MS milliseconds of scrolling pass
//   ready                  wait for the language server to be up (10 s at most)
//   diag                   wait for the server's next set of diagnostics (10 s at most)
//   wait:MS                let MS milliseconds pass, taking in whatever the server sends
// Scrolling is settled instantly, so the next event sees the final view.
void App::run_script(const char* script) {
    std::string_view rest = script;
    while (!rest.empty()) {
        const size_t semi = rest.find(';');
        std::string_view item = rest.substr(0, semi);
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
        const size_t colon = item.find(':');
        const std::string_view verb = item.substr(0, colon);
        const std::string arg(colon == std::string_view::npos ? std::string_view{} : item.substr(colon + 1));
        SDL_Event e{};
        if (verb == "text") {
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.text = arg.c_str();
            handle(e);
        } else if (verb == "key") {
            std::string name = arg;
            Uint16 mod = 0;
            for (;;) {
                if (name.starts_with("shift+")) mod |= SDL_KMOD_LSHIFT, name.erase(0, 6);
                else if (name.starts_with("cmd+")) mod |= static_cast<Uint16>(kPrimaryMod), name.erase(0, 4);
                else if (name.starts_with("word+")) mod |= static_cast<Uint16>(kWordMod), name.erase(0, 5);
                else if (name.starts_with("ctrl+")) mod |= SDL_KMOD_LCTRL, name.erase(0, 5);
                else if (name.starts_with("alt+")) mod |= SDL_KMOD_LALT, name.erase(0, 4);
                else break;
            }
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = SDL_GetKeyFromName(name.c_str());
            e.key.mod = mod;
            e.key.down = true;
            if (e.key.key == SDLK_UNKNOWN) std::fprintf(stderr, "script: unknown key %s\n", name.c_str());
            handle(e);
        } else if (verb == "wheel") {
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.y = static_cast<float>(std::atof(arg.c_str()));
            precise_until_ns_ = 0;
            handle(e);
        } else if (verb == "ready" || verb == "diag" || verb == "wait") {
            if (lsp_pending_) {
                lsp_pending_ = false;
                start_lsp();
            }
            const u64 gen = lsp_.diagnostics_generation();
            const u64 until = now_ns() + (verb == "wait" ? static_cast<u64>(std::atof(arg.c_str()) * 1e6) : 10'000'000'000ull);
            if (grammars_pending_) {
                grammars_pending_ = false;
                want_grammars();
            }
            while (now_ns() < until) {
                sync_lsp();
                sync_grammars();
                lsp_tick();
                if (verb == "ready" && (lsp_.ready() || !lsp_.attached())) break;
                if (verb == "diag" && (lsp_.diagnostics_generation() != gen || !lsp_.attached())) break;
                SDL_Delay(2);
            }
            sync_lsp();
        } else if (verb == "panel") {  // let the file tree finish reading folders (2 s at most)
            const u64 until = now_ns() + 2'000'000'000ull;
            for (int quiet = 0; quiet < 30 && now_ns() < until;) {
                const bool was_dirty = dirty_;
                dirty_ = false;
                sync_panel();
                quiet = dirty_ ? 0 : quiet + 1;
                dirty_ = dirty_ || was_dirty;
                SDL_Delay(2);
            }
        } else if (verb == "rclick") {
            float x = 0, y = 0;
            std::sscanf(arg.c_str(), "%f,%f", &x, &y);
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.button = SDL_BUTTON_RIGHT;
            e.button.x = x;
            e.button.y = y;
            e.button.clicks = 1;
            handle(e);
        } else if (verb == "click" || verb == "drag" || verb == "mouse") {
            float x = 0, y = 0;
            int clicks = 1;
            std::sscanf(arg.c_str(), "%f,%f,%d", &x, &y, &clicks);
            if (verb == "click") {
                e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                e.button.button = SDL_BUTTON_LEFT;
                e.button.x = x;
                e.button.y = y;
                e.button.clicks = static_cast<Uint8>(clicks);
            } else {
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.x = x;
                e.motion.y = y;
            }
            handle(e);
        } else if (verb == "release") {
            e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.button = SDL_BUTTON_LEFT;
            handle(e);
        } else if (verb == "save") {
            save();
        } else if (verb == "open") {
            open_path(arg.c_str());
        } else if (verb == "vim") {  // keys in Vim's notation, straight to Vim
            vim_attach();
            vim_.keys(pane_->doc->ed, arg);
            vim_after(Vim::kHandled);
        } else if (verb == "vimdump") {
            std::fprintf(stderr, "%s\n", vim_dump().c_str());
        } else if (verb == "startdump") {
            std::fprintf(stderr, "%s\n", start_dump().c_str());
        } else if (verb == "statusdump") {
            std::fprintf(stderr, "%s\n", status_dump().c_str());
        } else if (verb == "workdir") {  // what choosing a folder in the side panel does
            panel_set_workdir(arg);
        } else if (verb == "git" || verb == "gitwait" || verb == "gitdump") {
            // git:ACTION[:ARGUMENT] does what the Git tab would do, by name.
            // Around it, and for gitwait and gitdump, git is waited for (20 s
            // at most): until what was asked for is done, the status has
            // been read again, and the changed lines and the blame of the
            // files on show are in.
            auto settle = [&] {
                git_follow();
                const u64 until = now_ns() + 20'000'000'000ull;
                for (int quiet = 0; quiet < 2 && now_ns() < until;) {
                    sync_git();
                    git_tick();
                    if (git_.idle() && !git_due_ns_) {
                        frame();  // which may ask for more
                        sync_git();
                        quiet = git_.idle() && !git_due_ns_ ? quiet + 1 : 0;
                    } else {
                        quiet = 0;
                        SDL_Delay(2);
                    }
                }
            };
            settle();
            if (verb == "git") {
                const size_t c2 = arg.find(':');
                git_command(arg.substr(0, c2), c2 == std::string::npos ? std::string() : arg.substr(c2 + 1));
                settle();
            }
            if (verb == "gitdump") std::fprintf(stderr, "%s%s\n", arg.empty() ? "" : (arg + ": ").c_str(), git_dump().c_str());
        } else if (verb == "dump") {
            // How the window is split, on one line: a split is its tabs in
            // brackets, the one in front starred and the split with the
            // keyboard marked ">"; H(..|..) is two side by side, V(../..)
            // one above the other.
            auto show = [&](auto&& self, const SplitNode* n) -> std::string {
                if (!n) return "?";
                if (n->pane) {
                    std::string out = n->pane == pane_ ? ">[" : "[";
                    for (const std::unique_ptr<Doc>& d : n->pane->docs) {
                        const std::string& path = d->ed.path;
                        if (out.back() != '[') out += ' ';
                        out += !d->title.empty() ? d->title : path.empty() ? "untitled" : path.substr(path.find_last_of('/') + 1);
                        if (d.get() == n->pane->doc) out += '*';
                    }
                    // (and the line at the top of its view, when that is not the first)
                    const long top = std::lround(n->pane->target_y / font_->cell_h()) + 1;
                    return out + (top > 1 ? "]@" + std::to_string(top) : "]");
                }
                char share[16];
                std::snprintf(share, sizeof share, ")%.2f", static_cast<double>(n->share));
                return std::string(n->side_by_side ? "H(" : "V(") + self(self, n->a.get()) + (n->side_by_side ? " | " : " / ") +
                       self(self, n->b.get()) + share;
            };
            std::fprintf(stderr, "%s%s\n", arg.empty() ? "" : (arg + ": ").c_str(), show(show, split_.get()).c_str());
        } else if (verb == "pan" || verb == "step") {
            // A touchpad stroke of so many wheel units (the view follows it
            // exactly and may be pulled past an end), or so many
            // milliseconds of the view easing back. Neither is settled
            // afterwards: what they leave is what the next event sees.
            if (verb == "pan") {
                float dx = 0, dy = 0;
                std::sscanf(arg.c_str(), "%f,%f", &dx, &dy);
                e.type = SDL_EVENT_MOUSE_WHEEL;
                e.wheel.x = dx + 0.001f;  // a fraction: this is no wheel notch
                e.wheel.y = dy + 0.001f;
                handle(e);
            } else {
                step_scroll(std::atof(arg.c_str()) * 1e-3);
                step_fx(std::atof(arg.c_str()) * 1e-3);
            }
            continue;
        } else if (verb == "find") {  // the box that finds things, for a purpose that has no key of its own
            find_open(arg == "folder" ? kFindFolder : arg == "save" ? kFindSave : arg == "open" ? kFindOpen : arg == "text" ? kFindText : kFindNames);
        } else if (verb == "remotewait") {  // files on their way from other machines are here (or have failed)
            const u64 until = now_ns() + 60'000'000'000ull;
            for (;;) {
                remote_sync();
                bool loading = false;
                for (const std::unique_ptr<Pane>& p : panes_)
                    for (const std::unique_ptr<Doc>& d : p->docs) loading |= d->loading;
                bool connecting = false;
                for (const std::shared_ptr<Remote>& r : Remote::all()) connecting |= r->state() == Remote::kConnecting;
                if (!Remote::asks().empty()) {  // ssh has a question: the script answers it
                    remote_sync();
                    break;
                }
                if ((!loading && !connecting && !remote_busy()) || now_ns() > until) break;
                SDL_Delay(2);
            }
        } else if (verb == "prof") {  // prof:what=value (program, args, cwd, dwarf, annotate)
            const size_t eq = arg.find('=');
            prof_set(arg.substr(0, eq), eq == std::string::npos ? std::string() : arg.substr(eq + 1));
        } else if (verb == "profpanel") {  // the side panel's Profile tab
            if (!panel_px()) panel_toggle();
            panel_show_profile();
        } else if (verb == "profrecord") {
            prof_record();
        } else if (verb == "profstop") {
            prof_stop();
        } else if (verb == "profwait") {  // the recording has finished (two minutes at most)
            const u64 until = now_ns() + 120'000'000'000ull;
            while (prof_busy() && now_ns() < until) {
                prof_sync();
                SDL_Delay(5);
            }
            prof_sync();
        } else if (verb == "profdump") {
            std::fprintf(stderr, "%s\n", prof_dump().c_str());
        } else if (verb == "term") {  // a terminal (term:folder, or in the folder being worked in)
            term_open(arg);
        } else if (verb == "termwait") {  // the terminal in front shows this text (10 s at most)
            if (!term_wait(arg)) std::fprintf(stderr, "termwait: no \"%s\" on the screen\n", arg.c_str());
            term_sync();
        } else if (verb == "debug") {  // debug:what=value (program, args, cwd)
            const size_t eq = arg.find('=');
            debug_set(arg.substr(0, eq), eq == std::string::npos ? std::string() : arg.substr(eq + 1));
        } else if (verb == "break") {  // break:N, a breakpoint on line N of the file in front (F9 there)
            Editor& ed = pane_->doc->ed;
            ed.move_to(ed.buf.line_start(std::min<u64>(static_cast<u64>(std::max(1, std::atoi(arg.c_str()))) - 1, ed.buf.line_count() - 1)), false);
            debug_toggle_break();
        } else if (verb == "debugwait") {  // debugwait:stopped, or debugwait:idle
            debug_wait(arg);
        } else if (verb == "debugdump") {
            std::fprintf(stderr, "%s\n", debug_dump().c_str());
        } else if (verb == "newproject") {  // the start screen's New project tab
            project_page(true);
        } else if (verb == "project") {  // project:key=value (kind, name, where, git, or a field of the kind's)
            const size_t eq = arg.find('=');
            project_set(arg.substr(0, eq), eq == std::string::npos ? std::string() : arg.substr(eq + 1));
        } else if (verb == "projectcreate") {
            project_create();
        } else if (verb == "projectdump") {
            std::fprintf(stderr, "%s\n", project_dump().c_str());
        } else if (verb == "buildpanel") {  // the side panel's Build tab
            if (!panel_px()) panel_toggle();
            panel_show_build();
        } else if (verb == "builddump") {  // the tasks found (by system; > the Run button's), and how the last run went
            std::fprintf(stderr, "%s\n", build_dump().c_str());
        } else if (verb == "buildrun") {  // buildrun:name runs that task (buildrun alone: the Run button's)
            build_settle();
            if (!build_run_named(arg)) std::fprintf(stderr, "buildrun: no \"%s\"\n", arg.c_str());
        } else if (verb == "buildwait") {  // the task run has ended (60 s at most)
            build_wait();
        } else if (verb == "commands") {  // every command, a line each: its name, a tab, its keys
            for (const PaletteItem& it : palette_items()) std::fprintf(stderr, "%s\t%s\n", it.title.c_str(), it.keys.c_str());
        } else if (verb == "command") {  // command:name runs that command, as the command search would
            bool ran = false;
            const std::vector<PaletteItem> items = palette_items();
            for (int pass = 0; pass < 2 && !ran; ++pass)  // (the name whole, else the first that starts so: a script's can have a ; in it)
                for (const PaletteItem& it : items)
                    if (pass == 0 ? it.title == arg : it.title.starts_with(arg)) {
                        palette_run(it.id);
                        ran = true;
                        break;
                    }
            if (!ran) std::fprintf(stderr, "command: no \"%s\"\n", arg.c_str());
        } else if (verb == "termend") {  // the terminal's program has ended (10 s at most)
            term_end();
        } else if (verb == "termdump") {
            term_sync();
            std::fprintf(stderr, "%s\n", term_dump().c_str());
        } else if (verb == "remotedump") {  // each connection, and how it is
            std::string out;
            for (const std::shared_ptr<Remote>& r : Remote::all()) {
                const Remote::State st = r->state();
                out += r->host() + (st == Remote::kReady ? ": ready " + r->system() : st == Remote::kFailed ? ": failed: " + r->error() : ": connecting") + "; ";
            }
            std::fprintf(stderr, "%s\n", out.empty() ? "no connections" : out.c_str());
        } else if (verb == "findwait") {  // the worker that finds files and text has finished
            find_settle();
        } else if (verb == "finddump") {
            find_settle();
            std::fprintf(stderr, "%s%s\n", arg.empty() ? "" : (arg + ": ").c_str(), find_dump().c_str());
        } else if (verb == "motion") {  // from here on things move as they do in a window (see step)
            fx_live_ = true;
            continue;
        } else if (verb == "frame") {
            settle_syntax();
            frame();
        } else if (verb == "col") {
            pane_->target_x = std::atof(arg.c_str()) * font_->cell_w();
        } else if (verb == "line") {
            pane_->target_y = (std::atof(arg.c_str()) - 1) * font_->cell_h();
        } else if (verb == "lua") {  // Lua, run as :lua would (no ";" in it: that ends the event)
            script_string(arg, "=script");
        } else if (verb == "luafile") {
            script_file(arg);
        } else if (verb == "luawait") {  // the scripts' timers and programs: MS milliseconds of them, or until none is left (10 s at most)
            const bool timed = !arg.empty();
            const u64 until = now_ns() + (timed ? static_cast<u64>(std::atof(arg.c_str()) * 1e6) : 10'000'000'000ull);
            while (script_ && now_ns() < until) {
                script_jobs();
                script_tick();
                if (!timed && !script_busy()) break;
                SDL_Delay(1);
            }
        } else if (verb == "luadump") {
            std::fprintf(stderr, "%s%s\n", arg.empty() ? "" : (arg + ": ").c_str(), script_dump().c_str());
        } else if (!verb.empty()) {
            std::fprintf(stderr, "script: unknown event %.*s\n", static_cast<int>(verb.size()), verb.data());
        }
        if (script_) script_tick();  // (what a real loop does after each batch of input)
        clamp_scroll();
        pane_->scroll_x = pane_->target_x;
        pane_->scroll_y = pane_->target_y;
        animating_ = false;
    }
}

int App::run_shot() {
    if (opt_.off_w == 0 && !gpu_.set_offscreen(px_w_, px_h_)) return 1;
    if (opt_.script) run_script(opt_.script);
    settle_syntax();  // the picture should not depend on how fast the worker was
    if (frame() != Renderer::kDrawn) return 1;
    std::vector<u8> rgba;
    int w = 0, h = 0;
    if (!gpu_.read_pixels(rgba, &w, &h)) return 1;
    SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);
    const bool ok = s && SDL_SaveBMP(s, opt_.shot);
    SDL_DestroySurface(s);
    if (!ok) std::fprintf(stderr, "blit: cannot write %s: %s\n", opt_.shot, SDL_GetError());
    return ok ? 0 : 1;
}

}  // namespace blit
