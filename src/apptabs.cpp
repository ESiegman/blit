// The open files: one tab each along the top of the text area, and the line
// numbers down its left. Both are drawn like the side panel and the popups,
// as a grid of cells of their own laid over the window.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "app.hpp"
#include "remote.hpp"
#include "tabbar.hpp"
#include "textbox.hpp"
#include "tree.hpp"

namespace blit {

std::string parent_of(const std::string& path) {
    std::string host, there;
    if (split_remote(path, &host, &there)) return remote_path(host, parent_of(there));  // (another machine's: its parent there)
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string full_path(const std::string& path) {
    if (path.empty()) return {};
    if (path[0] == '/' || is_remote(path)) return path;
    char* cwd = SDL_GetCurrentDirectory();
    std::string out = cwd ? cwd : "";
    SDL_free(cwd);
    if (!out.empty() && out.back() != '/') out += '/';
    return out + path;
}

namespace {

constexpr u32 kMaxLabel = 32;  // columns of a tab's name before it is cut short
constexpr u32 kBlameCols = 32;  // width of the blame column, when it is on

std::string_view base_name(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// "folder/name": told apart from another tab with the same name.
std::string_view with_folder(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos || slash == 0) return path;
    const size_t before = path.find_last_of("/\\", slash - 1);
    return before == std::string_view::npos ? path : path.substr(before + 1);
}

}  // namespace

// The window starts as one split with one new file.
void App::tabs_create() {
    pane_ = add_pane();
    split_ = std::make_unique<SplitNode>();
    split_->pane = pane_;
    pane_->doc = add_doc(0);
    pane_->tab = 0;
}

Pane* App::add_pane() {
    auto p = std::make_unique<Pane>();
    p->id = next_pane_id_++;
    p->bar = std::make_shared<TabBar>();
    panes_.push_back(std::move(p));
    return panes_.back().get();
}

// ---------------------------------------------------------------- documents

Doc* App::add_doc(size_t at) {
    auto d = std::make_unique<Doc>();
    d->ed.tab_width = cfg_.tab_width;
    d->id = next_doc_id_++;
    Doc* raw = d.get();
    at = std::min(at, pane_->docs.size());
    pane_->docs.insert(pane_->docs.begin() + static_cast<std::ptrdiff_t>(at), std::move(d));
    if (pane_->doc && at <= pane_->tab) ++pane_->tab;  // the one in front moved along
    return raw;
}

Doc* App::find_doc(u64 id, Pane** pane, size_t* index) {
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->id == id) {
                if (pane) *pane = p.get();
                if (index) *index = i;
                return p->docs[i].get();
            }
    return nullptr;
}

bool App::find_open(const std::string& full, Pane** pane, size_t* index) const {
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (!p->docs[i]->ed.path.empty() && full_path(p->docs[i]->ed.path) == full) {
                *pane = p.get();
                *index = i;
                return true;
            }
    return false;
}

// Runs fn once for every open file, in every split, with that file standing
// as the one in front of the focused split while it runs.
void App::for_each_doc(std::function_ref<void()> fn) {
    Pane* const focused = pane_;
    for (const std::unique_ptr<Pane>& p : panes_) {
        Doc* const front = p->doc;
        pane_ = p.get();
        for (const std::unique_ptr<Doc>& d : p->docs) {
            p->doc = d.get();
            fn();
        }
        p->doc = front;
    }
    pane_ = focused;
}

// Brings a tab to the front. The view of the one that was there is kept for
// when it comes back; the language server moves over to the new one.
void App::tab_show(size_t i) {
    if (i >= pane_->docs.size()) return;
    pane_->bar->reveal = true;
    dirty_ = true;
    if (pane_->doc == pane_->docs[i].get()) {
        pane_->tab = i;
        return;
    }
    if (pane_->doc) {
        stop_lsp(true);  // it listens to the document in front; its server may do for the next one
        pane_->doc->scroll_x = pane_->target_x;
        pane_->doc->scroll_y = pane_->target_y;
    }
    pane_->tab = i;
    pane_->doc = pane_->docs[i].get();
    pane_->scroll_x = pane_->target_x = pane_->doc->scroll_x;
    pane_->scroll_y = pane_->target_y = pane_->doc->scroll_y;
    animating_ = dragging_ = dragging_thumb_ = false;
    pane_->cache_rev = ~0ull;
    lsp_pending_ = true;       // after the next frame, like at startup
    grammars_pending_ = true;  // this file's grammar may be one to fetch
    pick_theme();              // the colours of this document's language
    vim_attach();              // and Vim leaves the old document for this one
}

void App::tab_new() {
    add_doc(pane_->tab + 1);
    tab_show(pane_->tab + 1);
    panel_focus(false);
}

void App::tab_step(int by) {
    const int n = static_cast<int>(pane_->docs.size());
    tab_show(static_cast<size_t>(((static_cast<int>(pane_->tab) + by) % n + n) % n));
}

void App::tab_move(size_t from, size_t to) {
    if (from >= pane_->docs.size() || to >= pane_->docs.size() || from == to) return;
    std::unique_ptr<Doc> d = std::move(pane_->docs[from]);
    pane_->docs.erase(pane_->docs.begin() + static_cast<std::ptrdiff_t>(from));
    pane_->docs.insert(pane_->docs.begin() + static_cast<std::ptrdiff_t>(to), std::move(d));
    for (size_t i = 0; i < pane_->docs.size(); ++i)
        if (pane_->docs[i].get() == pane_->doc) pane_->tab = i;
    dirty_ = true;
}

// Closes a tab, asking first about changes that are not saved. The last
// tab closed leaves a new, empty file behind; closing that one closes blit.
bool App::tab_close(size_t i, bool force) {
    if (i >= pane_->docs.size()) return true;
    if (pane_->docs[i]->ed.modified() && !force) {
        tab_show(i);  // what the question is about
        const std::string name(pane_->doc->ed.path.empty() ? std::string_view("this new file") : base_name(pane_->doc->ed.path));
        const std::string what = "Save the changes to " + name + " before closing it?";
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Save"},
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
        if (!win_) hit = 2;  // scripted runs have nobody to ask
        else
#endif
        SDL_ShowMessageBox(&box, &hit);
        if (hit == 0) return false;
        if (hit == 1) {
            if (pane_->doc->ed.path.empty()) {
                // It needs a name first. The save dialog answers later, and
                // the tab closes then (see the handling of user event 1).
                close_after_save_ = true;
                save();
                return false;
            }
            save();
            if (pane_->doc->ed.modified()) return false;  // it would not save
        }
    }
    const bool front = pane_->docs[i].get() == pane_->doc;
    if (pane_->docs.size() == 1 && panes_.size() > 1) {  // its last tab: the split goes with it
        stop_lsp();
        remove_pane(pane_);
        return true;
    }
    if (pane_->docs.size() == 1) {
        if (pane_->doc->ed.path.empty() && pane_->doc->ed.buf.size() == 0) {
            running_ = false;
            return true;
        }
        stop_lsp();
        pane_->doc = nullptr;
        pane_->docs.clear();
        add_doc(0);
        tab_show(0);
        return true;
    }
    if (front) {
        stop_lsp();
        pane_->doc = nullptr;
    }
    pane_->docs.erase(pane_->docs.begin() + static_cast<std::ptrdiff_t>(i));  // its syntax worker stops with it
    if (front) {
        tab_show(std::min(i, pane_->docs.size() - 1));
    } else {
        for (size_t k = 0; k < pane_->docs.size(); ++k)
            if (pane_->docs[k].get() == pane_->doc) pane_->tab = k;
    }
    pane_->bar->reveal = true;
    dirty_ = true;
    return true;
}

// A file or folder was renamed (in the side panel): open files under the
// old name go by the new one.
void App::renamed_on_disk(const std::string& from, const std::string& to) {
    for (const std::unique_ptr<Pane>& p : panes_)
      for (const std::unique_ptr<Doc>& d : p->docs) {
        if (d->ed.path.empty()) continue;
        const std::string full = full_path(d->ed.path);
        if (full != from && !full.starts_with(from + "/")) continue;
        if (d.get() == pane_->doc) {
            stop_lsp();  // the server knows the file by its name
            lsp_pending_ = true;
        }
        d->ed.path = to + full.substr(from.size());
    }
    dirty_ = true;
}

// ---------------------------------------------------------------- input

int App::tabs_px() const {
    if (!cfg_.tab_bar || !font_) return 0;
    const int ch = font_->cell_h();
    return ch + 2 * (ch / 4);
}

bool App::tabs_key(const SDL_KeyboardEvent& k) {
    const bool cmd = k.mod & kPrimaryMod, shift = k.mod & SDL_KMOD_SHIFT, ctrl = k.mod & SDL_KMOD_CTRL;
    if (ctrl && k.key == SDLK_TAB) {
        tab_step(shift ? -1 : 1);
        return true;
    }
    if (ctrl && (k.key == SDLK_PAGEDOWN || k.key == SDLK_PAGEUP)) {
        tab_step(k.key == SDLK_PAGEDOWN ? 1 : -1);
        return true;
    }
    if (!cmd) return false;
    switch (k.key) {
        case SDLK_T: tab_new(); return true;
        case SDLK_W: tab_close(pane_->tab); return true;
        case SDLK_RIGHTBRACKET:
        case SDLK_LEFTBRACKET:
            if (!shift) return false;
            tab_step(k.key == SDLK_RIGHTBRACKET ? 1 : -1);
            return true;
        default: break;
    }
    if (k.key >= SDLK_1 && k.key <= SDLK_9) {  // the first eight tabs by number; 9 is the last
        const size_t n = static_cast<size_t>(k.key - SDLK_1);
        if (k.key == SDLK_9) tab_show(pane_->docs.size() - 1);
        else if (n < pane_->docs.size()) tab_show(n);
        return true;
    }
    return false;
}

bool App::tabs_mouse_down(const SDL_MouseButtonEvent& b) {
    TabBar& t = *pane_->bar;
    const int bar = tabs_px();
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (!bar || py < pane_->y0 || py >= pane_->y0 + bar || px < pane_->x0 || px >= pane_->x1) return false;
    const int cw = font_->cell_w();
    const int col = static_cast<int>(std::floor(static_cast<double>(px - pane_->x0 - (cw / 2 + 1)) / cw)) + t.scroll;
    panel_focus(false);
    lsp_close(kPopAll);
    dirty_ = true;
    for (size_t i = 0; i < t.spans.size() && i < pane_->docs.size(); ++i) {
        const TabBar::Span& s = t.spans[i];
        if (col < s.x0 || col >= s.x1) continue;
        if (b.button == SDL_BUTTON_MIDDLE || (b.button == SDL_BUTTON_LEFT && col >= s.mark - 1 && col <= s.mark + 1)) {
            tab_close(i);
        } else if (b.button == SDL_BUTTON_LEFT) {
            tab_show(i);
            t.pressed = true;
            t.dragging = false;
            t.held = pane_->docs[i]->id;
            t.press_x = b.x;
            // It may be carried off to another split; with Alt held, all
            // the tabs of this split go together.
            const bool all = SDL_GetModState() & SDL_KMOD_ALT;
            drag_arm(all ? kDragGroup : kDragTab, t.held, {}, b.x, b.y);
            if (all) t.pressed = false;
        }
        return true;
    }
    // Past the last tab: the "+", or a double click on the empty part. A
    // press there that turns into a drag carries the whole group of tabs.
    if (b.button == SDL_BUTTON_LEFT && ((col >= t.plus - 1 && col <= t.plus + 1) || b.clicks == 2)) tab_new();
    else if (b.button == SDL_BUTTON_LEFT) drag_arm(kDragGroup, 0, {}, b.x, b.y);
    return true;
}

// A held tab follows the mouse along the bar: it changes places with a
// neighbour when the mouse is past that neighbour's middle. (Past the
// middle, not merely over it: tabs differ in width, and one that swapped as
// soon as it touched a wider one would swap straight back.)
bool App::tabs_mouse_motion(const SDL_MouseMotionEvent& m) {
    TabBar& t = *pane_->bar;
    const int bar = tabs_px();
    if (!bar) return false;
    const int px = static_cast<int>(m.x * density_), py = static_cast<int>(m.y * density_);
    if (!t.pressed) {  // over a tab bar, anyone's: not the text's business
        const Pane* over = pane_at(px, py);
        return over && py < over->text_y;
    }
    if (!t.dragging && std::fabs(m.x - t.press_x) < 4) return true;
    t.dragging = true;
    size_t from = 0;
    Pane* where = nullptr;
    if (!find_doc(t.held, &where, &from) || where != pane_ || from >= t.spans.size()) return true;
    const int cw = font_->cell_w();
    const double col = static_cast<double>(px - pane_->x0 - (cw / 2 + 1)) / cw + t.scroll;
    size_t to = from;
    while (to + 1 < t.spans.size() && col >= (t.spans[to + 1].x0 + t.spans[to + 1].x1) * 0.5) ++to;
    while (to > 0 && to <= from && col < (t.spans[to - 1].x0 + t.spans[to - 1].x1) * 0.5) --to;
    if (to != from) {
        tab_move(from, to);
        t.reveal = true;
        // The spans are the last frame's. More mouse movement may arrive
        // before the next one, so they are put in the new order here.
        TabBar::Span moved = t.spans[from];
        t.spans.erase(t.spans.begin() + static_cast<std::ptrdiff_t>(from));
        t.spans.insert(t.spans.begin() + static_cast<std::ptrdiff_t>(to), moved);
        int at = 0;
        for (TabBar::Span& s : t.spans) {
            const int width = s.x1 - s.x0, mark = s.mark - s.x0;
            s.x0 = at;
            s.mark = at + mark;
            s.x1 = at += width;
        }
    }
    return true;
}

void App::tabs_mouse_up() {
    for (const std::unique_ptr<Pane>& p : panes_) p->bar->pressed = p->bar->dragging = false;
}

// The wheel over the bar slides the tabs, whichever way it turns.
bool App::tabs_wheel(const SDL_MouseWheelEvent& w) {
    TabBar& t = *pane_->bar;
    const int bar = tabs_px();
    if (!bar || mouse_x_ < 0) return false;
    const int px = static_cast<int>(mouse_x_ * density_), py = static_cast<int>(mouse_y_ * density_);
    if (py < pane_->y0 || py >= pane_->y0 + bar || px < pane_->x0 || px >= pane_->x1) return false;
    const float d = std::fabs(w.x) > std::fabs(w.y) ? w.x : -w.y;
    t.scroll = std::clamp(t.scroll + static_cast<int>(std::lround(d * 3)), 0, std::max(0, t.width - t.cols));
    t.drawn = 0;
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- drawing

// Adds the tab bar to a frame. Its cells are made again only when what
// they show has changed: the files, their names, which have unsaved
// changes, which is in front, the room there is, the font.
void App::tabs_layout(int w, int h, FrameDesc& fd, bool focused) {
    TabBar& t = *pane_->bar;
    (void)h;
    const int bar = tabs_px();
    if (!bar || fd.popup_count >= std::size(fd.popups)) return;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    (void)w;
    const int pad_x = cw / 2 + 1, width = std::max(1, pane_->x1 - pane_->x0);
    const u32 cols = static_cast<u32>(std::max(1, (width - 2 * pad_x) / cw));

    // One number that changes whenever any of that does.
    auto signature = [&] {
        u64 sig = 1469598103934665603ull;
        auto mix = [&](u64 v) { sig = (sig ^ v) * 1099511628211ull; };
        mix(pane_->docs.size());
        mix(pane_->tab);
        mix(cols);
        mix(font_->generation());
        mix(static_cast<u64>(t.scroll));
        mix(t.reveal);
        mix(static_cast<u64>(t.drop + 1));
        mix(static_cast<u64>(t.hover + 1));
        mix(focused);
        for (const std::unique_ptr<Doc>& d : pane_->docs) {
            mix(d->id);
            mix(d->ed.modified());
            mix(std::hash<std::string>{}(d->ed.path));
            mix(std::hash<std::string>{}(d->title));
        }
        return sig;
    };
    if (signature() != t.drawn) {
        Font& font = *font_;
        // What each tab says: the file's name, with its folder when two
        // tabs would otherwise read the same.
        std::vector<std::string> labels(pane_->docs.size());
        for (size_t i = 0; i < pane_->docs.size(); ++i) {
            const std::string& path = pane_->docs[i]->ed.path;
            if (!pane_->docs[i]->title.empty()) {  // a diff, a commit: what it was called when it was made
                labels[i] = pane_->docs[i]->title;
                continue;
            }
            if (path.empty()) {
                labels[i] = "untitled";
                continue;
            }
            const std::string_view name = base_name(path);
            bool twice = false;
            for (size_t k = 0; k < pane_->docs.size() && !twice; ++k) twice = k != i && base_name(pane_->docs[k]->ed.path) == name;
            labels[i] = std::string(twice ? with_folder(path) : name);
            if (width_of(labels[i]) > kMaxLabel) {
                // Keep the end: that is where names differ.
                size_t cut = labels[i].size() - std::min<size_t>(labels[i].size(), kMaxLabel - 1);
                while (cut < labels[i].size() && is_cont(static_cast<u8>(labels[i][cut]))) ++cut;
                labels[i] = "\xE2\x80\xA6" + labels[i].substr(cut);
            }
        }
        // Where each goes: a space, the icon, a space, the name, a space,
        // the close mark, a space.
        t.spans.resize(pane_->docs.size());
        int at = 0;
        for (size_t i = 0; i < pane_->docs.size(); ++i) {
            TabBar::Span& s = t.spans[i];
            s.x0 = at;
            s.mark = at + 4 + static_cast<int>(width_of(labels[i])) + 1;
            s.x1 = s.mark + 2;
            at = s.x1;
        }
        t.plus = at + 1;
        t.width = at + 3;
        t.cols = static_cast<int>(cols);
        if (t.reveal && pane_->tab < t.spans.size()) {
            // The tab in front, whole, with a little of its neighbours.
            const TabBar::Span& s = t.spans[pane_->tab];
            if (s.x1 + 2 - t.scroll > t.cols) t.scroll = s.x1 + 2 - t.cols;
            if (s.x0 - 2 < t.scroll) t.scroll = s.x0 - 2;
        }
        t.reveal = false;
        t.scroll = std::clamp(t.scroll, 0, std::max(0, t.width - t.cols));

        TextBox& box = t.box;
        box.resize(cols, 1);
        const bool nerd = font.has(0xE5FF) && font.has(0xE61D);  // the font brings file icons of its own
        auto cell = [&](int col) -> u32* {
            const int c = col - t.scroll;
            return c >= 0 && c < static_cast<int>(cols) ? box.cells.data() + c : nullptr;
        };
        auto text = [&](int col, std::string_view s, u32 ink, u32 style) {
            // (A name partly scrolled off the left starts at its first
            // character that shows.)
            const u8* p = reinterpret_cast<const u8*>(s.data());
            size_t i = 0;
            int c = col - t.scroll;
            while (c < 0 && i < s.size()) {
                const Decoded d = decode_utf8(p + i, s.size() - i);
                i += d.len;
                c += static_cast<int>(cp_width(d.cp));
            }
            if (c >= 0 && c < static_cast<int>(cols)) box.put(font, 0, static_cast<u32>(c), s.substr(i), ink, style);
        };
        for (size_t i = 0; i < pane_->docs.size(); ++i) {
            const TabBar::Span& s = t.spans[i];
            if (s.x1 <= t.scroll || s.x0 - t.scroll >= static_cast<int>(cols)) continue;
            const bool front = i == pane_->tab, changed = pane_->docs[i]->ed.modified();
            const FileIcon ic = file_icon(base_name(pane_->docs[i]->title.empty() ? pane_->docs[i]->ed.path : pane_->docs[i]->title), false, false);
            if (u32* a = cell(s.x0 + 1), *b = cell(s.x0 + 2); a && b) {
                if (nerd) {
                    *a = (font.glyph(ic.nerd) & (Font::kWide - 1)) | (ic.ink << kCellInkShift);
                } else {
                    const u32 slot = font.icon(ic.drawn);
                    *a = slot | (ic.ink << kCellInkShift);
                    *b = (slot + 1) | (ic.ink << kCellInkShift);
                }
            }
            // (Bold in the split that has the keyboard: that is how to tell which one does.)
            text(s.x0 + 4, labels[i], front ? 0 : kInkMuted, front && focused ? Font::kBold : 0);
            // A file with unsaved changes shows a dot; the tab in front and
            // the one under the mouse show the cross that closes them.
            const bool under = static_cast<int>(i) == t.hover;
            if (changed) text(s.mark, "\xE2\x97\x8F", kInkAccent, 0);
            else if (front || under) text(s.mark, "\xC3\x97", kInkMuted, 0);
            if (front || under)
                for (int c = s.x0; c < s.x1; ++c)
                    if (u32* p = cell(c)) *p |= front ? kCellSelected : kCellHover;
        }
        text(t.plus, "+", kInkMuted, 0);
        if (t.hover == static_cast<int>(t.spans.size()))
            for (int c = t.plus - 1; c <= t.plus + 1; ++c)
                if (u32* p = cell(c)) *p |= kCellHover;
        // Where a tab being dragged in from another split would go.
        if (t.drop >= 0) {
            const int mark = static_cast<size_t>(t.drop) < t.spans.size() ? t.spans[static_cast<size_t>(t.drop)].x0 : (t.spans.empty() ? 0 : t.spans.back().x1);
            text(mark, "\xE2\x94\x82", kInkAccent, 0);
        }
        // A mark at either end says there are more tabs that way.
        if (t.scroll > 0) box.put(font, 0, 0, "\xE2\x80\xB9", kInkAccent);
        if (t.width - t.scroll > static_cast<int>(cols)) box.put(font, 0, cols - 1, "\xE2\x80\xBA", kInkAccent);

        t.drawn = signature();  // (after the scroll was settled: it is part of it)
    }

    Popup& p = fd.popups[fd.popup_count++];
    p.x = pane_->x0;
    p.y = pane_->y0;
    p.w = width;
    p.h = std::min(bar, pane_->y1 - pane_->y0);
    p.pad_x = pad_x;
    p.pad_y = (bar - ch) / 2;
    p.cols = t.box.cols;
    p.rows = t.box.rows;
    p.cells = t.box.cells.data();
    p.border_px = 0;
    p.sel = theme_.bg;  // the tab in front is the colour of the text under it
    // Each lit tab is a rounded box, a little taller than its letters and
    // with a gap to the next. And once text has slid up under the bar, the
    // bar casts a shadow on it.
    const Look lk = look();
    if (lk.pill > 0) {
        p.pill = lk.pill;
        p.pill_dy = -std::max(0.0f, static_cast<float>(p.pad_y) - std::round(2 * scale_));
        p.pill_dx = std::round(1.5f * scale_);
    }
    if (lk.shadow > 0 && pane_->scroll_y > 0) {
        p.shadow = std::round(lk.shadow * 0.5f);
        p.shadow_dy = std::round(p.shadow * 0.3f);
        p.shadow_alpha = 0.8f * static_cast<float>(std::min(1.0, pane_->scroll_y / (2.0 * ch)));
        p.clip[0] = pane_->x0, p.clip[1] = pane_->y0, p.clip[2] = pane_->x1, p.clip[3] = pane_->y1;
    }
}

// ---------------------------------------------------------------- line numbers

// Room for the number of the last line, and never fewer than three digits,
// so that the text does not shift about in a short file.
int App::gutter_px() const {
    if (!cfg_.line_numbers || !font_) return 0;
    u32 digits = 1;
    for (u64 n = pane_->doc->ed.buf.line_count(); n >= 10; n /= 10) ++digits;
    const int cw = font_->cell_w();
    // (A blank cell either side; with blame on, a column for it on the left.)
    return static_cast<int>(std::max<u32>(digits, 3) + 2 + (blame_shown(*pane_->doc) ? kBlameCols : 0)) * cw;
}

// The numbers are a grid one cell row taller than the text area, moved up by
// the same part of a row as the text is scrolled by, so the two stay level.
void App::gutter_layout(int w, int h, FrameDesc& fd) {
    TabBar& t = *pane_->bar;
    (void)w;
    const int gw = pane_->text_x - pane_->x0;
    (void)h;
    if (gw <= 0 || pane_->y1 <= pane_->text_y || fd.popup_count >= std::size(fd.popups)) return;
    const i64 cw = font_->cell_w(), ch = font_->cell_h();
    const i64 sy = std::llround(pane_->scroll_y);
    const i64 first = sy >= 0 ? sy / ch : -((-sy + ch - 1) / ch);  // before line 0 while the view is pulled down
    const u32 cols = static_cast<u32>(std::max<i64>(2, gw / cw)), rows = static_cast<u32>((pane_->y1 - pane_->text_y) / ch + 2);
    const u64 lines = pane_->doc->ed.buf.line_count();
    TextBox& box = t.gutter;
    const Doc& doc = *pane_->doc;
    // Blame is shown only while it is the blame of the text as it is.
    const bool shown = blame_shown(doc);
    const GitBlame* blame = shown && doc.blame && doc.blame_rev == doc.ed.revision() ? doc.blame.get() : nullptr;
    const bool relative = cfg_.relative_numbers && cfg_.modal;  // (counts for j and k are Vim's)
    const u64 git_key = ((((doc.id * 1000003 + doc.hunks_gen) * 1000003 + doc.blame_gen) * 4 + (shown ? 2 : 0) + (blame ? 1 : 0)) * 2 + (relative ? 1 : 0)) ^ debug_gen_ * 0x9E3779B97F4A7C15ull;
    const bool same_rows = first == t.g_first && lines == t.g_lines && font_->generation() == t.g_font && box.cols == cols && box.rows == rows && git_key == t.g_git;
    if (same_rows && pane_->cur_line != t.g_current && !blame && !relative) {
        // Only the cursor has gone to another line: the number of the line
        // it left and of the one it is on change colour, nothing else.
        auto ink_row = [&](u64 line, u32 ink) {
            const i64 r = static_cast<i64>(line) - first;
            if (r < 0 || r >= static_cast<i64>(rows) || line >= lines) return;
            u32* const start = box.cells.data() + static_cast<size_t>(r) * cols;
            u32* c = start + cols - 1;  // (the last cell, with the mark in it, is not a digit)
            for (u64 n = line + 1; n > 0 && c > start; n /= 10) {
                --c;
                *c = (*c & ~(0x1Fu << kCellInkShift)) | ink;
            }
        };
        ink_row(t.g_current, kInkMuted << kCellInkShift);
        ink_row(pane_->cur_line, 0);
        t.g_current = pane_->cur_line;
    } else if (!same_rows || pane_->cur_line != t.g_current) {
        t.g_first = first;
        t.g_current = pane_->cur_line;
        t.g_lines = lines;
        t.g_font = font_->generation();
        t.g_git = git_key;
        box.resize(cols, rows);
        Font& font = *font_;
        const std::vector<GitHunk>& hunks = doc.hunks;
        const i64 today = static_cast<i64>(std::time(nullptr));
        // Relative numbers: the cursor's line's own number starts where the
        // widest number does (as Vim has it), the others end at the right.
        const u64 cur = pane_->cur_line;
        u32 widest = 1;
        for (u64 n = lines; n >= 10; n /= 10) ++widest;
        widest = std::max<u32>(widest, 3);
        for (u32 r = 0; r < rows; ++r) {
            const i64 line = first + r;
            if (line < 0 || static_cast<u64>(line) >= lines) continue;
            // Digits are their own glyph slots; the cursor's line stands out.
            const u32 ink = static_cast<u64>(line) == pane_->cur_line ? 0 : kInkMuted << kCellInkShift;
            u32* const row = box.cells.data() + static_cast<size_t>(r) * cols;
            u32* end = row + cols - 1;  // the last cell stays blank
            const u64 at = static_cast<u64>(line);
            const u64 number = !relative || at == cur ? at + 1 : at > cur ? at - cur : cur - at;
            if (relative && at == cur) {
                u32 digits = 1;
                for (u64 n = number; n >= 10; n /= 10) ++digits;
                if (cols > widest + 1 && digits <= widest) end = row + (cols - 1 - widest) + digits;
            }
            for (u64 n = number; n > 0 && end > row; n /= 10) *--end = static_cast<u32>('0' + n % 10) | ink;
            if (const u32 mark = debug_gutter(doc, at)) row[0] = mark;  // a breakpoint, or where the program has stopped
            // In that last cell, a bar beside lines added or changed since
            // the last commit, and a dash where lines were taken out.
            if (!hunks.empty()) {
                auto it = std::upper_bound(hunks.begin(), hunks.end(), at, [](u64 l, const GitHunk& k) { return l < k.line; });
                u32 mark = 0;
                // (Lines taken from the very end are marked under the last line.)
                if (at + 1 == lines && hunks.back().kind == GitHunk::kDeleted && hunks.back().line >= lines)
                    mark = (font.glyph(0x2581) & (Font::kWide - 1)) | (kInkDiagnostic << kCellInkShift);
                if (it != hunks.begin()) {
                    const GitHunk& k = *std::prev(it);
                    if (k.kind == GitHunk::kDeleted && k.line == at) mark = (font.glyph(0x2594) & (Font::kWide - 1)) | (kInkDiagnostic << kCellInkShift);
                    else if (k.kind != GitHunk::kDeleted && at < u64(k.line) + k.count)
                        mark = (font.glyph(0x258E) & (Font::kWide - 1)) | ((k.kind == GitHunk::kAdded ? kInkAdded : kInkChanged) << kCellInkShift);
                }
                if (mark) box.cells[static_cast<size_t>(r + 1) * cols - 1] = mark;
            }
            // Blame: who and when, on the first line of each run of lines
            // from one commit (and on the top row, whatever it is).
            if (blame && static_cast<u64>(line) < blame->lines.size() && cols > kBlameCols) {
                const u32 which = blame->lines[static_cast<size_t>(line)];
                if (r == 0 || line == 0 || blame->lines[static_cast<size_t>(line) - 1] != which) {
                    const GitBlame::Commit& c = blame->commits[which];
                    std::string who = c.time ? c.hash.substr(0, 7) + " " + c.author : std::string("\xC2\xB7 not committed");
                    TextBox cell;  // (cut to the column's width)
                    cell.resize(kBlameCols - 2, 1);
                    const u32 end_col = cell.put(font, 0, 0, who, kInkMuted);
                    if (c.time) {
                        const std::string age = relative_age(c.time, today);
                        const u32 need = width_of(age);
                        if (need + 2 <= kBlameCols - 2) {
                            // (at the column's right; a long name gives way to it)
                            const u32 start = kBlameCols - 2 - need;
                            (void)end_col;
                            for (u32 k = start > 0 ? start - 1 : 0; k < kBlameCols - 2; ++k) cell.cells[k] = 0;
                            cell.put(font, 0, start, age, kInkMuted);
                        }
                    }
                    std::copy(cell.cells.begin(), cell.cells.end(), box.cells.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(r) * cols + 1));
                }
            }
        }
    }
    Popup& p = fd.popups[fd.popup_count++];
    p.x = pane_->x0;
    p.y = pane_->text_y;
    p.w = gw;
    p.h = pane_->y1 - pane_->text_y;
    p.pad_x = gw - static_cast<int>(cols * cw);
    p.pad_y = -static_cast<int>(sy - first * ch);
    p.cols = cols;
    p.rows = rows;
    p.cells = box.cells.data();
    p.border_px = 0;
    p.bg = theme_.bg;
    // Once text has slid left under the numbers, they cast a shadow on it.
    const Look lk = look();
    if (lk.shadow > 0 && pane_->scroll_x > 0) {
        p.shadow = std::round(lk.shadow * 0.5f);
        p.shadow_dx = std::round(p.shadow * 0.3f);
        p.shadow_alpha = 0.8f * static_cast<float>(std::min(1.0, pane_->scroll_x / (2.0 * static_cast<double>(cw))));
        p.clip[0] = pane_->x0, p.clip[1] = pane_->text_y, p.clip[2] = pane_->x1, p.clip[3] = pane_->y1;
    }
}

}  // namespace blit
