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
#include <cctype>
#include <cstring>

#include "app.hpp"
#include "applua.hpp"
#include "look.hpp"
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

bool word_byte(u8 b) { return std::isalnum(b) || b == '_' || b >= 0x80; }

// The word at a place in the text (the one ending there, if it is between two).
std::string word_at(const Buffer& buf, u64 off) {
    u64 w0 = off, w1 = off;
    char b;
    while (w0 > 0 && off - w0 < 200 && buf.read(w0 - 1, &b, 1) == 1 && word_byte(static_cast<u8>(b))) --w0;
    while (w1 < buf.size() && w1 - off < 200 && buf.read(w1, &b, 1) == 1 && word_byte(static_cast<u8>(b))) ++w1;
    return buf.text(w0, w1 - w0);
}

// A tab with text of its own to search (not a terminal, a profile, a
// preview, a picture or a PDF).
bool has_text(const Doc& d) { return !d.term && !d.prof && !d.md && !d.image && !d.pdf; }

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
    // What Cmd+F last looked for without modes (with them, Vim's pattern is
    // what Cmd+G looks for).
    std::string find_text;
    // The side panel's tab a command last showed, while it may still have
    // the keyboard (see show_tab()).
    enum : int { kNone = -1, kProblems, kGit, kProfile, kDebug, kBuild };
    int shown = kNone;
    static void show_tab(App& a, int tab, bool toggle);
    static void find_next(App& a, bool forward);
    static bool git_ready(App& a);
};

// A side panel tab's key shows that tab, and pressed again while the tab
// has the keyboard, gives the keyboard back to the text (as Cmd+Shift+E does
// for the Files tab). Which tab has it is the panel's to know: this goes by
// the tab a command showed last, which is right unless another tab was
// clicked since.
void App::Commands::show_tab(App& a, int tab, bool toggle) {
    Commands& c = *a.commands_;
    if (!a.panel_has_keyboard()) c.shown = kNone;
    if (toggle && c.shown == tab) {
        a.panel_focus(false);
        c.shown = kNone;
        return;
    }
    switch (tab) {
        case kProblems: a.panel_show_problems(); break;
        case kGit: a.panel_show_git(); break;
        case kProfile: a.panel_show_profile(); break;
        case kDebug: a.panel_show_debug(); break;
        default: a.panel_show_build(); break;
    }
    c.shown = tab;
}

// Whether there is a repository for git's commands to act on; if not, the
// bottom line says why.
bool App::Commands::git_ready(App& a) {
    if (!a.cfg_.git) a.notify("Git is off (git.enabled in Settings)");
    else if (!a.git_summary().repo) a.notify("The project folder is not a git repository");
    else return true;
    return false;
}

// The next (or previous) match of what was looked for in the file: Vim's n
// and N with modes; without them, from the selection on, round the ends,
// and chosen.
void App::Commands::find_next(App& a, bool forward) {
    Doc& d = *a.pane_->doc;
    if (!has_text(d)) return;
    if (a.panel_has_keyboard()) a.panel_focus(false);
    Editor& ed = d.ed;
    a.lsp_close(kPopAll);
    if (a.cfg_.modal) {
        a.vim_attach();
        const char* keys = forward ? "n" : "N";
        if (a.vim_.mode() == Vim::kCommand) {
            // A search being typed is looked for (Enter); a command line is left alone.
            const std::string line = a.vim_.status();
            if (line.empty() || (line[0] != '/' && line[0] != '?')) return;
            keys = forward ? "<CR>" : "<CR>N";
        } else if (a.vim_.mode() == Vim::kInsert || a.vim_.mode() == Vim::kReplace) {
            keys = forward ? "<C-o>n" : "<C-o>N";  // (and on typing there)
        }
        a.vim_.keys(ed, keys);
        a.vim_after(Vim::kHandled);
        return;
    }
    const std::string& text = a.commands_->find_text;
    if (text.empty()) {
        a.notify("Nothing to find yet: " + key_symbols(chord_text(SDLK_F, kPrimaryMod), *a.font_) + " finds in the file");
        return;
    }
    // Capitals typed: they count; none: neither do the text's.
    Vim::Pattern p;
    p.text = text;
    p.ignore_case = std::none_of(text.begin(), text.end(), [](char ch) { return std::isupper(static_cast<unsigned char>(ch)) != 0; });
    const u64 at = Vim::find(ed.buf, p, ed.sel_min(), forward, true);
    if (at == ~0ull) {
        a.notify("No match for '" + text + "'");
        return;
    }
    ed.move_to(at, false);
    ed.move_to(at + text.size(), true);
    a.scroll_to_cursor();
    a.dirty_ = true;
}

void App::commands_create() {
    commands_ = std::make_shared<Commands>();
    std::vector<Commands::Command>& c = commands_->all;
    auto key = [](SDL_Keycode k, u8 m = 0) { return Chord{k, m}; };
    // Keys handled elsewhere: run by sending them.
    auto sent = [&](std::string title, std::vector<Chord> keys) { c.push_back({std::move(title), std::move(keys), false, nullptr}); };
    // Keys handled here (or none at all), and what they do.
    auto own = [&](std::string title, std::vector<Chord> keys, std::function<void()> run) { c.push_back({std::move(title), std::move(keys), true, std::move(run)}); };
    // A side panel tab, shown (or, its key again, left).
    auto tab = [&](std::string title, std::vector<Chord> keys, int which) {
        own(std::move(title), std::move(keys), [this, which] { Commands::show_tab(*this, which, true); });
    };
    // A git action, as the Git tab has it: the Git tab shows how it goes.
    auto git = [&](std::string title, std::string_view action) {
        own(std::move(title), {}, [this, action] {
            if (!Commands::git_ready(*this)) return;
            Commands::show_tab(*this, Commands::kGit, false);
            git_command(action, {});
        });
    };
    // One that asks for a name first, in the box that finds things.
    auto git_named = [&](std::string title, std::string prompt, std::string_view action) {
        own(std::move(title), {}, [this, prompt, action] {
            if (!Commands::git_ready(*this)) return;
            find_choose(kFindInput, prompt, {}, [this, action](const std::string& name) {
                if (name.empty()) return;
                Commands::show_tab(*this, Commands::kGit, false);
                git_command(action, name);
            });
        });
    };
    // Where Cmd is the editor's own key, Ctrl is free for keys of its own;
    // elsewhere Ctrl is the editor's, and the keys of a Mac's Cmd+G go to F3.
    const bool mac = kPrimaryMod == SDL_KMOD_GUI;
    const u8 ctl = mac ? kC : kP;

    // Files.
    sent("New file", {key(SDLK_N, kP), key(SDLK_T, kP)});
    sent("Open a file\xE2\x80\xA6", {key(SDLK_O, kP)});
    own("Open a folder\xE2\x80\xA6", {key(SDLK_O, kP | kS)}, [this] { find_open(kFindFolder); });
    own("New project\xE2\x80\xA6", {key(SDLK_N, kP | kA)}, [this] { project_anywhere(); });
    own("Connect to a server\xE2\x80\xA6", {}, [this] { find_open(kFindFolder, "ssh://"); });
    own("Reconnect to the servers that dropped", {}, [this] { remote_reconnect(); });
    sent("Save", {key(SDLK_S, kP)});
    own("Save as\xE2\x80\xA6", {key(SDLK_S, kP | kS)}, [this] { save_as(); });
    own("Save all", {key(SDLK_S, kP | kA)}, [this] { save_all(); });
    sent("Close the tab", {key(SDLK_W, kP)});
    sent("Open Settings (init.lua)", {key(SDLK_COMMA, kP)});
    sent("Quit", {key(SDLK_Q, kP)});
    // Finding.
    own("Command search", {key(SDLK_P, kP | kS)}, [this] { palette_open(); });
    sent("Go to file", {key(SDLK_P, kP)});
    sent("Search in files", {key(SDLK_F, kP | kS)});
    own("Find in this file", {key(SDLK_F, kP)}, [this] { find_in_file(); });
    own("Find the next match", {mac ? key(SDLK_G, kP) : key(SDLK_F3)}, [this] { Commands::find_next(*this, true); });
    own("Find the previous match", {mac ? key(SDLK_G, kP | kS) : key(SDLK_F3, kS)}, [this] { Commands::find_next(*this, false); });
    own("Go to the next hit of Search in files", {key(SDLK_F4)}, [this] { find_step(1); });
    own("Go to the previous hit of Search in files", {key(SDLK_F4, kS)}, [this] { find_step(-1); });
    // Editing.
    sent("Undo", {key(SDLK_Z, kP)});
    sent("Redo", {key(SDLK_Z, kP | kS), key(SDLK_Y, kP)});
    sent("Cut", {key(SDLK_X, kP)});
    sent("Copy", {key(SDLK_C, kP)});
    sent("Paste", {key(SDLK_V, kP)});
    sent("Select all", {key(SDLK_A, kP)});
    // The view.
    sent("Show or hide the side panel", {key(SDLK_B, kP)});
    sent("Show Files", {key(SDLK_E, kP | kS)});
    tab("Show Problems", {key(SDLK_M, kP | kS)}, Commands::kProblems);
    tab("Show Git", {key(SDLK_G, ctl | kS)}, Commands::kGit);
    tab("Show Profile", {key(SDLK_R, kP | kS)}, Commands::kProfile);
    sent("Show or hide who changed each line (git blame)", {key(SDLK_B, kP | kA)});
    sent("Make the text bigger", {key(SDLK_EQUALS, kP)});
    sent("Make the text smaller", {key(SDLK_MINUS, kP)});
    sent("Reset text size", {key(SDLK_0, kP)});
    // Tabs.
    sent("Go to the next tab", {key(SDLK_TAB, kC), key(SDLK_RIGHTBRACKET, kP | kS)});
    sent("Go to the previous tab", {key(SDLK_TAB, kC | kS), key(SDLK_LEFTBRACKET, kP | kS)});
    sent("Go to the last tab", {key(SDLK_9, kP)});
    for (int n = 1; n <= 8; ++n) sent("Go to tab " + std::to_string(n), {key(static_cast<SDL_Keycode>(SDLK_0 + n), kP)});
    own("Move the tab left", {key(SDLK_PAGEUP, kC | kS)}, [this] { tab_shift(-1); });
    own("Move the tab right", {key(SDLK_PAGEDOWN, kC | kS)}, [this] { tab_shift(1); });
    // Splits.
    sent("Split to the right", {key(SDLK_BACKSLASH, kP)});
    sent("Split below", {key(SDLK_BACKSLASH, kP | kS)});
    sent("Go to the split on the left", {key(SDLK_LEFT, kP | kA)});
    sent("Go to the split on the right", {key(SDLK_RIGHT, kP | kA)});
    sent("Go to the split above", {key(SDLK_UP, kP | kA)});
    sent("Go to the split below", {key(SDLK_DOWN, kP | kA)});
    own("Move the tab to the split on the left", {key(SDLK_LEFT, kP | kA | kS)}, [this] { tab_to_split(kEdgeLeft); });
    own("Move the tab to the split on the right", {key(SDLK_RIGHT, kP | kA | kS)}, [this] { tab_to_split(kEdgeRight); });
    own("Move the tab to the split above", {key(SDLK_UP, kP | kA | kS)}, [this] { tab_to_split(kEdgeTop); });
    own("Move the tab to the split below", {key(SDLK_DOWN, kP | kA | kS)}, [this] { tab_to_split(kEdgeBottom); });
    own("Make the split wider", {key(SDLK_RIGHT, kP | kC)}, [this] { split_grow(true, 1); });
    own("Make the split narrower", {key(SDLK_LEFT, kP | kC)}, [this] { split_grow(true, -1); });
    own("Make the split taller", {key(SDLK_DOWN, kP | kC)}, [this] { split_grow(false, 1); });
    own("Make the split shorter", {key(SDLK_UP, kP | kC)}, [this] { split_grow(false, -1); });
    // Code.
    sent("Show completions", {key(SDLK_SPACE, kC)});
    sent("Show the function's parameters", {key(SDLK_SPACE, kC | kS)});
    sent("Show what this is", {key(SDLK_I, kP)});
    sent("Go to the definition", {key(SDLK_F12)});
    sent("Go back", {mac ? key(SDLK_MINUS, kC) : key(SDLK_LEFT, kA)});
    own("Go to the next problem", {key(SDLK_F8)}, [this] { problem_step(1); });
    own("Go to the previous problem", {key(SDLK_F8, kS)}, [this] { problem_step(-1); });
    // Markdown.
    own("Preview Markdown beside the file", {key(SDLK_V, kP | kS)}, [this] { md_open(); });
    // Terminals: in the split along the bottom.
    own("Show or hide the terminal", {key(SDLK_GRAVE, ctl)}, [this] { term_toggle(); });
    own("New terminal", {key(SDLK_GRAVE, ctl | kS)}, [this] { term_new(); });
    // Debugging.
    own("Start or continue debugging", {key(SDLK_F5)}, [this] { debug_start(); });
    own("Stop debugging", {key(SDLK_F5, kS)}, [this] { debug_command("stop"); });
    own("Pause the program being debugged", {key(SDLK_F6)}, [this] { debug_command("pause"); });
    own("Add or remove a breakpoint", {key(SDLK_F9)}, [this] { debug_toggle_break(); });
    own("Step over", {key(SDLK_F10)}, [this] { debug_command("next"); });
    // (F11 shows the desktop on a Mac as it comes: F7 too.)
    own("Step into", {key(SDLK_F7), key(SDLK_F11)}, [this] { debug_command("stepIn"); });
    own("Step out", {key(SDLK_F11, kS)}, [this] { debug_command("stepOut"); });
    own("Evaluate an expression while stopped\xE2\x80\xA6", {key(SDLK_Y, kP | kS)}, [this] { debug_evaluate(); });
    tab("Show Debug", {key(SDLK_D, kP | kS)}, Commands::kDebug);
    // Building.
    own("Run the build task", {key(SDLK_B, kP | kS)}, [this] { build_run(-1); });
    own("Stop the build", {key(SDLK_PERIOD, kP)}, [this] { build_stop(); });
    tab("Show Build", {key(SDLK_U, kP | kS)}, Commands::kBuild);
    own("Look for build systems again", {}, [this] { build_changed(); });
    // Profiling.
    own("Record a profile of the Profile tab's program", {}, [this] {
        Commands::show_tab(*this, Commands::kProfile, false);
        prof_record();
    });
    own("Stop recording", {}, [this] { prof_stop(); });
    own("Show or hide inline performance labels", {}, [this] { prof_toggle_labels(); });
    sent("Zoom the flame graph into the chosen function", {key(SDLK_SPACE)});
    sent("Zoom the flame graph out", {key(SDLK_ESCAPE)});
    // Git.
    git("Git: Commit", "commit");
    git("Git: Pull", "pull");
    git("Git: Push", "push");
    git("Git: Fetch", "fetch");
    git("Git: Stage all", "stageall");
    git_named("Git: New branch\xE2\x80\xA6", "New branch", "branch");
    git_named("Git: Switch branch\xE2\x80\xA6", "Switch to branch", "switch");
}

// Cmd+F: a search in the file in front, starting from the selection (its
// first line) or else the word at the cursor. With modes it is Vim's / with
// that typed; without, the box that finds things as one line to type.
void App::find_in_file() {
    Doc& d = *pane_->doc;
    if (!has_text(d) || d.ed.buf.size() == 0) return;  // (the start screen's empty file too)
    if (panel_has_keyboard()) panel_focus(false);
    Editor& ed = d.ed;
    u64 lo = ed.sel_min(), hi = ed.sel_max();
    if (cfg_.modal) {
        vim_attach();
        if (!vim_selection(&lo, &hi)) lo = hi = 0;
    }
    std::string what = hi > lo ? ed.buf.text(lo, std::min<u64>(hi - lo, 400)) : std::string();
    what.resize(std::min(what.size(), what.find_first_of("\r\n")));
    if (what.empty()) what = word_at(ed.buf, ed.cursor());
    lsp_close(kPopAll);
    if (cfg_.modal) {
        // (Typed as keys: a backslash stands for itself, and "<" is written <lt>.)
        std::string keys = "<Esc>/";
        for (const char ch : what) keys += ch == '\\' ? "\\\\" : ch == '<' ? "<lt>" : std::string(1, ch);
        vim_.keys(ed, keys);
        vim_after(Vim::kHandled);
        return;
    }
    find_choose(kFindInput, "Find", what, [this](const std::string& typed) {
        if (!typed.empty()) commands_->find_text = typed;
        Commands::find_next(*this, true);
    });
}

// The commands' keys this file handles (the others are their owners').
bool App::command_key(const SDL_KeyboardEvent& k) {
    if (!commands_) return false;
    const u8 m = mods_of(k.mod);
    // (The panel's own keys for another of its tabs: which tab a command
    // showed last no longer says which has the keyboard.)
    if ((k.key == SDLK_TAB && (k.mod & SDL_KMOD_CTRL)) || (k.key == SDLK_E && m == (kP | kS))) commands_->shown = Commands::kNone;
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
