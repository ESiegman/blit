// Distraction-free and presentation modes: the text alone, the window full
// screen, without the tab bars, the breadcrumbs, the line numbers, the
// status bar, the side panel and the minimap (each as it was again after);
// presentation mode with the text larger too, for showing.
#include <algorithm>
#include <cmath>

#include "app.hpp"

namespace blit {

// The mode turned on (kFocusPlain, kFocusShow) or, if it is the one on, off.
void App::focus_toggle(u8 mode) {
    const u8 was = focus_mode_;
    focus_mode_ = was == mode ? static_cast<u8>(kFocusNone) : mode;
    if (was == kFocusNone && focus_mode_ != kFocusNone) {
        focus_pt_ = pt_;
        focus_full_ = win_ && (SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN);
        if (win_ && !focus_full_) SDL_SetWindowFullscreen(win_, true);
        panel_focus(false);
    }
    // The text's size: larger for showing, as it was otherwise.
    const float want = focus_mode_ == kFocusShow ? (cfg_.presentation_size > 0 ? cfg_.presentation_size : std::round(focus_pt_ * 1.6f)) : focus_pt_;
    if (focus_mode_ == kFocusShow || was == kFocusShow) zoom(want - pt_);
    if (focus_mode_ == kFocusNone) {
        if (win_ && !focus_full_) SDL_SetWindowFullscreen(win_, false);
    }
    for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
    place_panes(px_w_, px_h_);
    scroll_to_cursor();
    dirty_ = true;
}

// For the tests: the mode, the text's size, and what the chrome takes.
std::string App::focus_dump() {
    static const char* const kNames[] = {"off", "distraction-free", "presentation"};
    return std::string(kNames[focus_mode_]) + " | " + std::to_string(static_cast<int>(std::lround(pt_))) + "pt | tabs " + std::to_string(tabs_px()) + " status " +
           std::to_string(status_px()) + " gutter " + std::to_string(gutter_px()) + " panel " + std::to_string(panel_px()) + " crumbs " +
           std::to_string(crumbs_px(*pane_));
}

}  // namespace blit
