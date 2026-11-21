// Benchmarks and screenshots that run inside the real app, on the real GPU
// path. Each scenario prints one JSON object on stdout.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <unistd.h>

#include "app.hpp"
#include "fsio.hpp"
#include "profile.hpp"
#include "remote.hpp"

namespace blit {

namespace {
float script_mouse_x = 0, script_mouse_y = 0;  // (where the scripted mouse last was)
}  // namespace

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

    // The window divided into BLIT_BENCH_VIEWS splits (16, at most
    // kMaxViews), each showing the file (a document of its own with the
    // same text), all of them scrolling at once: what a frame costs with
    // that many views ("splits-16").
    int splits() {
        const char* env = SDL_getenv("BLIT_BENCH_VIEWS");
        const int want = std::clamp(env && *env ? std::atoi(env) : static_cast<int>(kMaxViews), 1, static_cast<int>(kMaxViews));
        for (int i = 1; i < want; ++i) {
            Pane* p = a.split_pane(a.pane_, i % 2 ? App::kEdgeRight : App::kEdgeBottom);
            if (!p) break;
            a.focus_pane(p);
            Doc* d = a.add_doc(0);
            std::string err;
            if (a.opt_.file) d->ed.open(a.opt_.file, &err);
            a.tab_show(0);
        }
        const double ch = a.font_->cell_h();
        const std::string name = "splits-" + std::to_string(a.panes_.size());
        frames(name.c_str(), a.opt_.frames > 0 ? a.opt_.frames : 2000, [&](int i) {
            a.for_each_pane([&] {
                const double max = a.max_scroll_y();
                a.pane_->target_y = max > 0 ? std::fmod(i * 3.37 * ch, max) : 0;
                snap();
            });
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
        // places and sizes it), and a scroll thumb fades away once it has
        // rested. Let all that pass (but for five seconds at most), then
        // measure true idle.
        const u64 settle_from = now_ns() + 1'500'000'000, give_up = now_ns() + 5'000'000'000;
        for (;;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) a.handle(e);
            a.fx_tick();
            if (a.animating_ || a.fx_moving_) {
                const u64 now = now_ns();
                const double dt = std::min(static_cast<double>(now - a.last_step_ns_) * 1e-9, 0.05);
                if (a.animating_) a.step_scroll(dt);
                if (a.fx_moving_) a.step_fx(dt);
                a.last_step_ns_ = now;
            }
            if (a.dirty_) a.frame();
            const u64 now = now_ns();
            if (now >= give_up || (now >= settle_from && !a.dirty_ && !a.animating_ && !a.fx_moving_ && a.fx_wait_ms() < 0)) break;
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
    if (!std::strcmp(name, "splits")) return b.splits();
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
//   key:[shift+][cmd+][word+]NAME   a key press, NAME as SDL names it (Left, Return, A, ...); cmd is the editor's key
//                          (Cmd on a Mac, Ctrl elsewhere), word the one that moves by words; ctrl, alt and gui are those keys
//   keyof:COMMAND          the key that does a command now (as the command search names it): its first that Vim would not take
//   wheel:N                N mouse wheel notches (negative scrolls down)
//   click:X,Y[,COUNT][,alt]  left click at window coordinates (,alt: with Option held); drag:X,Y[,alt]; release
//   save                   write the file
//   frame                  draw a frame now (the script otherwise draws only at the end)
//   col:N / line:N         scroll so that column N / line N is at the window edge
//   mouse:X,Y              move the mouse there
//   rclick:X,Y             right click
//   panel                  wait for the side panel's file tree to finish reading folders
//   open:PATH              open a file (in a tab of its own)
//   workdir:PATH           make a folder the working directory (the one the side panel shows)
//   folder:PATH            open a folder to work in (its session comes back; see appsession.cpp)
//   quit                   what Cmd+Q does; the session is written as the run ends
//   sessiondump            print the session the project folder would be written with now
//   vim:KEYS               keys in Vim's notation (dw, <Esc>, <C-r>); vimdump prints the mode, the bottom line,
//                          the cursor's line:column and the text of its line
//   dump[:LABEL]           print how the window is split, and the tabs of each split
//   views                  print each split's view of the file in front: its document, its own cursor and that line
//   emojidump              print the colour emoji the last frame drew: how many, the cells of each, in the text or a box
//   rowsdump:N             draw a frame and print the first N rows of the split in front's text as drawn, each after
//                          a "|" (ASCII as it is, anything else #)
//   usesdump               print the uses of the symbol at the cursor that are marked (line:column ranges, from 1)
//   rulerdump              print the marks along the scroll bar of the split in front: place (a third of a row):kind
//                          (> the cursor, e error, b breakpoint, w warning, f a match, c a change), or none
//   folddump               print what is folded in the file in front (header-last, from 1) and its first rows' lines
//   stickydump             print the lines pinned at the top of the split in front (from 1), or none
//   tabsdump               print what the tabs of the split in front say, " | " between them
//   crumbsdump             print the breadcrumbs under the split in front's tabs, " > " between them
//   coveragedump           print the lines the last run with coverage ran and missed in the file in front, and each file's share
//   testdump               print the project's tests by group (+ passed, x failed, - skipped, . not run), once found and run
//   runconfdump            print the run configuration chosen, and what Debug and Profile would run with it
//   seldump                print what is selected in the file in front, in [ ], its lines joined by " / " (or none)
//   shapedump              print the brackets on show in the file in front (line:column=depth, "-" not the tree's)
//                          and the pair marked at the cursor
//   git:ACTION[:ARG]       a git action by name (stage:PATH, commit:MESSAGE, push, switch:BRANCH, ...)
//   gitwait                wait for git to finish; gitdump[:LABEL] also prints what it has told the editor
//   gitview[:LABEL]        print the diff tab in front: its hunks, and its lines with what is coloured added or taken away
//   gitfield               print the rows of the commit message as its field shows them
//   gutterclick:N          a click on the change's mark beside line N of the file in front
//   breakclick:N           a click in the gutter's first cell beside line N (where a breakpoint's dot goes)
//   gutterdump             print the rows of the line numbers in front: each one's number (none on a wrapped line's
//                          later rows) and change mark (+ added, ~ changed, - lines taken out, . neither)
//   pan:DX,DY              a touchpad stroke; step:MS lets MS milliseconds of scrolling pass
//   ready                  wait for the language server to be up (10 s at most)
//   diag                   wait for the server's next set of diagnostics (10 s at most)
//   wait:MS                let MS milliseconds pass, taking in whatever the server sends
//   lspsettle              wait until every tab that can have a server has one that is up, and the servers
//                          have gone quiet (10 s at most)
//   lspdump                print the language servers (name, project, documents) and which tabs are on one
//   lspidle:MS             a server with no documents left stops after MS milliseconds
//   lsptext                print the text of the file in front, its lines joined by " / "
//   lspextras[:colours]    print the inlay hints on show in the file in front, and the servers' colours there
//                          (:colours, with the colour each is drawn in, #rrggbb)
//   compdump               print the completion list's rows (label and detail), joined by " | "
//   asked                  print the last question a box would have asked (scripted runs answer yes, or Don't Save)
//   answer:save            closing a tab or quitting with changes is answered Save from here on (answer:dontsave: back)
//   clock; within:MS       start a stopwatch; print whether the main thread got here within MS milliseconds
//   lap:LABEL              print the milliseconds since the stopwatch started (or the last lap), and start it again
//   treewait               wait for the side panel's tree to finish reading folders and changing files (20 s at most)
//   alive                  print whether blit is still running, or has been told to quit
//   gitbusy                wait until git is running for the file in front (20 s at most)
//   remotelost             wait for a connection to another machine to have gone (10 s at most)
//   remotestream:BYTES     a program on the machine of the file in front writes that many bytes, read here only after a
//                          second; print how many came and whether no more than a few MB were held here meanwhile
// Scrolling is settled instantly, so the next event sees the final view.
void App::run_script(const char* script) {
    std::string_view rest = script;
    u64 clock_ns = now_ns();
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
                else if (name.starts_with("gui+")) mod |= SDL_KMOD_LGUI, name.erase(0, 4);
                else break;
            }
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = SDL_GetKeyFromName(name.c_str());
            e.key.mod = mod;
            e.key.down = true;
            if (e.key.key == SDLK_UNKNOWN) std::fprintf(stderr, "script: unknown key %s\n", name.c_str());
            handle(e);
        } else if (verb == "keyof") {
            if (command_key_now(arg, &e.key)) handle(e);
            else std::fprintf(stderr, "keyof: no \"%s\"\n", arg.c_str());
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
        } else if (verb == "lspsettle") {
            const u64 until = now_ns() + 10'000'000'000ull;
            u64 quiet_since = now_ns(), gen = ~0ull;
            while (now_ns() < until) {
                if (lsp_pending_) {
                    lsp_pending_ = false;
                    start_lsp();
                }
                sync_lsp();
                lsp_tick();
                const std::string d = lsp_.servers_dump();
                const u64 g = lsp_.diagnostics_generation() + lsp_.problems_generation();
                if (g != gen || d.find("starting") != std::string::npos || lsp_pending_) gen = g, quiet_since = now_ns();
                else if (now_ns() - quiet_since > 150'000'000ull) break;
                SDL_Delay(2);
            }
        } else if (verb == "lspdump") {
            std::fprintf(stderr, "%s\n", lsp_dump().c_str());
        } else if (verb == "hoverdump") {  // the hover box: its rows and where it is down the window (after a frame)
            std::fprintf(stderr, "%s\n", hover_dump().c_str());
        } else if (verb == "lsptext") {
            std::string t = pane_->doc->ed.buf.text(0, pane_->doc->ed.buf.size());
            for (size_t at = t.find('\n'); at != std::string::npos; at = t.find('\n', at + 3)) t.replace(at, 1, " / ");
            std::fprintf(stderr, "%s\n", t.c_str());
        } else if (verb == "lspextras") {
            std::fprintf(stderr, "%s\n", lsp_extras_dump(arg == "colours").c_str());
        } else if (verb == "compdump") {  // the completion list's rows: label and detail
            std::fprintf(stderr, "%s\n", completion_dump().c_str());
        } else if (verb == "cpuquiet") {  // cpuquiet:MS, the CPU time every thread took while the main one slept MS ms: "quiet" under a quarter of it
            const std::clock_t c0 = std::clock();
            const u64 t0 = now_ns();
            SDL_Delay(static_cast<Uint32>(std::atoi(arg.c_str())));
            const double cpu = static_cast<double>(std::clock() - c0) / CLOCKS_PER_SEC, wall = static_cast<double>(now_ns() - t0) * 1e-9;
            if (cpu < wall / 4) std::fprintf(stderr, "quiet\n");
            else std::fprintf(stderr, "busy (%.0f%%)\n", 100.0 * cpu / wall);
        } else if (verb == "lspidle") {
            lsp_.set_idle_stop_ms(static_cast<u64>(std::atoll(arg.c_str())));
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
            // (",alt", ",cmd" (Ctrl off a Mac) or ",shift" at its end: with
            // that held, as long as the event takes.)
            const SDL_Keymod with = arg.ends_with(",alt") ? SDL_KMOD_ALT : arg.ends_with(",cmd") ? kPrimaryMod : arg.ends_with(",shift") ? SDL_KMOD_SHIFT : SDL_KMOD_NONE;
            const bool alt = with != SDL_KMOD_NONE;
            const SDL_Keymod held = SDL_GetModState();
            if (alt) SDL_SetModState(static_cast<SDL_Keymod>(held | with));
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
                if (verb == "drag") e.motion.state = SDL_BUTTON_LMASK;  // (as a real drag says: the button is held)
            }
            handle(e);
            if (alt) SDL_SetModState(held);
            script_mouse_x = x;
            script_mouse_y = y;
        } else if (verb == "release") {  // (where the mouse last was, as a real one says)
            e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.x = script_mouse_x;
            e.button.y = script_mouse_y;
            handle(e);
        } else if (verb == "save") {
            save();
        } else if (verb == "open") {
            if (find_open_) find_close(false);  // (as a click elsewhere would: a box with the keyboard has nothing to do with it)
            open_path(arg.c_str());
        } else if (verb == "vim") {  // keys in Vim's notation, straight to Vim
            vim_attach();
            vim_.keys(pane_->doc->ed, arg);
            vim_after(Vim::kHandled);
        } else if (verb == "vimdump") {
            std::fprintf(stderr, "%s\n", vim_dump().c_str());
        } else if (verb == "vimline") {  // the bottom line as the last frame drew it: left | right
            std::fprintf(stderr, "%s\n", vim_line_dump().c_str());
        } else if (verb == "startdump") {
            std::fprintf(stderr, "%s\n", start_dump().c_str());
        } else if (verb == "statusdump") {
            std::fprintf(stderr, "%s\n", status_dump().c_str());
        } else if (verb == "workdir") {  // what choosing a folder in the side panel does
            panel_set_workdir(arg);
        } else if (verb == "folder") {  // a folder opened to work in, as the start screen's Recent opens one
            project_open(arg);
        } else if (verb == "quit") {  // what Cmd+Q does (the session is written when the run ends)
            request_quit();
        } else if (verb == "sessiondump") {
            std::fprintf(stderr, "%s\n", session_dump().c_str());
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
        } else if (verb == "gitgraph") {  // History as the Git tab draws it: lanes, what points at each commit, its subject
            std::fprintf(stderr, "%s\n", git_graph_dump().c_str());
        } else if (verb == "rebasedump") {  // the rebase tab in front: each commit's action and subject
            std::fprintf(stderr, "%s\n", git_rebase_dump().c_str());
        } else if (verb == "mergedump") {  // the merge tab in front: its changes, what is taken of each conflict, and how many are open
            std::fprintf(stderr, "%s\n", git_merge_dump().c_str());
        } else if (verb == "gitview") {
            std::fprintf(stderr, "%s%s\n", arg.empty() ? "" : (arg + ": ").c_str(), git_view_dump().c_str());
        } else if (verb == "gitfield") {
            std::fprintf(stderr, "%s\n", git_field_dump().c_str());
        } else if (verb == "gutterclick" || verb == "breakclick") {  // (where the gutter last drew that line's mark: its last cell; or its first)
            const double ch = font_->cell_h(), cw = font_->cell_w();
            const double py = pane_->text_y + (std::atof(arg.c_str()) - 1) * ch - pane_->scroll_y + ch / 2,
                         px = verb == "breakclick" ? pane_->x0 + cw / 2 : pane_->text_x - cw / 2;
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.x = static_cast<float>(px / density_);
            e.button.y = static_cast<float>(py / density_);
            e.button.clicks = 1;
            handle(e);
            SDL_Event up{};
            up.type = SDL_EVENT_MOUSE_BUTTON_UP;
            up.button.button = SDL_BUTTON_LEFT;
            handle(up);
        } else if (verb == "gutterdump") {  // the rows of the line numbers in front: each one's number and change mark
            std::fprintf(stderr, "%s\n", gutter_dump().c_str());
        } else if (verb == "seldump") {  // what is selected (Vim's Visual mode, or the editor's), its lines joined by " / "
            const Editor& ed = pane_->doc->ed;
            u64 lo = 0, hi = 0;
            if (!vim_selection(&lo, &hi)) lo = ed.sel_min(), hi = ed.sel_max();
            std::string t = ed.buf.text(lo, std::min<u64>(hi - lo, 4000));
            for (size_t at = 0; (at = t.find('\n', at)) != std::string::npos;) t.replace(at, 1, " / ");
            std::fprintf(stderr, "%s\n", hi > lo ? ("[" + t + "]").c_str() : "none");
        } else if (verb == "rulerdump") {  // the marks along the split in front's scroll bar
            std::fprintf(stderr, "%s\n", ruler_dump().c_str());
        } else if (verb == "folddump") {  // what is folded in the file in front, and the lines its first rows show
            std::fprintf(stderr, "%s\n", fold_dump().c_str());
        } else if (verb == "stickydump") {  // the lines sticky scroll pins in the split in front
            std::fprintf(stderr, "%s\n", sticky_dump().c_str());
        } else if (verb == "tabsdump") {  // what the tabs of the split in front say
            std::fprintf(stderr, "%s\n", tabs_dump().c_str());
        } else if (verb == "crumbsdump") {  // the breadcrumbs of the split in front
            std::fprintf(stderr, "%s\n", crumbs_dump().c_str());
        } else if (verb == "shapedump") {  // the brackets on show by depth, and the pair at the cursor
            std::fprintf(stderr, "%s\n", shape_dump().c_str());
        } else if (verb == "usesdump") {  // the uses of the symbol at the cursor, as they are marked
            std::fprintf(stderr, "%s\n", lsp_uses_dump().c_str());
        } else if (verb == "emojidump") {
            // The colour emoji the last frame drew as pictures: how many, and
            // the cells each covers ("t" in the text, "b" in a box).
            std::string out = "emoji " + std::to_string(gpu_.emoji_drawn().size()) + ":";
            for (const PictureDraw& pd : gpu_.emoji_drawn())
                out += " " + std::to_string(static_cast<int>(std::lround((pd.rect[2] - pd.rect[0]) / font_->cell_w()))) + (pd.after ? "b" : "t");
            std::fprintf(stderr, "%s\n", out.c_str());
        } else if (verb == "rowsdump") {  // rowsdump:N, the first N rows of the split in front as drawn (ASCII; # anything else)
            gpu_.keep_view(static_cast<int>(pane_->view));
            settle_syntax();
            frame();
            gpu_.keep_view(-1);
            u32 cols = 0;
            const std::vector<u32>& cells = gpu_.kept_cells(&cols);
            std::string out = "rows";
            for (size_t r = 0; cols && r < cells.size() / cols && r < static_cast<size_t>(std::max(1, std::atoi(arg.c_str()))); ++r) {
                std::string row;
                for (u32 c = 0; c < cols; ++c) {
                    const u32 slot = cells[r * cols + c] & 0x1FFF;
                    row += slot == 0 ? ' ' : slot < Font::kFaces * 128 ? static_cast<char>(slot & 127) : '#';
                }
                while (!row.empty() && row.back() == ' ') row.pop_back();
                out += " |" + row;
            }
            std::fprintf(stderr, "%s\n", out.c_str());
        } else if (verb == "views") {
            // Each split's view of the file in front, on one line, in the
            // order the splits were made: its name and document (the same
            // number: the same text), the split's own cursor (line:column
            // from 0) and that line's text; "<" marks the one with the keyboard.
            std::string out = "views:";
            for (const std::unique_ptr<Pane>& p : panes_) {
                if (!p->doc) continue;
                const Doc& d = *p->doc;
                const u64 cur = std::min(caret_in(*p, d).cursor, d.ed.buf.size());
                const u64 line = d.ed.buf.line_of(cur), start = d.ed.buf.line_start(line);
                const size_t slash = d.ed.path.find_last_of('/');
                const std::string name = d.ed.path.empty() ? "untitled" : d.ed.path.substr(slash == std::string::npos ? 0 : slash + 1);
                char head[96];
                std::snprintf(head, sizeof head, " [%s#%llu %llu:%llu ", name.c_str(), static_cast<unsigned long long>(d.id), static_cast<unsigned long long>(line),
                              static_cast<unsigned long long>(cur - start));
                out += head + d.ed.buf.text(start, d.ed.line_end(line) - start) + "]" + (p.get() == pane_ ? "<" : "");
            }
            std::fprintf(stderr, "%s\n", out.c_str());
        } else if (verb == "dump") {
            // How the window is split, on one line: a split is its tabs in
            // brackets, the one in front starred and the split with the
            // keyboard marked ">"; H(..|..) is two side by side, V(../..)
            // one above the other.
            auto show = [&](auto&& self, const SplitNode* n) -> std::string {
                if (!n) return "?";
                if (n->pane) {
                    std::string out = n->pane == pane_ ? ">[" : "[";
                    for (const std::shared_ptr<Doc>& d : n->pane->docs) {
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
        } else if (verb == "title") {  // what the window's title would be
            std::fprintf(stderr, "%s\n", window_title().c_str());
        } else if (verb == "statusclick") {  // statusclick:TEXT, a click on the part of the status bar that starts with it
            if (!status_click(arg)) std::fprintf(stderr, "statusclick: no \"%s\"\n", arg.c_str());
        } else if (verb == "jumpdump") {  // the places Ctrl+- goes back through, file:line, ">" at the one walked back to (at the end: none)
            std::fprintf(stderr, "%s\n", jump_dump().c_str());
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
                    for (const std::shared_ptr<Doc>& d : p->docs) loading |= d->loading;
                bool connecting = false;
                for (const std::shared_ptr<Remote>& r : Remote::all()) connecting |= r->state() == Remote::kConnecting;
                if (!Remote::asks().empty()) {  // ssh has a question: the script answers it
                    remote_sync();
                    break;
                }
                if ((!loading && !connecting && !remote_busy()) || now_ns() > until) break;
                SDL_Delay(2);
            }
        } else if (verb == "focus") {  // the window brought to the front (what that does, as the event does it)
            SDL_Event ev{};
            ev.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
            handle_event(ev);
        } else if (verb == "remoteback") {  // every connection up again, made again by itself (30 s at most)
            const u64 until = now_ns() + 30'000'000'000ull;
            for (;;) {
                remote_sync();
                bool all = true;
                for (const std::shared_ptr<Remote>& r : Remote::all()) all &= r->state() == Remote::kReady;
                if (all || now_ns() > until) {
                    if (!all) std::fprintf(stderr, "remoteback: not back\n");
                    break;
                }
                SDL_Delay(5);
            }
            remote_sync();
        } else if (verb == "remotelost") {  // a connection has been seen to go (10 s at most): what it went with is told
            const u64 until = now_ns() + 10'000'000'000ull;
            for (bool lost = false; !lost && now_ns() < until; SDL_Delay(2))
                for (const std::shared_ptr<Remote>& r : Remote::all()) lost |= r->state() == Remote::kFailed;
            remote_sync();
        } else if (verb == "clock") {
            clock_ns = now_ns();
            nb_spent_ns_ = 0;
        } else if (verb == "lap") {  // (for measuring: not for tests, whose output must not change from run to run)
            const u64 now = now_ns();
            std::fprintf(stderr, "%s %.3f ms\n", arg.c_str(), static_cast<double>(now - clock_ns) / 1e6);
            clock_ns = now;
        } else if (verb == "within") {  // (the time the main thread took, since clock: what waits for the network does not count against it)
            const u64 ms = (now_ns() - clock_ns) / 1'000'000, most = static_cast<u64>(std::atoll(arg.c_str()));
            if (ms <= most) std::fprintf(stderr, "within %llu ms\n", static_cast<unsigned long long>(most));
            else std::fprintf(stderr, "took %llu ms (over %llu)\n", static_cast<unsigned long long>(ms), static_cast<unsigned long long>(most));
        } else if (verb == "alive") {
            std::fprintf(stderr, "%s\n", running_ ? "running" : "quitting");
        } else if (verb == "settingsdump") {
            std::fprintf(stderr, "%s\n", settings_dump().c_str());
        } else if (verb == "settingsopen") {  // settingsopen:KEY, the cursor on that setting's line
            settings_open();
            const Doc& d = *pane_->doc;
            for (u64 l = 0; !arg.empty() && d.settings && l < d.ed.buf.line_count(); ++l) {
                const u64 a = d.ed.buf.line_start(l);
                if (d.ed.buf.text(a, std::min<u64>(d.ed.line_end(l) - a, 200)).starts_with("  " + arg + " ")) {
                    pane_->doc->ed.move_to(a, false);
                    break;
                }
            }
        } else if (verb == "treedump") {
            std::fprintf(stderr, "%s\n", tree_dump().c_str());
        } else if (verb == "watchheld") {  // watchheld:COMMAND, a shell command run while what is heard of the disk is held back
            if (watch_) watch_->held([&] { (void)std::system(arg.c_str()); });
        } else if (verb == "watchwait") {  // something changed on disk: heard (3 s at most), the tree read again, the open files looked at
            const u64 seen = watch_events_, until = now_ns() + 3'000'000'000ull;
            while (watch_events_ == seen && now_ns() < until) {
                watch_take();
                SDL_Delay(2);
            }
            if (watch_events_ == seen) std::fprintf(stderr, "watchwait: nothing heard\n");
            while (watch_due_ns_ && now_ns() < watch_due_ns_) {  // (and what is heard meanwhile, as the event loop would)
                watch_take();
                SDL_Delay(5);
            }
            watch_tick();
            const u64 calm = now_ns() + 2'000'000'000ull;
            while (panel_busy() && now_ns() < calm) {
                sync_panel();
                SDL_Delay(2);
            }
            sync_panel();
        } else if (verb == "ranwait") {  // what ran on another machine has ended: the tree, the open files and git there asked again (not waited for)
            if (!ran_due_ns_) std::fprintf(stderr, "ranwait: nothing ended there\n");
            while (ran_due_ns_ && now_ns() < ran_due_ns_) SDL_Delay(2);
            watch_tick();
        } else if (verb == "treewait") {
            const u64 until = now_ns() + 20'000'000'000ull;
            while (panel_busy() && now_ns() < until) {
                sync_panel();
                SDL_Delay(2);
            }
            sync_panel();
        } else if (verb == "gitbusy") {
            const u64 until = now_ns() + 20'000'000'000ull;
            while (!git_.working_on(pane_->doc->id) && now_ns() < until) SDL_Delay(1);
            if (!git_.working_on(pane_->doc->id)) std::fprintf(stderr, "gitbusy: git is not running for it\n");
        } else if (verb == "remotestream") {
            std::string there;
            const std::shared_ptr<Remote> r = remote_for(full_path(pane_->doc->ed.path), &there, kRemoteWaitMs);
            RemoteProc p;
            std::string err;
            if (!r || !r->spawn({"/bin/sh", "-c", "head -c \"$1\" /dev/zero", "sh", arg}, "/", {}, &p, &err)) {
                std::fprintf(stderr, "remotestream: %s\n", r ? err.c_str() : remote_error(pane_->doc->ed.path).c_str());
            } else {
                ::close(p.in);
                SDL_Delay(1000);  // (nobody reads it meanwhile)
                u64 got = 0;
                char buf[65536];
                for (ssize_t k; (k = ::read(p.out, buf, sizeof buf)) != 0;) {
                    if (k > 0) got += static_cast<u64>(k);
                    else if (errno != EINTR) break;
                }
                ::close(p.out);
                if (p.err >= 0) ::close(p.err);
                const int code = r->wait(p.id, 10'000);
                const u64 held = r->held_most();
                const std::string how = held <= (8u << 20) ? "held a few MB at most" : "held " + std::to_string(held >> 20) + " MB";
                std::fprintf(stderr, "streamed %llu bytes (exit %d), %s\n", static_cast<unsigned long long>(got), code, how.c_str());
            }
        } else if (verb == "remotefeed") {  // remotefeed:N, N bytes written to a program there that takes none for a second
            std::string there;
            const std::shared_ptr<Remote> r = remote_for(full_path(pane_->doc->ed.path), &there, kRemoteWaitMs);
            RemoteProc p;
            std::string err;
            if (!r || !r->spawn({"/bin/sh", "-c", "sleep 1; wc -c"}, "/", {}, &p, &err)) {
                std::fprintf(stderr, "remotefeed: %s\n", r ? err.c_str() : remote_error(pane_->doc->ed.path).c_str());
            } else {
                const u64 want = std::strtoull(arg.c_str(), nullptr, 10);
                std::atomic<u64> fed{0};
                std::thread feeder([&] {
                    std::string chunk(65536, 'x');
                    while (fed < want) {
                        const ssize_t k = ::write(p.in, chunk.data(), static_cast<size_t>(std::min<u64>(chunk.size(), want - fed)));
                        if (k > 0) fed += static_cast<u64>(k);
                        else if (errno != EINTR) break;
                    }
                    ::close(p.in);
                });
                SDL_Delay(700);  // (it takes none meanwhile: what it was fed waits, here or there)
                const u64 early = fed;
                feeder.join();
                std::string out;
                char buf[4096];
                for (ssize_t k; (k = ::read(p.out, buf, sizeof buf)) != 0;) {
                    if (k > 0) out.append(buf, static_cast<size_t>(k));
                    else if (errno != EINTR) break;
                }
                ::close(p.out);
                if (p.err >= 0) ::close(p.err);
                const int code = r->wait(p.id, 10'000);
                const std::string how = early <= (12u << 20) ? "a few MB taken before it read" : std::to_string(early >> 20) + " MB taken before it read";
                std::fprintf(stderr, "fed %llu bytes, it read %llu (exit %d), %s\n", static_cast<unsigned long long>(fed.load()), std::strtoull(out.c_str(), nullptr, 10), code,
                             how.c_str());
            }
        } else if (verb == "prof") {  // prof:what=value (program, args, cwd, dwarf, annotate, kind: time, memory or waiting)
            const size_t eq = arg.find('=');
            prof_set(arg.substr(0, eq), eq == std::string::npos ? std::string() : arg.substr(eq + 1));
        } else if (verb == "profpanel") {  // the side panel's Profile tab
            if (!panel_px()) panel_toggle();
            panel_show_profile();
        } else if (verb == "profrecord") {
            prof_record();
        } else if (verb == "profstop") {
            prof_stop();
        } else if (verb == "profwait") {  // the recording has finished (five minutes at most: a fresh machine's first xctrace takes its time)
            const u64 until = now_ns() + 300'000'000'000ull;
            while (prof_busy() && now_ns() < until) {
                prof_sync();
                SDL_Delay(5);
            }
            prof_sync();
        } else if (verb == "profpick") {  // profpick:SECTION:N, Enter on the Nth row of a section of the Profile tab (profiles, threads)
            const size_t sep = arg.find(':');
            frame();  // (its rows as they are now)
            if (!prof_pick(arg.substr(0, sep), sep == std::string::npos ? 1 : static_cast<u32>(std::atoi(arg.c_str() + sep + 1))))
                std::fprintf(stderr, "profpick: no %s\n", arg.c_str());
        } else if (verb == "profopen") {  // profopen:FILE, a profile kept (.json) or perf script's text, opened in a tab
            std::string text, why;
            auto p = std::make_shared<Profile>();
            const bool ok = fs_read(arg, &text, 64u << 20) && (arg.ends_with(".json") ? Profile::from_json(text, p.get(), &why) : parse_perf_script(text, {}, p.get(), &why));
            if (!ok) std::fprintf(stderr, "profopen: %s\n", why.c_str());
            else {
                if (p->backend.empty()) p->backend = "perf", p->when = "00:00";
                prof_take_profile(p);
            }
        } else if (verb == "proftime") {  // proftime:T0,T1, the profile tab in front narrowed to that stretch of seconds (proftime: all of it)
            double t0 = -1, t1 = -1;
            if (std::sscanf(arg.c_str(), "%lf,%lf", &t0, &t1) != 2) t0 = t1 = -1;
            prof_set_stretch(t0, t1);
        } else if (verb == "profdump") {
            std::fprintf(stderr, "%s\n", prof_dump().c_str());
        } else if (verb == "term") {  // a terminal (term:folder, or in the folder being worked in)
            term_open(arg);
        } else if (verb == "termwait") {  // the terminal in front shows this text (10 s at most)
            if (!term_wait(arg)) std::fprintf(stderr, "termwait: no \"%s\" on the screen\n", arg.c_str());
            term_sync();
        } else if (verb == "debugattach") {  // debugattach:PID, the debugger attached to that process
            debug_attach(std::atoll(arg.c_str()));
        } else if (verb == "debugcore") {  // debugcore:PROGRAM:CORE, a core file opened
            const size_t sep = arg.find(':');
            debug_core(arg.substr(0, sep), sep == std::string::npos ? std::string() : arg.substr(sep + 1));
        } else if (verb == "debug") {  // debug:what=value (program, args, cwd)
            const size_t eq = arg.find('=');
            debug_set(arg.substr(0, eq), eq == std::string::npos ? std::string() : arg.substr(eq + 1));
        } else if (verb == "break") {  // break:N, a breakpoint on line N of the file in front (F9 there)
            Editor& ed = pane_->doc->ed;
            ed.move_to(ed.buf.line_start(std::min<u64>(static_cast<u64>(std::max(1, std::atoi(arg.c_str()))) - 1, ed.buf.line_count() - 1)), false);
            debug_toggle_break();
        } else if (verb == "breakif") {  // breakif:N, what Shift+F9 does on line N of the file in front (its boxes then take what is typed)
            Editor& ed = pane_->doc->ed;
            ed.move_to(ed.buf.line_start(std::min<u64>(static_cast<u64>(std::max(1, std::atoi(arg.c_str()))) - 1, ed.buf.line_count() - 1)), false);
            debug_edit_break();
        } else if (verb == "debugbreaks") {  // the breakpoints, where they are now and what they wait for
            std::fprintf(stderr, "%s\n", debug_breaks_dump().c_str());
        } else if (verb == "debugpanel") {  // the side panel's Debug tab
            if (!panel_px()) panel_toggle();
            panel_show_debug();
        } else if (verb == "debugpick") {  // debugpick:SECTION:N, Enter on the Nth row of a section of the Debug tab (threads, stack, watch, add, breaks)
            const size_t sep = arg.find(':');
            frame();  // (its rows as they are now)
            const std::string which = sep == std::string::npos ? "1" : arg.substr(sep + 1);  // (a number, or a variable's name)
            const bool named = !which.empty() && !std::isdigit(static_cast<unsigned char>(which[0]));
            if (!debug_pick(arg.substr(0, sep), named ? 0 : static_cast<u32>(std::atoi(which.c_str())), named ? which : std::string()))
                std::fprintf(stderr, "debugpick: no %s\n", arg.c_str());
        } else if (verb == "debugwait") {  // debugwait:stopped, debugwait:idle, or debugwait:answers (every request answered)
            debug_wait(arg);
        } else if (verb == "debugdump") {
            std::fprintf(stderr, "%s\n", debug_dump().c_str());
        } else if (verb == "debuginline") {  // the notes after the lines of the first view while stopped (where, the values), as last drawn
            std::fprintf(stderr, "%s\n", debug_inline_dump().c_str());
        } else if (verb == "snippetdump") {  // the snippet being filled in: each stop's places (line:column, from 1), the one the cursor is at in brackets
            std::fprintf(stderr, "%s\n", snippet_dump().c_str());
        } else if (verb == "asmdump") {  // once a file's assembly is made: its lines (from 1) of the file's cursor line, as "line: lines"
            for (const u64 until = now_ns() + 60'000'000'000ull; asm_dump() == "asking" && now_ns() < until;) {
                if (script_) script_jobs();
                asm_take();
                SDL_Delay(5);
            }
            frame();
            std::fprintf(stderr, "%s\n", asm_dump().c_str());
        } else if (verb == "nbdump") {  // the notebook in front's saved outputs on show, as "line: output"
            frame();
            std::fprintf(stderr, "%s\n", nb_dump().c_str());
        } else if (verb == "nbcells") {  // the notebook in front's cells: kind, count, same/changed/new, its outputs' kinds
            std::fputs(nb_cells_dump().c_str(), stderr);
        } else if (verb == "nbview") {  // its view, once drawn: the first row on show (@ its line), then each row as "line: text"
            std::fputs(nb_view_dump().c_str(), stderr);
        } else if (verb == "nbset") {  // nbset:CELL,COUNT,FILE, the cell (from 1) given the outputs in FILE (JSON) and that count, as a kernel would
            int cell = 0, count = -1;
            char file[1024] = "";
            std::string text;
            if (std::sscanf(arg.c_str(), "%d,%d,%1023[^\n]", &cell, &count, file) == 3 && fs_read(file, &text, 64u << 20) && cell > 0) {
                if (!nb_set_outputs(*pane_->doc, static_cast<size_t>(cell - 1), text, count)) std::fprintf(stderr, "nbset: no such code cell, or not outputs\n");
            }
        } else if (verb == "nbwait") {  // nbwait: until no notebook's kernel runs a cell or has one waiting (and each is ready); nbwait:started, until a cell is running and has given an output
            if (!nb_kernel_wait(arg == "started")) std::fprintf(stderr, "nbwait: timed out\n");
        } else if (verb == "nbspent") {  // nbspent:MS, whether the main thread took in what kernels said (since clock) within that many ms
            const u64 ms = nb_spent_ns_ / 1'000'000, most = static_cast<u64>(std::atoll(arg.c_str()));
            if (ms <= most) std::fprintf(stderr, "kernels taken in within %llu ms\n", static_cast<unsigned long long>(most));
            else std::fprintf(stderr, "kernels taken in in %llu ms (over %llu)\n", static_cast<unsigned long long>(ms), static_cast<unsigned long long>(most));
        } else if (verb == "nbkernel") {  // the kernel of the notebook in front: ipython or python, its pid, running or idle, how many cells wait
            std::fprintf(stderr, "%s\n", nb_kernel_dump().c_str());
        } else if (verb == "nbclick") {  // nbclick:ROW, a click on that row of the notebook's view (as nbview counts them, from 0)
            md_wait();
            float x = 0, y = 0;
            if (nb_view_point(static_cast<u32>(std::atoi(arg.c_str())), &x, &y)) {
                e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                e.button.button = SDL_BUTTON_LEFT;
                e.button.x = x;
                e.button.y = y;
                e.button.clicks = 1;
                handle(e);
                e.type = SDL_EVENT_MOUSE_BUTTON_UP;
                handle(e);
            } else {
                std::fprintf(stderr, "nbclick: that row is not on show\n");
            }
        } else if (verb == "hierdump") {  // the call or type hierarchy on show: its rows (once the server has answered)
            for (const u64 until = now_ns() + 10'000'000'000ull; hier_dump() == "waiting" && now_ns() < until;) {
                sync_lsp();
                SDL_Delay(2);
            }
            std::fprintf(stderr, "%s\n", hier_dump().c_str());
        } else if (verb == "peekdump") {  // a peek's box: its file:line, that line, whether it has the keyboard and is inside its split
            for (const u64 until = now_ns() + 10'000'000'000ull; peek_pending_ && now_ns() < until;) {
                sync_lsp();
                SDL_Delay(2);
            }
            frame();
            std::fprintf(stderr, "%s\n", peek_dump().c_str());
        } else if (verb == "lensdump") {  // the reference counts drawn in the split in front (once asked and answered: a second at most)
            for (const u64 until = now_ns() + 3'000'000'000ull; now_ns() < until;) {
                sync_lsp();
                lsp_tick();
                frame();
                if (!lens_waiting()) break;
                SDL_Delay(5);
            }
            std::fprintf(stderr, "%s\n", lens_dump().c_str());
        } else if (verb == "historydump") {  // the versions kept of the file in front, newest first: s saved, t while typing
            std::fprintf(stderr, "%s\n", history_dump().c_str());
        } else if (verb == "minimapdump") {  // each split's minimap: where the file's lines on it start, and its tiles; or none
            frame();
            std::fprintf(stderr, "%s\n", minimap_dump().c_str());
        } else if (verb == "spelldump") {  // the words underlined as wrong in the split in front, once checked
            std::fprintf(stderr, "%s\n", spell_dump().c_str());
        } else if (verb == "wrapdump") {  // the split in front's soft wrap: its width and rows, and each row on show (line@column), or off
            frame();
            std::fprintf(stderr, "%s\n", wrap_dump().c_str());
        } else if (verb == "prdump") {  // a pull request's comments, once gh has answered (as "path:line author")
            for (const u64 until = now_ns() + 10'000'000'000ull; script_ && script_busy() && now_ns() < until;) {
                script_jobs();
                SDL_Delay(5);
            }
            frame();
            std::fprintf(stderr, "%s\n", pr_dump().c_str());
        } else if (verb == "focusdump") {  // distraction-free or presentation mode: which, the text's size, and what the chrome takes
            frame();
            std::fprintf(stderr, "%s\n", focus_dump().c_str());
        } else if (verb == "caretdump") {  // every caret of the file in front: line:column (from 1), a selection's other end after a dash, the primary starred
            std::fprintf(stderr, "%s\n", caret_dump().c_str());
        } else if (verb == "docdump") {  // docdump:N, the tab in front's title, then its lines from N before the cursor's to N after (the cursor's marked >, and in it the cursor |)
            const Editor& ed = pane_->doc->ed;
            const u64 n = static_cast<u64>(std::max(0, std::atoi(arg.c_str()))), at = ed.buf.line_of(ed.cursor());
            std::string out = pane_->doc->title;
            for (u64 l = at > n ? at - n : 0; l <= at + n && l < ed.buf.line_count(); ++l) {
                const u64 b = ed.buf.line_start(l);
                std::string text = ed.buf.text(b, ed.line_end(l) - b);
                if (l == at) text.insert(std::min<size_t>(text.size(), ed.cursor() - b), "|");
                out += "\n" + std::string(l == at ? "> " : "  ") + text;
            }
            std::fprintf(stderr, "%s\n", out.c_str());
        } else if (verb == "newproject") {  // the start screen's New project tab
            project_page(true);
        } else if (verb == "project") {  // project:key=value (kind, name, where, git, or a field of the kind's)
            const size_t eq = arg.find('=');
            project_set(arg.substr(0, eq), eq == std::string::npos ? std::string() : arg.substr(eq + 1));
        } else if (verb == "projectcreate") {  // what Create does (the project is made on a thread: projectwait)
            project_create();
        } else if (verb == "projectwait") {  // what the New project tab waits for has come: the project made, or the facts a search wants
            project_wait();
        } else if (verb == "projectfacts") {
            std::fprintf(stderr, "%s\n", project_facts_dump().c_str());
        } else if (verb == "projectdump") {
            std::fprintf(stderr, "%s\n", project_dump().c_str());
        } else if (verb == "mdpreview") {  // the Markdown preview of the file in front
            md_open();
        } else if (verb == "mdwait") {  // every preview drawn as its text is now (they are drawn on a thread)
            md_wait();
        } else if (verb == "mddump") {  // the preview's rows, as text
            std::fputs(md_dump().c_str(), stderr);
        } else if (verb == "mdinks") {  // and each of their runs in its colour
            std::fputs(md_dump(1).c_str(), stderr);
        } else if (verb == "mdlooks") {  // and in its style (b, i, s, u), on its tone (t1...), with where it goes (>link) and a row's id (#id)
            std::fputs(md_dump(2).c_str(), stderr);
        } else if (verb == "mdtop") {  // the first row the preview shows: its number and its text
            std::fputs(md_top_dump().c_str(), stderr);
        } else if (verb == "mdclick") {  // mdclick:LINK, a click on the first link on show that goes there ("#fn-1")
            float x = 0, y = 0;
            if (md_link_point(arg, &x, &y)) {
                e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                e.button.button = SDL_BUTTON_LEFT;
                e.button.x = x;
                e.button.y = y;
                e.button.clicks = 1;
                handle(e);
                e.type = SDL_EVENT_MOUSE_BUTTON_UP;
                handle(e);
            } else {
                std::fprintf(stderr, "mdclick: no such link on show\n");
            }
        } else if (verb == "diagramdump") {  // the preview's diagrams as laid out: boxes of nodes, subgraphs and labels, points along edges
            std::fputs(md_diagram_dump().c_str(), stderr);
        } else if (verb == "mdtoday") {  // mdtoday:DAYS, the date diagrams are drawn for from now on (days from 1970)
            md_set_today(std::atoll(arg.c_str()));
        } else if (verb == "imagewait") {  // every picture asked for has been read (10 s at most)
            picture_wait();
        } else if (verb == "imagedump") {  // the image tab in front: what it shows, and how
            std::fprintf(stderr, "%s\n", image_dump().c_str());
        } else if (verb == "imageframes") {  // imageframes:MS, MS milliseconds of the main loop: how many frames it drew (an animation's)
            std::fprintf(stderr, "%s\n", picture_frames(std::atoi(arg.c_str())).c_str());
        } else if (verb == "pdfwait") {  // the PDFs read, and their pages on show drawn (20 s at most)
            pdf_wait();
        } else if (verb == "pdfdump") {  // the PDF tab in front: what it shows, and how
            std::fprintf(stderr, "%s\n", pdf_dump().c_str());
        } else if (verb == "pdfpage") {  // pdfpage:N, where its page N is drawn
            std::fprintf(stderr, "page %s at %s\n", arg.c_str(), pdf_page_rect(std::atoi(arg.c_str())).c_str());
        } else if (verb == "pdfclip" || verb == "pdfopened" || verb == "pdfoutline") {  // what Cmd+C copied, the web address a link opened, the outline
            std::fprintf(stderr, "%s\n", pdf_dump_more(verb.substr(3)).c_str());
        } else if (verb == "pdftextat") {  // pdftextat:N:TEXT, where TEXT is on page N (its text read): x0,y0,x1,y1 in pixels
            std::fprintf(stderr, "%s at %s\n", arg.c_str(), pdf_dump_more(arg).c_str());
        } else if (verb == "pixel") {  // pixel:X,Y, the colour of the last frame there (pixels, not points)
            int x = 0, y = 0, w = 0, h = 0;
            std::sscanf(arg.c_str(), "%d,%d", &x, &y);
            std::vector<u8> rgba;
            if (gpu_.read_pixels(rgba, &w, &h) && x >= 0 && y >= 0 && x < w && y < h) {
                const u8* p = rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
                std::fprintf(stderr, "pixel %d,%d: %d %d %d\n", x, y, p[0], p[1], p[2]);
            } else {
                std::fprintf(stderr, "pixel %d,%d: none\n", x, y);
            }
        } else if (verb == "reload") {  // what coming back to the window does: files changed on disk read again
            reload_changed();
        } else if (verb == "pinch") {  // pinch:SCALE, a touchpad's pinch (over where the mouse was put)
            e.type = SDL_EVENT_PINCH_UPDATE;
            e.pinch.scale = static_cast<float>(std::atof(arg.c_str()));
            handle(e);
        } else if (verb == "buildpanel") {  // the side panel's Build tab
            if (!panel_px()) panel_toggle();
            panel_show_build();
        } else if (verb == "coveragedump") {  // the coverage of the file in front, and each file's share
            std::fprintf(stderr, "%s\n", coverage_dump().c_str());
        } else if (verb == "testdump") {  // the tests found (finding and running them waited for), and how each went
            std::fprintf(stderr, "%s\n", test_dump().c_str());
        } else if (verb == "runconfdump") {  // the run configuration chosen, and what Debug and Profile would run
            build_settle();
            const BuildTask* t = run_config();
            std::fprintf(stderr, "%s | debug: %s | profile: %s\n", t ? t->name.c_str() : "none", debug_target().c_str(), prof_target().c_str());
        } else if (verb == "builddump") {  // the tasks found (by system; > the Run button's), and how the last run went
            std::fprintf(stderr, "%s\n", build_dump().c_str());
        } else if (verb == "buildrun") {  // buildrun:name runs that task (buildrun alone: the Run button's)
            build_settle();
            if (!build_run_named(arg)) std::fprintf(stderr, "buildrun: no \"%s\"\n", arg.c_str());
        } else if (verb == "buildwait") {  // the task run has ended, and the targets being listed are (60 s at most)
            build_wait();
        } else if (verb == "problemsdump") {  // the Run button's words and the Problems tab's rows (> the one picked)
            std::fprintf(stderr, "%s\n", problems_dump().c_str());
        } else if (verb == "termclick") {  // termclick:TEXT, Cmd+click on that text in a terminal on show
            term_sync();
            if (!term_click_text(arg)) std::fprintf(stderr, "termclick: no \"%s\" on show\n", arg.c_str());
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
        } else if (verb == "answer") {  // (save or dontsave; keep or end, for jobs on other machines)
            if (arg == "keep" || arg == "end") answer_keep_ = arg == "keep";
            else answer_save_ = arg == "save";
        } else if (verb == "asked") {  // the last question there was nobody to ask (a box would have asked it)
            std::fprintf(stderr, "%s\n", asked_.empty() ? "nothing asked" : asked_.c_str());
            asked_.clear();
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
        } else if (verb == "findwait") {  // the worker that finds files and text has finished, and a folder there has been listed
            find_settle(true);
        } else if (verb == "finddump") {
            find_settle();
            std::fprintf(stderr, "%s%s\n", arg.empty() ? "" : (arg + ": ").c_str(), find_dump().c_str());
        } else if (verb == "findheading") {  // what the box says it lists ("References (4)"), or closed
            find_settle();
            std::fprintf(stderr, "%s\n", find_heading().c_str());
        } else if (verb == "findinks") {  // the box drawn: the colour of a row's secondary text and of the chosen row's, and their contrast
            find_settle();
            frame();
            std::fprintf(stderr, "%s\n", find_inks_dump().c_str());
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
    if (!running_) session_save();  // (it quit: as loop() ending does)
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
