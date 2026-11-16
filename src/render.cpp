#include "render.hpp"

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace blit {

namespace {

// Compiled shaders, baked in with C++26 #embed. See shaders/build.sh. Metal
// takes compiled libraries when there are any (they need Xcode's compiler
// to make) and the source otherwise; Vulkan takes SPIR-V.
#if defined(__APPLE__)
#if __has_embed("../shaders/gen/grid.vert.metallib") && __has_embed("../shaders/gen/grid.frag.metallib") && \
    __has_embed("../shaders/gen/grid_fx.frag.metallib") && __has_embed("../shaders/gen/grid_dim.frag.metallib") && \
    __has_embed("../shaders/gen/popup.frag.metallib") && __has_embed("../shaders/gen/popup_fx.frag.metallib") && \
    __has_embed("../shaders/gen/image.frag.metallib")
#define BLIT_SHADERS_COMPILED 1
const u8 kVert[] = {
#embed "../shaders/gen/grid.vert.metallib"
};
const u8 kText[] = {
#embed "../shaders/gen/grid.frag.metallib"
};
const u8 kTextFx[] = {
#embed "../shaders/gen/grid_fx.frag.metallib"
};
const u8 kTextDim[] = {
#embed "../shaders/gen/grid_dim.frag.metallib"
};
const u8 kPopup[] = {
#embed "../shaders/gen/popup.frag.metallib"
};
const u8 kPopupFx[] = {
#embed "../shaders/gen/popup_fx.frag.metallib"
};
const u8 kImage[] = {
#embed "../shaders/gen/image.frag.metallib"
};
#else
#define BLIT_SHADERS_COMPILED 0
const u8 kVert[] = {
#embed "../shaders/gen/grid.vert.msl"
};
const u8 kText[] = {
#embed "../shaders/gen/grid.frag.msl"
};
const u8 kTextFx[] = {
#embed "../shaders/gen/grid_fx.frag.msl"
};
const u8 kTextDim[] = {
#embed "../shaders/gen/grid_dim.frag.msl"
};
const u8 kPopup[] = {
#embed "../shaders/gen/popup.frag.msl"
};
const u8 kPopupFx[] = {
#embed "../shaders/gen/popup_fx.frag.msl"
};
const u8 kImage[] = {
#embed "../shaders/gen/image.frag.msl"
};
#endif
#else
#define BLIT_SHADERS_COMPILED 1
const u8 kVert[] = {
#embed "../shaders/gen/grid.vert.spv"
};
const u8 kText[] = {
#embed "../shaders/gen/grid.frag.spv"
};
const u8 kTextFx[] = {
#embed "../shaders/gen/grid_fx.frag.spv"
};
const u8 kTextDim[] = {
#embed "../shaders/gen/grid_dim.frag.spv"
};
const u8 kPopup[] = {
#embed "../shaders/gen/popup.frag.spv"
};
const u8 kPopupFx[] = {
#embed "../shaders/gen/popup_fx.frag.spv"
};
const u8 kImage[] = {
#embed "../shaders/gen/image.frag.spv"
};
#endif

// Mirrors the std140 block in shaders/grid.frag.
struct Uniforms {
    float bg[4], fg[4], sel[4], cur_color[4], thumb_color[4];
    float cur_rect[4], thumb_rect[4];
    float cell[2], inv_cell[2], origin[2], inv_atlas[2];
    u32 cols, rows, atlas_shift, cell_base;
    float frame_rect[4], border[4], squiggle[4], guide[4];
    float tint_rect[4], tint_color[4];
    float radii[4], shadow[4], inset[4], fx[4];
    float palette[32][4];
    float tones[8][4];  // a popup's: the backgrounds of lit cells that name one of them (bits 21-23; 0 names none)
};
static_assert(sizeof(Uniforms) == 960);

// Mirrors the block in shaders/image.frag (at the start of a Uniforms' room).
struct PictureUniforms {
    float rect[4], fx[4], check_a[4], check_b[4];
};
static_assert(sizeof(PictureUniforms) <= sizeof(Uniforms));

// Where compiled pipelines are kept from one run to the next.
std::string cache_dir() {
    if (const char* d = std::getenv("BLIT_CACHE_DIR"); d && *d) return d;
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/Library/Caches/blit";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/blit";
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.cache/blit";
#endif
    return {};
}

}  // namespace

Renderer::~Renderer() {
    release();
    if (gpu_) blit_gpu_destroy(gpu_);
#if defined(__APPLE__)
    if (view_) SDL_Metal_DestroyView(static_cast<SDL_MetalView>(view_));
#endif
}

bool Renderer::init(SDL_Window* win, int frames_in_flight, bool debug, std::string* err) {
    void* surface = nullptr;
#if defined(__APPLE__)
    if (win) {
        view_ = SDL_Metal_CreateView(win);
        if (!view_) {
            if (err) *err = std::string("cannot make a Metal view: ") + SDL_GetError();
            return false;
        }
        surface = SDL_Metal_GetLayer(static_cast<SDL_MetalView>(view_));
    }
#endif
    BlitGpuShaders sh{};
    const int src = BLIT_SHADERS_COMPILED ? 0 : 1;
    sh.vertex = {kVert, sizeof kVert, src};
    sh.fragment[BLIT_PIPE_TEXT] = {kText, sizeof kText, src};
    sh.fragment[BLIT_PIPE_TEXT_FX] = {kTextFx, sizeof kTextFx, src};
    sh.fragment[BLIT_PIPE_TEXT_DIM] = {kTextDim, sizeof kTextDim, src};
    sh.fragment[BLIT_PIPE_POPUP] = {kPopup, sizeof kPopup, src};
    sh.fragment[BLIT_PIPE_POPUP_FX] = {kPopupFx, sizeof kPopupFx, src};
    sh.image = {kImage, sizeof kImage, src};
    const std::string cache = cache_dir();
    char why[512] = "";
    // Frames under way at once: two for a window (the display sets the pace,
    // and fewer waiting means less delay), three offscreen, where the GPU
    // sets it and would otherwise wait between frames for the CPU to hear
    // that one has finished.
    if (frames_in_flight <= 0) frames_in_flight = win ? 2 : 3;
    gpu_ = blit_gpu_create(surface, &sh, frames_in_flight, debug ? 1 : 0, cache.empty() ? nullptr : cache.c_str(), why, sizeof why);
    if (!gpu_) {
        if (err) *err = why;
        return false;
    }
    win_ = win;
    return true;
}

bool Renderer::set_present_mode(int mode) {
    if (!gpu_ || !win_) return false;
    if (mode == kMailbox) return false;  // (Metal has none: the caller falls back to vsync)
    return blit_gpu_set_present(gpu_, mode == kImmediate ? BLIT_PRESENT_IMMEDIATE : BLIT_PRESENT_VSYNC) != 0;
}

void Renderer::set_images(int n) {
    if (!gpu_ || !win_ || n == images_) return;
    if (blit_gpu_set_images(gpu_, n)) images_ = n;
}

u32 Renderer::image_add(const u8* rgba, int w, int h) { return gpu_ ? blit_gpu_image_add(gpu_, rgba, w, h) : 0; }

void Renderer::image_drop(u32 id) {
    if (gpu_ && id) blit_gpu_image_drop(gpu_, id);
}

bool Renderer::set_font(const Font& font, std::string* err) {
    if (!blit_gpu_set_atlas(gpu_, font.atlas_w(), font.atlas_h())) {
        if (err) *err = "atlas texture creation failed";
        return false;
    }
    return true;
}

bool Renderer::set_offscreen(int w, int h) {
    off_w_ = off_h_ = 0;
    if (!blit_gpu_set_offscreen(gpu_, w, h)) return false;
    if (w > 0 && h > 0) off_w_ = w, off_h_ = h;
    return true;
}

Renderer::Result Renderer::acquire() {
    if (!gpu_) return kFailed;
    if (acquired_) return kDrawn;
    int want_w = 0, want_h = 0;
    if (win_ && !off_w_) SDL_GetWindowSizeInPixels(win_, &want_w, &want_h);
    acq_times_ = {};
    const int began = blit_gpu_begin(gpu_, want_w, want_h, &acq_w_, &acq_h_, &acq_times_);
    if (began == BLIT_FRAME_SKIPPED) return kSkipped;  // minimized or otherwise not presentable right now
    if (began != BLIT_FRAME_DRAWN) return kFailed;
    acquired_ = true;
    return kDrawn;
}

void Renderer::release() {
    if (!acquired_) return;
    acquired_ = false;
    blit_gpu_cancel(gpu_);
}

Renderer::Result Renderer::draw(Font& font, LayoutFn layout, FrameStats* stats) {
    const u64 t0 = now_ns();
    if (!gpu_) return kFailed;
    // The memory of a frame, and the image of the window to draw into
    // (waited for here, before the layout, so that what is drawn is as
    // fresh as it can be), unless acquire() has waited for them already.
    const bool early = acquired_;
    BlitGpuTimes gt = acq_times_;
    int iw = acq_w_, ih = acq_h_;
    acquired_ = false;
    if (!early) {
        int want_w = 0, want_h = 0;
        if (win_ && !off_w_) SDL_GetWindowSizeInPixels(win_, &want_w, &want_h);
        gt = {};
        const int began = blit_gpu_begin(gpu_, want_w, want_h, &iw, &ih, &gt);
        if (began == BLIT_FRAME_SKIPPED) return kSkipped;  // minimized or otherwise not presentable right now
        if (began != BLIT_FRAME_DRAWN) return kFailed;
    }
    const u32 w = static_cast<u32>(iw), h = static_cast<u32>(ih);
    const u64 wait = early ? 0 : gt.target_ns;  // (waited in this call)
    const u64 t_begin = now_ns();

    // The layout is asked again if the glyph atlas restarted while the
    // cells were being made (it filled up), since slots handed out before
    // that are gone.
    FrameDesc fd;
    u32 text_total = 0, total = 0;
    u32* cells = nullptr;  // the frame's cells, in the memory the GPU reads them from
    u32 view_base[kMaxViews] = {};  // where each view's cells start in the buffer
    u64 fill_ns = 0, layout_ns = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const u32 gen = font.generation();
        if (attempt > 0) fd = FrameDesc{};  // (made fresh just above the first time)
        const u64 l0 = now_ns();
        layout(static_cast<int>(w), static_cast<int>(h), fd);
        layout_ns += now_ns() - l0;
        fd.view_count = std::min(fd.view_count, kMaxViews);
        text_total = 0;
        for (u32 i = 0; i < fd.view_count; ++i) {
            view_base[i] = text_total;
            text_total += fd.views[i].grid.cols * fd.views[i].grid.rows;
        }
        total = text_total;
        for (u32 i = 0; i < fd.popup_count; ++i) total += fd.popups[i].cols * fd.popups[i].rows;
        if (text_total == 0 || !(cells = blit_gpu_cells(gpu_, total))) {
            blit_gpu_cancel(gpu_);
            return kFailed;
        }
        const u64 t1 = now_ns();
        for (u32 i = 0; i < fd.view_count; ++i) {
            const TextView& v = fd.views[i];
            if (!v.buf || !v.grid.cols || !v.grid.rows) continue;
            u32* const view_cells = cells + view_base[i];
            // (Rows left empty above the first line, while the view is pulled
            // down past the top, are no part of the text: it starts under them.)
            GridSpec text = v.grid;
            const u32 blank = std::min(v.blank_rows, text.rows);
            text.rows -= blank;
            u32* const text_cells = view_cells + static_cast<size_t>(blank) * text.cols;
            std::memset(view_cells, 0, static_cast<size_t>(blank) * text.cols * sizeof(u32));
            fill_grid(*v.buf, font, text, text_cells, v.anchors ? v.anchors : &anchors_);
            if (text.guide_step) guide_grid(*v.buf, text, text_cells, v.guides ? *v.guides : guides_);
            if (!text.decos.empty() || !text.notes.empty()) decorate_grid(text, text_cells);
            if (v.sel_radius > 0) round_selection(text, text_cells);
            // Flag the cells under the cursor, the scroll thumb and a tint (see the shader).
            auto mark = [&](const float* r) {
                if (r[2] <= r[0] || r[3] <= r[1] || r[2] <= 0 || r[3] <= 0) return;
                const float cw = static_cast<float>(font.cell_w()), ch = static_cast<float>(font.cell_h());
                // Rectangles are in window pixels; the grid starts at the view's corner.
                const float left = static_cast<float>(v.x), top = static_cast<float>(v.y);
                if (r[2] <= left || r[3] <= top) return;
                // Cell index of a pixel offset, clamped before it becomes unsigned. The end is
                // exclusive and rounded up, so a rectangle that ends part way into a pixel
                // still flags the cell under that last, half covered pixel.
                if ((r[0] - left + v.origin[0]) / cw >= static_cast<float>(v.grid.cols) || (r[1] - top + v.origin[1]) / ch >= static_cast<float>(v.grid.rows)) return;
                auto cell_at = [](float px, float size, u32 last) { return static_cast<u32>(std::clamp(px / size, 0.0f, static_cast<float>(last))); };
                const u32 c0 = cell_at(r[0] - left + v.origin[0], cw, v.grid.cols - 1);
                const u32 c1 = cell_at(std::ceil(r[2] - left + v.origin[0]) - 1, cw, v.grid.cols - 1);
                const u32 r0 = cell_at(r[1] - top + v.origin[1], ch, v.grid.rows - 1);
                const u32 r1 = cell_at(std::ceil(r[3] - top + v.origin[1]) - 1, ch, v.grid.rows - 1);
                for (u32 y = r0; y <= r1; ++y)
                    for (u32 x = c0; x <= c1; ++x) view_cells[static_cast<size_t>(y) * v.grid.cols + x] |= kCellOverlay;
            };
            mark(v.cursor);
            mark(v.thumb);
            mark(v.tint);
        }
        fill_ns += now_ns() - t1;
        // Popup cells follow the text's in the same buffer.
        u32 at = text_total;
        for (u32 i = 0; i < fd.popup_count; ++i) {
            const u32 n = fd.popups[i].cols * fd.popups[i].rows;
            std::memcpy(cells + at, fd.popups[i].cells, static_cast<size_t>(n) * sizeof(u32));
            if (fd.popups[i].pill > 0) mark_runs(cells + at, fd.popups[i].cols, fd.popups[i].rows);
            at += n;
        }
        if (font.generation() == gen) break;
    }
    const u64 t2 = now_ns();

    // The cells are where the GPU reads them already. Glyphs drawn into the
    // atlas since the last frame go with this one.
    int y0, y1;
    const bool atlas_changed = font.take_dirty(&y0, &y1);
    if (atlas_changed) blit_gpu_atlas_rows(gpu_, font.pixels(), y0, y1);
    const u64 t3 = now_ns();

    Uniforms u{};
    std::memcpy(u.bg, fd.theme.bg, sizeof u.bg);
    std::memcpy(u.fg, fd.theme.fg, sizeof u.fg);
    std::memcpy(u.sel, fd.theme.sel, sizeof u.sel);
    std::memcpy(u.cur_color, fd.theme.cursor, sizeof u.cur_color);
    std::memcpy(u.thumb_color, fd.theme.thumb, sizeof u.thumb_color);
    std::memcpy(u.palette, fd.theme.palette, sizeof u.palette);
    std::memcpy(u.tint_color, fd.theme.tint, sizeof u.tint_color);
    u.cell[0] = static_cast<float>(font.cell_w());
    u.cell[1] = static_cast<float>(font.cell_h());
    u.inv_cell[0] = 1.0f / u.cell[0];
    u.inv_cell[1] = 1.0f / u.cell[1];
    u.inv_atlas[0] = 1.0f / static_cast<float>(font.atlas_w());
    u.inv_atlas[1] = 1.0f / static_cast<float>(font.atlas_h());
    u.atlas_shift = font.atlas_shift();
    u.guide[0] = fd.guide_px;
    std::memcpy(u.squiggle, fd.squiggle, sizeof u.squiggle);
    u.fx[2] = fd.scale;
    u.inset[2] = 1;

    // The draws, each with the pipeline and the uniforms it was given last
    // (the backend makes them into commands).
    uniforms_.clear();
    draws_.clear();
    u32 bound = BLIT_PIPE_TEXT, pushed = 0;
    auto bind = [&](u32 pipe) { bound = pipe; };
    auto push = [&](const Uniforms& block) {
        const size_t at = uniforms_.size();
        uniforms_.resize(at + sizeof(Uniforms));
        std::memcpy(uniforms_.data() + at, &block, sizeof(Uniforms));
        pushed = static_cast<u32>(at / sizeof(Uniforms));
    };
    auto draw_in = [&](int left, int top, int right, int bottom) {
        if (right <= left || bottom <= top) return;
        draws_.push_back({bound, pushed, left, top, right - left, bottom - top, 0});
    };
    // The pictures to go over what is drawn so far: those after `popups` popups.
    const u32 pictures = std::min(fd.picture_count, kMaxPictures);
    u32 next_picture = 0;
    auto pictures_after = [&](u32 popups) {
        for (; next_picture < pictures && fd.pictures[next_picture].after <= popups; ++next_picture) {
            const PictureDraw& pd = fd.pictures[next_picture];
            if (!pd.image) continue;
            int left = static_cast<int>(std::floor(pd.rect[0])), top = static_cast<int>(std::floor(pd.rect[1]));
            int right = static_cast<int>(std::ceil(pd.rect[2])), bottom = static_cast<int>(std::ceil(pd.rect[3]));
            left = std::max({left, pd.clip[0], 0}), top = std::max({top, pd.clip[1], 0});
            right = std::min({right, pd.clip[2], static_cast<int>(w)}), bottom = std::min({bottom, pd.clip[3], static_cast<int>(h)});
            if (right <= left || bottom <= top) continue;
            PictureUniforms pu{};
            std::memcpy(pu.rect, pd.rect, sizeof pu.rect);
            pu.fx[0] = pd.radius;
            pu.fx[1] = pd.opacity;
            pu.fx[2] = pd.sharp ? 1.0f : 0.0f;
            pu.fx[3] = pd.checker;
            std::memcpy(pu.check_a, pd.check_a, sizeof pu.check_a);
            std::memcpy(pu.check_b, pd.check_b, sizeof pu.check_b);
            const size_t at = uniforms_.size();
            uniforms_.resize(at + sizeof(Uniforms));
            std::memset(uniforms_.data() + at, 0, sizeof(Uniforms));
            std::memcpy(uniforms_.data() + at, &pu, sizeof pu);
            // Written straight where nothing of it is see-through and it fills
            // its rectangle to the pixel: the GPU then leaves out what is under
            // it (the tab's box), which blending would make it draw.
            const bool whole = std::floor(pd.rect[0]) == pd.rect[0] && std::floor(pd.rect[1]) == pd.rect[1] && std::floor(pd.rect[2]) == pd.rect[2] && std::floor(pd.rect[3]) == pd.rect[3];
            const bool solid = whole && pd.opacity >= 1 && pd.radius <= 0 && (!pd.clear || pd.checker > 0);
            draws_.push_back({solid ? BLIT_PIPE_IMAGE_SOLID : BLIT_PIPE_IMAGE, static_cast<u32>(at / sizeof(Uniforms)), left, top, right - left, bottom - top, pd.image});
        }
    };
    // Each text view is the one triangle, cut to the view's rectangle: a
    // side panel, tab bars and line numbers have the rest of the window, and
    // so do the other views.
    for (u32 i = 0; i < fd.view_count; ++i) {
        const TextView& v = fd.views[i];
        const int left = std::max(v.x, 0), top = std::max(v.y, 0);
        const int right = std::min(v.x + v.w, static_cast<int>(w)), bottom = std::min(v.y + v.h, static_cast<int>(h));
        if (right <= left || bottom <= top || !v.grid.cols || !v.grid.rows) continue;
        // (The shader that rounds what is selected, and the one that dims too, only where there is something for them to do.)
        bind(v.dim > 0 ? BLIT_PIPE_TEXT_DIM : v.sel_radius > 0 ? BLIT_PIPE_TEXT_FX : BLIT_PIPE_TEXT);
        std::memcpy(u.cur_rect, v.cursor, sizeof u.cur_rect);
        u.cur_color[3] = v.cursor_block ? 0.5f : 1.0f;
        std::memcpy(u.thumb_rect, v.thumb, sizeof u.thumb_rect);
        u.thumb_color[3] = fd.theme.thumb[3] * v.thumb_alpha;
        std::memcpy(u.tint_rect, v.tint, sizeof u.tint_rect);
        u.radii[0] = v.sel_radius;
        u.radii[1] = v.cursor_radius;
        u.radii[2] = v.match_radius;
        u.radii[3] = v.tint_radius;
        u.fx[0] = v.dim;
        u.fx[1] = v.sel_radius > 0 ? 1.0f : 0.0f;
        u.origin[0] = v.origin[0] - static_cast<float>(v.x);
        u.origin[1] = v.origin[1] - static_cast<float>(v.y);
        u.cols = v.grid.cols;
        u.rows = v.grid.rows;
        u.cell_base = view_base[i];
        push(u);
        draw_in(left, top, right, bottom);
    }
    // Each popup is the same triangle again, cut down to the popup's box,
    // with its own cells and colours, through the popup shader.
    // A plain one is written straight, as before. One with round corners
    // or less than full opacity is laid over what is there, shadow and all,
    // and may draw outside its box. One that only casts a shadow (a tab bar
    // over the text under it) is written straight too, and just its shadow
    // is laid over what is around it, in up to four strips: blending all of
    // a box that hides what is under it anyway would be paid for per pixel.
    // (Pictures are in the order they are drawn; one is put after the popups
    // before it, under the ones after.)
    std::stable_sort(fd.pictures, fd.pictures + pictures, [](const PictureDraw& a, const PictureDraw& b) { return a.after < b.after; });
    u32 base = text_total;
    for (u32 i = 0; i < fd.popup_count; ++i) {
        pictures_after(i);
        const Popup& p = fd.popups[i];
        const u32 n = p.cols * p.rows;
        const bool has_cells = n > 0;  // (an empty popup has no cells to read)
        const bool shadowed = p.shadow > 0 && p.shadow_alpha > 0 && fd.theme.shadow > 0;
        const bool laid_over = p.radius > 0 || p.opacity < 1;
        // How far the shadow shows on each side: the wide one fades out
        // 0.35 of its reach from the box, further the way it falls and less
        // the other way; the close one is all round. (See shade() in the shader.)
        auto reach = [&](float fall) { return shadowed ? static_cast<int>(std::ceil(std::max(std::max(2.0f, p.shadow * 0.12f), p.shadow * 0.35f + fall))) + 1 : 0; };
        // The box, and the box with its shadow, as far as they are in the window (and in the clip, the shadow).
        const int ix0 = std::max(p.x, 0), iy0 = std::max(p.y, 0);
        const int ix1 = std::min(p.x + p.w, static_cast<int>(w)), iy1 = std::min(p.y + p.h, static_cast<int>(h));
        int ox0 = std::max(p.x - reach(-p.shadow_dx), 0), oy0 = std::max(p.y - reach(-p.shadow_dy), 0);
        int ox1 = std::min(p.x + p.w + reach(p.shadow_dx), static_cast<int>(w)), oy1 = std::min(p.y + p.h + reach(p.shadow_dy), static_cast<int>(h));
        if (p.clip[2] > p.clip[0] && p.clip[3] > p.clip[1]) {
            ox0 = std::max(ox0, p.clip[0]), oy0 = std::max(oy0, p.clip[1]);
            ox1 = std::min(ox1, p.clip[2]), oy1 = std::min(oy1, p.clip[3]);
        }
        if (p.opacity > 0 && has_cells && (laid_over ? ox1 > ox0 && oy1 > oy0 : ix1 > ix0 && iy1 > iy0)) {
            Uniforms pu = u;
            std::memcpy(pu.bg, p.bg ? p.bg : fd.theme.popup_bg, sizeof pu.bg);
            if (p.sel) std::memcpy(pu.sel, p.sel, sizeof pu.sel);
            if (p.tones) std::memcpy(pu.tones, p.tones, sizeof pu.tones);
            if (p.palette) std::memcpy(pu.palette, p.palette, sizeof pu.palette);
            std::memcpy(pu.cur_color, fd.theme.hover, sizeof pu.cur_color);  // (a popup has no cursor: the slot carries the colour of cells under the mouse)
            pu.radii[0] = std::min(p.radius, static_cast<float>(std::min(p.w, p.h)) * 0.5f);
            pu.radii[1] = p.pill;
            pu.shadow[0] = p.shadow;
            pu.shadow[1] = shadowed && laid_over ? fd.theme.shadow * p.shadow_alpha : 0.0f;
            pu.shadow[2] = p.shadow_dx;
            pu.shadow[3] = p.shadow_dy;
            pu.inset[0] = p.pill_dx;
            pu.inset[1] = p.pill_dy;
            pu.inset[3] = p.pill_out;
            pu.inset[2] = laid_over ? p.opacity : 1.0f;
            pu.origin[0] = -static_cast<float>(p.x + p.pad_x);
            pu.origin[1] = -static_cast<float>(p.y + p.pad_y);
            pu.cols = p.cols;
            pu.rows = p.rows;
            pu.cell_base = base;
            pu.frame_rect[0] = static_cast<float>(p.x);
            pu.frame_rect[1] = static_cast<float>(p.y);
            pu.frame_rect[2] = static_cast<float>(p.x + p.w);
            pu.frame_rect[3] = static_cast<float>(p.y + p.h);
            std::memcpy(pu.border, fd.theme.popup_border, sizeof(float) * 3);
            pu.border[3] = p.border_px;
            auto draw = draw_in;
            if (laid_over) {
                bind(BLIT_PIPE_POPUP_BLEND);
                push(pu);
                draw(ox0, oy0, ox1, oy1);
            } else if (p.pill <= 0 || p.pill_dy < 0 || p.rows < 2) {
                bind(p.pill > 0 ? BLIT_PIPE_POPUP_FX : BLIT_PIPE_POPUP);
                push(pu);
                draw(ix0, iy0, ix1, iy1);
            } else {
                // A tall box with rounded runs (the side panel): most of its
                // rows have nothing lit in them, and those are drawn by the
                // plain shader. Rows are taken in bands of one kind.
                const u32* box = cells + base;
                lit_rows_.assign(p.rows, 0);
                for (u32 r = 0; r < p.rows; ++r)
                    for (u32 c = 0; c < p.cols && !lit_rows_[r]; ++c) lit_rows_[r] = (box[static_cast<size_t>(r) * p.cols + c] & (kCellSelected | kCellHover)) != 0;
                const int ch = font.cell_h();
                for (u32 r = 0; r < p.rows;) {
                    u32 e = r + 1;
                    while (e < p.rows && lit_rows_[e] == lit_rows_[r]) ++e;
                    bind(lit_rows_[r] ? BLIT_PIPE_POPUP_FX : BLIT_PIPE_POPUP);
                    push(pu);
                    // (The margins above the first row and below the last go with those rows.)
                    const int top = r == 0 ? iy0 : p.y + p.pad_y + static_cast<int>(r) * ch;
                    const int bottom = e == p.rows ? iy1 : p.y + p.pad_y + static_cast<int>(e) * ch;
                    draw(ix0, std::max(top, iy0), ix1, std::min(bottom, iy1));
                    r = e;
                }
            }
            if (shadowed && !laid_over) {
                bind(BLIT_PIPE_POPUP_BLEND);
                pu.shadow[1] = fd.theme.shadow * p.shadow_alpha;
                push(pu);
                draw(ox0, oy0, ox1, std::min(iy0, oy1));                                       // above the box
                draw(ox0, std::max(iy1, oy0), ox1, oy1);                                       // below it
                draw(ox0, std::max(iy0, oy0), std::min(ix0, ox1), std::min(iy1, oy1));         // left of it
                draw(std::max(ix1, ox0), std::max(iy0, oy0), ox1, std::min(iy1, oy1));         // right of it
            }
        }
        base += n;
    }
    pictures_after(~0u);
    if (blit_gpu_end(gpu_, uniforms_.data(), sizeof(Uniforms), uniforms_.size() / sizeof(Uniforms), draws_.data(), static_cast<u32>(draws_.size()), &gt) != BLIT_FRAME_DRAWN)
        return kFailed;
    if (stats) {
        const u64 t5 = now_ns();
        stats->fill_ns = fill_ns;
        stats->layout_ns = layout_ns;
        stats->wait_ns = gt.target_ns;
        stats->cpu_ns = t5 - t0 - wait;
        stats->begin_ns = early ? gt.free_ns : t_begin - t0 - wait;  // (waiting for a frame's memory: the GPU is behind)
        stats->upload_ns = t3 - t2;             // (the atlas rows; the cells are written where the GPU reads them)
        stats->pass_ns = t5 - t3 - gt.submit_ns;
        stats->submit_ns = gt.submit_ns;
        stats->uploaded = atlas_changed;
    }
    return kDrawn;
}

void Renderer::wait_idle() {
    if (gpu_) blit_gpu_wait_idle(gpu_);
}

bool Renderer::read_pixels(std::vector<u8>& rgba, int* w, int* h) {
    if (!gpu_ || !off_w_) return false;
    rgba.resize(static_cast<size_t>(off_w_) * static_cast<size_t>(off_h_) * 4);
    if (!blit_gpu_read_pixels(gpu_, rgba.data(), off_w_, off_h_)) return false;
    *w = off_w_;
    *h = off_h_;
    return true;
}

const char* Renderer::driver() const { return gpu_ ? blit_gpu_name(gpu_) : ""; }

void Renderer::setup_times(u64* library_ns, u64* pipelines_ns, bool* cached) const {
    uint64_t l = 0, p = 0;
    int c = 0;
    if (gpu_) blit_gpu_setup_times(gpu_, &l, &p, &c);
    *library_ns = l;
    *pipelines_ns = p;
    *cached = c != 0;
}

}  // namespace blit
