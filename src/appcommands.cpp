// Every command blit has, with its keys: what the command search (Cmd+Shift+P)
// lists and runs, and what makes the whole editor work from the keyboard.
//
// A command whose keys an older part of blit already handles (Cmd+P in
// appfind.cpp, Ctrl+Tab in apptabs.cpp ...) is run from the search by
// sending those very keys through on_key, so the list cannot say a key does
// something it does not. The commands added here (moving a tab to another
// split, resizing splits ...) have their keys handled here, by command_key.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

#include "app.hpp"
#include "applua.hpp"
#include "remote.hpp"

namespace blit {

namespace {

// Modifiers as the command list knows them: the editor's own (Cmd on a
// Mac, Ctrl elsewhere), Shift, Alt (Option), and Ctrl where it is not the
// editor's own.
enum : u8 { kP = 1, kS = 2, kA = 4, kC = 8 };

struct Chord {
    SDL_Keycode key = 0;
    u8 mods = 0;
};

u8 mods_of(SDL_Keymod m) {
    u8 out = 0;
    if (m & kPrimaryMod) out |= kP;
    if (m & SDL_KMOD_SHIFT) out |= kS;
    if (m & SDL_KMOD_ALT) out |= kA;
    if ((m & SDL_KMOD_CTRL) && kPrimaryMod != SDL_KMOD_CTRL) out |= kC;
    return out;
}

SDL_Keymod keymod_of(u8 mods) {
    int m = 0;
    if (mods & kP) m |= kPrimaryMod;
    if (mods & kS) m |= SDL_KMOD_SHIFT;
    if (mods & kA) m |= SDL_KMOD_ALT;
    if (mods & kC) m |= SDL_KMOD_CTRL;
    return static_cast<SDL_Keymod>(m);
}

}  // namespace

// A chord in words, as the README writes them: "Cmd+Shift+P", "Ctrl+`".
std::string App::chord_text(SDL_Keycode key, SDL_Keymod mod) {
    const bool mac = kPrimaryMod == SDL_KMOD_GUI;
    const u8 m = mods_of(mod);
    std::string s;
    if (m & kP) s += mac ? "Cmd+" : "Ctrl+";
    if (m & kC) s += "Ctrl+";
    if (m & kA) s += mac ? "Option+" : "Alt+";
    if (m & kS) s += "Shift+";
    std::string name = SDL_GetKeyName(key);
    if (name == "Return") name = "Enter";
    else if (name == "Escape") name = "Esc";
    else if (name == "PageUp") name = "PageUp";
    s += name.empty() ? "?" : name;
    return s;
}

struct App::Commands {
    struct Command {
        std::string title;
        std::vector<Chord> keys;
        bool here = false;              // its keys are handled by command_key
        std::function<void()> run;      // none: its first chord is sent through on_key
    };
    std::vector<Command> all;
};

void App::commands_create() {
    commands_ = std::make_shared<Commands>();
    std::vector<Commands::Command>& c = commands_->all;
    auto key = [](SDL_Keycode k, u8 m = 0) { return Chord{k, m}; };
    // Keys handled elsewhere: run by sending them.
    auto sent = [&](std::string title, std::vector<Chord> keys) { c.push_back({std::move(title), std::move(keys), false, nullptr}); };
    // Keys handled here (or none at all), and what they do.
    auto own = [&](std::string title, std::vector<Chord> keys, std::function<void()> run) { c.push_back({std::move(title), std::move(keys), true, std::move(run)}); };

    // Files.
    sent("New file (a new tab)", {key(SDLK_T, kP)});
    sent("Open a file\xE2\x80\xA6", {key(SDLK_O, kP)});
    own("Open a folder\xE2\x80\xA6", {key(SDLK_O, kP | kS)}, [this] { find_open(kFindFolder); });
    own("Connect to a server\xE2\x80\xA6", {}, [this] { find_open(kFindFolder, "ssh://"); });
    own("Reconnect to the servers that dropped", {}, [this] { remote_reconnect(); });
    sent("Save", {key(SDLK_S, kP)});
    sent("Close the tab", {key(SDLK_W, kP)});
    sent("Settings (init.lua)", {key(SDLK_COMMA, kP)});
    sent("Quit", {key(SDLK_Q, kP)});
    // Finding.
    own("Search commands and keys", {key(SDLK_P, kP | kS)}, [this] { palette_open(); });
    sent("Find a file in the folder", {key(SDLK_P, kP)});
    sent("Search the folder's files", {key(SDLK_F, kP | kS)});
    // Editing.
    sent("Undo", {key(SDLK_Z, kP)});
    sent("Redo", {key(SDLK_Z, kP | kS), key(SDLK_Y, kP)});
    sent("Cut", {key(SDLK_X, kP)});
    sent("Copy", {key(SDLK_C, kP)});
    sent("Paste", {key(SDLK_V, kP)});
    sent("Select all", {key(SDLK_A, kP)});
    // The view.
    sent("Show or hide the side panel", {key(SDLK_B, kP)});
    sent("Files (the side panel's file tree)", {key(SDLK_E, kP | kS)});
    own("Problems (the side panel)", {key(SDLK_M, kP | kS)}, [this] {
        if (!panel_px()) panel_toggle();
        panel_show_problems();
    });
    sent("Git (the side panel)", {key(SDLK_G, kP | kS)});
    own("Profile (the side panel)", {key(SDLK_R, kP | kS)}, [this] {
        if (!panel_px()) panel_toggle();
        panel_show_profile();
    });
    sent("Show or hide who changed each line (git blame)", {key(SDLK_B, kP | kA)});
    sent("Bigger text", {key(SDLK_EQUALS, kP)});
    sent("Smaller text", {key(SDLK_MINUS, kP)});
    sent("Text size as set", {key(SDLK_0, kP)});
    // Tabs.
    sent("Next tab", {key(SDLK_TAB, kC), key(SDLK_RIGHTBRACKET, kP | kS)});
    sent("Previous tab", {key(SDLK_TAB, kC | kS), key(SDLK_LEFTBRACKET, kP | kS)});
    sent("Last tab", {key(SDLK_9, kP)});
    for (int n = 1; n <= 8; ++n) sent("Tab " + std::to_string(n), {key(static_cast<SDL_Keycode>(SDLK_0 + n), kP)});
    own("Move the tab left", {key(SDLK_PAGEUP, kC | kS)}, [this] { tab_shift(-1); });
    own("Move the tab right", {key(SDLK_PAGEDOWN, kC | kS)}, [this] { tab_shift(1); });
    // Splits.
    sent("Split to the right", {key(SDLK_BACKSLASH, kP)});
    sent("Split below", {key(SDLK_BACKSLASH, kP | kS)});
    sent("To the split on the left", {key(SDLK_LEFT, kP | kA)});
    sent("To the split on the right", {key(SDLK_RIGHT, kP | kA)});
    sent("To the split above", {key(SDLK_UP, kP | kA)});
    sent("To the split below", {key(SDLK_DOWN, kP | kA)});
    own("Move the tab to the split on the left", {key(SDLK_LEFT, kP | kA | kS)}, [this] { tab_to_split(kEdgeLeft); });
    own("Move the tab to the split on the right", {key(SDLK_RIGHT, kP | kA | kS)}, [this] { tab_to_split(kEdgeRight); });
    own("Move the tab to the split above", {key(SDLK_UP, kP | kA | kS)}, [this] { tab_to_split(kEdgeTop); });
    own("Move the tab to the split below", {key(SDLK_DOWN, kP | kA | kS)}, [this] { tab_to_split(kEdgeBottom); });
    own("Make the split wider", {key(SDLK_RIGHT, kP | kC)}, [this] { split_grow(true, 1); });
    own("Make the split narrower", {key(SDLK_LEFT, kP | kC)}, [this] { split_grow(true, -1); });
    own("Make the split taller", {key(SDLK_DOWN, kP | kC)}, [this] { split_grow(false, 1); });
    own("Make the split shorter", {key(SDLK_UP, kP | kC)}, [this] { split_grow(false, -1); });
    // Code.
    sent("Completions", {key(SDLK_SPACE, kC)});
    sent("The function's parameters (signature help)", {key(SDLK_SPACE, kC | kS)});
    sent("What this is (hover)", {key(SDLK_I, kP)});
    sent("Go to the definition", {key(SDLK_F12)});
    sent("Go back", {kPrimaryMod == SDL_KMOD_GUI ? key(SDLK_MINUS, kC) : key(SDLK_LEFT, kA)});
    // Terminals.
    own("New terminal", {key(SDLK_GRAVE, kPrimaryMod == SDL_KMOD_GUI ? kC : kP)}, [this] { term_open(); });
    // Debugging.
    own("Start or continue debugging", {key(SDLK_F5)}, [this] { debug_start(); });
    own("Stop debugging", {key(SDLK_F5, kS)}, [this] { debug_command("stop"); });
    own("Pause the program being debugged", {key(SDLK_F6)}, [this] { debug_command("pause"); });
    own("Breakpoint on this line, or not", {key(SDLK_F9)}, [this] { debug_toggle_break(); });
    own("Step over", {key(SDLK_F10)}, [this] { debug_command("next"); });
    own("Step into", {key(SDLK_F11)}, [this] { debug_command("stepIn"); });
    own("Step out", {key(SDLK_F11, kS)}, [this] { debug_command("stepOut"); });
    own("Evaluate an expression (while stopped)\xE2\x80\xA6", {key(SDLK_Y, kP | kS)}, [this] { debug_evaluate(); });
    own("Debug (the side panel)", {key(SDLK_D, kP | kS)}, [this] {
        if (!panel_px()) panel_toggle();
        panel_show_debug();
    });
    // Profiling.
    own("Record a profile (the Profile tab's program)", {}, [this] {
        if (!panel_px()) panel_toggle();
        panel_show_profile();
        prof_record();
    });
    own("Stop recording", {}, [this] { prof_stop(); });
    own("Show or hide inline performance labels", {}, [this] { prof_toggle_labels(); });
    sent("Flame graph: zoom into the chosen function (a profile's tab)", {key(SDLK_SPACE)});
    sent("Flame graph: zoom out (a profile's tab)", {key(SDLK_ESCAPE)});
}

// The commands' keys this file handles (the others are their owners').
bool App::command_key(const SDL_KeyboardEvent& k) {
    if (!commands_) return false;
    const u8 m = mods_of(k.mod);
    for (const Commands::Command& c : commands_->all) {
        if (!c.here) continue;
        for (const Chord& ch : c.keys)
            if (ch.key == k.key && ch.mods == m) {
                c.run();
                return true;
            }
    }
    return false;
}

std::vector<App::PaletteItem> App::palette_items() {
    std::vector<PaletteItem> out;
    if (!commands_) return out;
    for (u32 i = 0; i < commands_->all.size(); ++i) {
        const Commands::Command& c = commands_->all[i];
        PaletteItem it{c.title, {}, i};
        for (const Chord& ch : c.keys) it.keys += (it.keys.empty() ? "" : "  ") + chord_text(ch.key, keymod_of(ch.mods));
        out.push_back(std::move(it));
    }
    // The scripts' commands (:Name), run as Vim's command line would.
    if (script_)
        for (u32 i = 0; i < script_->commands.size(); ++i) {
            const auto& sc = script_->commands[i];
            out.push_back({":" + sc.name + (sc.desc.empty() ? "" : "  " + sc.desc), {}, 0x80000000u | i});
        }
    return out;
}

void App::palette_run(u32 id) {
    if (id & 0x80000000u) {
        const u32 i = id & 0x7FFFFFFFu;
        if (!script_ || i >= script_->commands.size()) return;
        VimRequest q;
        q.kind = VimRequest::kEx;
        q.name = script_->commands[i].name;
        script_ex(q);
        dirty_ = true;
        return;
    }
    if (!commands_ || id >= commands_->all.size()) return;
    const Commands::Command& c = commands_->all[id];
    if (c.run) {
        c.run();
    } else if (!c.keys.empty()) {  // the very key, through the handlers that own it
        SDL_KeyboardEvent ev{};
        ev.type = SDL_EVENT_KEY_DOWN;
        ev.key = c.keys[0].key;
        ev.mod = keymod_of(c.keys[0].mods);
        ev.down = true;
        on_key(ev);
    }
    dirty_ = true;
}

// ---------------------------------------------------------------- what the new keys do

// The tab in front, one place left or right among its split's tabs.
void App::tab_shift(int by) {
    const size_t n = pane_->docs.size(), at = pane_->tab;
    if (by < 0 && at > 0) move_doc(pane_, at, pane_, at - 1);
    else if (by > 0 && at + 1 < n) move_doc(pane_, at, pane_, at + 2);
}

// The tab in front, into the split on that side (made, if there is none).
void App::tab_to_split(Edge edge) {
    Pane* const from = pane_;
    const int cx = (from->x0 + from->x1) / 2, cy = (from->y0 + from->y1) / 2;
    Pane* to = edge == kEdgeLeft    ? pane_at(from->x0 - 4, cy)
               : edge == kEdgeRight ? pane_at(from->x1 + 4, cy)
               : edge == kEdgeTop   ? pane_at(cx, from->y0 - 4)
                                    : pane_at(cx, from->y1 + 4);
    if (to == from) to = nullptr;
    if (!to) {
        if (from->docs.size() < 2) return;  // (its only tab: there is nothing to leave behind)
        to = split_pane(from, edge);
        if (!to) return;
        move_doc(from, from->tab, to, 0);
        // The split was made with a new empty file in it: that goes.
        for (size_t i = 0; i < to->docs.size(); ++i) {
            const Doc& d = *to->docs[i];
            if (to->docs.size() > 1 && d.ed.path.empty() && !d.ed.modified() && d.ed.buf.size() == 0 && !d.term && !d.prof && d.title.empty()) {
                Pane* const keep = pane_;
                pane_ = to;
                tab_close(i, true);
                pane_ = keep;
                break;
            }
        }
    } else {
        move_doc(from, from->tab, to, to->docs.size());
    }
    focus_pane(to);
    dirty_ = true;
}

// The focused split, wider (or narrower), or taller (or shorter): the
// nearest division of the window the right way round moves, by a twentieth.
void App::split_grow(bool across, int by) {
    // The path from the root to the focused pane's leaf.
    std::vector<SplitNode*> path;
    std::function<bool(SplitNode*)> find = [&](SplitNode* n) {
        if (!n) return false;
        path.push_back(n);
        if (n->pane == pane_) return true;
        if (find(n->a.get()) || find(n->b.get())) return true;
        path.pop_back();
        return false;
    };
    if (!find(split_.get())) return;
    for (size_t i = path.size() - 1; i-- > 0;) {
        SplitNode* n = path[i];
        if (!n->a || !n->b || n->side_by_side != across) continue;
        const bool first = n->a.get() == path[i + 1];
        const float d = 0.05f * static_cast<float>(by) * (first ? 1.0f : -1.0f);
        n->share = std::clamp(n->share + d, 0.1f, 0.9f);
        place_panes(px_w_, px_h_);
        for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
        dirty_ = true;
        return;
    }
}

}  // namespace blit
