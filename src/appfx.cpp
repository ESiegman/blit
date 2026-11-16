// How things look and move: how round corners are and how far shadows
// reach (worked out from the [look] settings and the display's density),
// what the mouse is over, and the motion: popups that fade in, a cursor
// that glides, a scroll thumb that fades away when left alone.
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "tabbar.hpp"

namespace blit {

namespace {

constexpr double kFadeInS = 0.07;    // a popup, from nothing to all there
constexpr u64 kThumbRestNs = 1'200'000'000;  // the scroll thumb stays this long after the view last moved
constexpr double kThumbFadeS = 0.3;  // and then takes this long to go

}  // namespace

struct App::Fx {
    // The cursor on its way: where it is drawn and where it is going (x0 y0
    // x1 y1 in the text's own pixels: columns times the cell's width, lines
    // times its height, so that scrolling does not come into it), and in
    // which split's which document.
    double cur[4] = {0, 0, 0, 0}, to[4] = {0, 0, 0, 0};
    u64 cur_pane = 0, cur_doc = 0;
    bool cur_known = false;
    // Popups fading in, by kind: how far along each is (0 to 1), and
    // whether the frame being laid out shows one of that kind.
    float fade[kFxKinds] = {};
    bool shown[kFxKinds] = {};
};

void App::fx_create() { fx_ = std::make_shared<Fx>(); }

// The sizes of things, in pixels. Everything rounded is sized from one
// setting, so that the corners of a menu, of the row lit in it and of a
// selection in the text stay in proportion.
App::Look App::look() const {
    Look l;
    const float r = cfg_.look_rounding * scale_;
    const float cw = font_ ? static_cast<float>(font_->cell_w()) : 16, ch = font_ ? static_cast<float>(font_->cell_h()) : 31;
    l.box = std::round(r * 1.3f);
    l.pill = std::min(std::round(r * 0.85f), std::floor(ch / 2) - 1);
    // (What is selected in the text is rounded cell by cell: no more than half a cell.)
    l.sel = std::min(std::round(r * 0.55f), std::floor(std::min(cw, ch) / 2) - 1);
    l.match = std::min(std::round(r * 0.4f), std::floor(std::min(cw, ch) / 2) - 1);
    l.cursor = std::round(r * 0.25f);
    l.shadow = cfg_.look_shadows ? std::round(22 * scale_) : 0;
    return l;
}

// Something has begun to move: frames follow one another until it is there.
void App::fx_kick() {
    if (!animating_ && !fx_moving_) last_step_ns_ = now_ns() - 8'000'000;
    fx_moving_ = true;
}

// A box that floats over the rest (a menu, a completion list, hover text):
// round corners, a shadow under it, the row lit in it as a rounded bar, and
// it fades in, rising a little.
void App::fx_float(Popup& p, int kind) {
    const Look l = look();
    p.radius = l.box;
    p.pill = l.pill;
    if (l.pill > 0) {
        p.pill_dy = std::round(0.5f * scale_);
        p.pill_out = std::max(0.0f, static_cast<float>(p.pad_x) - std::round(3 * scale_));
    }
    p.shadow = l.shadow;
    p.shadow_dy = std::round(l.shadow * 0.3f);
    Fx& f = *fx_;
    if (!f.shown[kind] && f.fade[kind] <= 0) f.fade[kind] = 0.2f;  // (its first frame is not spent on nothing at all)
    f.shown[kind] = true;
    if (!fx_live_ || !cfg_.look_animations) return;
    const float t = std::min(f.fade[kind], 1.0f), eased = 1 - (1 - t) * (1 - t);
    p.opacity = eased;
    p.y += static_cast<int>(std::lround((1 - eased) * 4 * scale_));
    if (t < 1) fx_kick();
}

void App::fx_begin_layout() { std::fill(std::begin(fx_->shown), std::end(fx_->shown), false); }

void App::fx_end_layout() {
    for (int k = 0; k < kFxKinds; ++k)
        if (!fx_->shown[k]) fx_->fade[k] = 0;  // gone: the next one of its kind fades in afresh
}

// Where to draw the cursor this frame, given where it belongs (both in the
// text's own pixels). It glides there from where it was; it jumps when it
// has just come to another document, or would have further to go than the
// view is wide or high.
void App::fx_cursor(const double to[4], double out[4]) {
    Fx& f = *fx_;
    const bool same = f.cur_known && f.cur_pane == pane_->id && f.cur_doc == pane_->doc->id;
    const double far_x = pane_->x1 - pane_->text_x, far_y = pane_->y1 - pane_->text_y;
    std::copy(to, to + 4, f.to);
    if (!same || !fx_live_ || cfg_.look_cursor_glide_ms <= 0 || std::fabs(to[0] - f.cur[0]) > far_x || std::fabs(to[1] - f.cur[1]) > far_y) {
        std::copy(to, to + 4, f.cur);
    } else if (!std::equal(to, to + 4, f.cur)) {
        fx_kick();
    }
    f.cur_known = true;
    f.cur_pane = pane_->id;
    f.cur_doc = pane_->doc->id;
    std::copy(f.cur, f.cur + 4, out);
}

// Whether a split's scroll thumb shows, and how strongly: fully while the
// view moves or the mouse is by it, then fading away.
float App::fx_thumb(Pane& p, i64 sx, i64 sy) {
    if (!fx_live_ || !cfg_.look_animations) return 1;
    if (static_cast<double>(sx) != p.thumb_x || static_cast<double>(sy) != p.thumb_y || dragging_thumb_) {
        p.thumb_x = static_cast<double>(sx);
        p.thumb_y = static_cast<double>(sy);
        p.thumb_alpha = 1;
        p.thumb_until_ns = now_ns() + kThumbRestNs;
    }
    return p.thumb_alpha;
}

// Moves everything that is on its way along by dt seconds. True while
// something still is.
bool App::step_fx(double dt) {
    Fx& f = *fx_;
    bool moving = false;
    // The cursor: most of the way in the time the setting gives.
    if (f.cur_known && !std::equal(f.to, f.to + 4, f.cur)) {
        const double tau = cfg_.look_cursor_glide_ms * 1e-3 / 4;
        const double k = tau > 0 ? 1.0 - std::exp(-dt / tau) : 1.0;
        bool there = true;
        for (int i = 0; i < 4; ++i) {
            f.cur[i] += (f.to[i] - f.cur[i]) * k;
            there = there && std::fabs(f.to[i] - f.cur[i]) < 0.4;
        }
        if (there) std::copy(f.to, f.to + 4, f.cur);
        else moving = true;
        dirty_ = true;
    }
    for (int k = 0; k < kFxKinds; ++k) {
        if (!f.shown[k] || f.fade[k] >= 1) continue;
        f.fade[k] = std::min(1.0f, f.fade[k] + static_cast<float>(dt / kFadeInS));
        moving = moving || f.fade[k] < 1;
        dirty_ = true;
    }
    if (cfg_.look_animations) {
        const u64 now = now_ns();
        for (const std::unique_ptr<Pane>& p : panes_) {
            if (p->thumb_alpha <= 0 || now < p->thumb_until_ns) continue;
            p->thumb_alpha = std::max(0.0f, p->thumb_alpha - static_cast<float>(dt / kThumbFadeS));
            moving = moving || p->thumb_alpha > 0;
            dirty_ = true;
        }
    }
    fx_moving_ = moving;
    return moving;
}

// Everything where it is going, at once (a scripted run, between events).
void App::fx_settle() {
    Fx& f = *fx_;
    std::copy(f.to, f.to + 4, f.cur);
    for (int k = 0; k < kFxKinds; ++k)
        if (f.shown[k]) f.fade[k] = 1;
    if (!fx_live_)
        for (const std::unique_ptr<Pane>& p : panes_) p->thumb_alpha = 1, p->thumb_until_ns = ~0ull;
    fx_moving_ = false;
}

// How long the main loop may sleep before a scroll thumb starts to fade
// (-1: none is waiting to), and what it does when that time has come.
int App::fx_wait_ms() const {
    if (!fx_live_ || !cfg_.look_animations) return -1;
    const u64 now = now_ns();
    u64 soonest = ~0ull;
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->thumb_alpha > 0) soonest = std::min(soonest, p->thumb_until_ns);
    if (soonest == ~0ull) return -1;
    return soonest <= now ? 0 : static_cast<int>((soonest - now) / 1'000'000) + 1;
}

void App::fx_tick() {
    if (!fx_live_ || !cfg_.look_animations) return;
    const u64 now = now_ns();
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->thumb_alpha > 0 && now >= p->thumb_until_ns) fx_kick();
}

// What the mouse is over, of the things that light up under it: a tab, a
// row of the side panel, a scroll thumb's edge of a split. (-1, -1: it has
// left the window.)
void App::hover_update(int px, int py) {
    bool changed = false;
    const int bar = tabs_px(), cw = font_->cell_w();
    const bool carrying = dragging_ || dragging_thumb_;
    for (const std::unique_ptr<Pane>& p : panes_) {
        TabBar& t = *p->bar;
        int hover = -1;
        if (bar && !carrying && !t.pressed && px >= p->x0 && px < p->x1 && py >= p->y0 && py < p->y0 + bar) {
            const int col = static_cast<int>(std::floor(static_cast<double>(px - p->x0 - (cw / 2 + 1)) / cw)) + t.scroll;
            for (size_t i = 0; i < t.spans.size(); ++i)
                if (col >= t.spans[i].x0 && col < t.spans[i].x1) hover = static_cast<int>(i);
            if (hover < 0 && col >= t.plus - 1 && col <= t.plus + 1) hover = static_cast<int>(t.spans.size());  // the "+"
        }
        if (hover != t.hover) {
            t.hover = hover;
            changed = true;
        }
        // By the right edge of the text: the scroll thumb comes back.
        if (fx_live_ && cfg_.look_animations && p->has_thumb && px >= p->x1 - static_cast<int>(std::lround(14 * scale_)) && px < p->x1 && py >= p->text_y && py < p->y1) {
            changed = changed || p->thumb_alpha < 1;
            p->thumb_alpha = 1;
            p->thumb_until_ns = now_ns() + kThumbRestNs;
        }
    }
    if (panel_hover(carrying ? -1 : px, py)) changed = true;
    if (changed) dirty_ = true;
}

}  // namespace blit
