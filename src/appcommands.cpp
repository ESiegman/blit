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
#include "regex.hpp"
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

// A key as the list names it. The bracket keys go by where they are: with
// Shift or Option held, a Mac reports what they type ("{", "“") instead.
SDL_Keycode key_of(const SDL_KeyboardEvent& k) {
    if (k.scancode == SDL_SCANCODE_LEFTBRACKET) return SDLK_LEFTBRACKET;
    if (k.scancode == SDL_SCANCODE_RIGHTBRACKET) return SDLK_RIGHTBRACKET;
    return k.key;
}

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

// A chord in words, as the manual (docs/manual.md) writes them: "Cmd+Shift+P", "Ctrl+`".
std::string App::chord_text(SDL_Keycode key, SDL_Keymod mod) {
    const bool mac = kPrimaryMod == SDL_KMOD_GUI;
    const u8 m = mods_of(mod);
    std::string s;
    if (!mac && (mod & SDL_KMOD_GUI)) s += "Super+";  // (no command has it: keys looked up in the command search may)
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
    // The side panel's tabs a command shows (the panel's own numbers are one
    // more: its first tab, Files, has a command of its own).
    enum : int { kNone = -1, kProblems, kGit, kProfile, kDebug, kBuild };
    static void show_tab(App& a, int tab, bool toggle);
    static void find_next(App& a, bool forward);
    static bool git_ready(App& a);
};

// A side panel tab's key shows that tab, and pressed again while that tab
// has the keyboard, gives the keyboard back to the text (as Cmd+Shift+E does
// for the Files tab).
void App::Commands::show_tab(App& a, int tab, bool toggle) {
    if (toggle && a.panel_has_keyboard() && a.panel_tab() == static_cast<u32>(tab) + 1) {
        a.panel_focus(false);
        return;
    }
    switch (tab) {
        case kProblems: a.panel_show_problems(); break;
        case kGit: a.panel_show_git(); break;
        case kProfile: a.panel_show_profile(); break;
        case kDebug: a.panel_show_debug(); break;
        default: a.panel_show_build(); break;
    }
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
// and N with modes; without them, what the find box looked for, from the
// selection on, round the ends, and chosen.
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
    a.find_file_step(forward ? 1 : -1);
}

void App::commands_create() {
    commands_ = std::make_shared<Commands>();
    std::vector<Commands::Command>& c = commands_->all;
    // Where Cmd is the editor's own key, Ctrl is free for keys of its own;
    // elsewhere Ctrl is the editor's (a chord written with Ctrl is that
    // key), and the keys of a Mac's Cmd+G go to F3.
    const bool mac = kPrimaryMod == SDL_KMOD_GUI;
    const u8 ctl = mac ? kC : kP;
    auto key = [mac](SDL_Keycode k, u8 m = 0) { return Chord{k, mac || !(m & kC) ? m : static_cast<u8>((m & ~kC) | kP)}; };
    // Where Vim's modes may have a command's Ctrl key (kCtrlShared, and
    // docs/manual.md's "Keys without Cmd"), the keys it has there too.
    auto also = [&](std::vector<Chord> keys, std::initializer_list<Chord> more) {
        if (kCtrlShared) keys.insert(keys.end(), more);
        return keys;
    };
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
    // Files.
    sent("New file", {key(SDLK_N, kP), key(SDLK_T, kP)});
    sent("Open a file\xE2\x80\xA6", also({key(SDLK_O, kP)}, {key(SDLK_O, kA)}));
    own("Open a folder\xE2\x80\xA6", {key(SDLK_O, kP | kS)}, [this] { find_open(kFindFolder); });
    own("New project\xE2\x80\xA6", {key(SDLK_N, kP | kA)}, [this] { project_anywhere(); });
    own("Connect to a server\xE2\x80\xA6", {}, [this] { find_open(kFindFolder, "ssh://"); });
    own("Reconnect to the servers that dropped", {}, [this] { remote_reconnect(); });
    sent("Save", {key(SDLK_S, kP)});
    own("Save as\xE2\x80\xA6", {key(SDLK_S, kP | kS)}, [this] { save_as(); });
    own("Save all", {key(SDLK_S, kP | kA)}, [this] { save_all(); });
    sent("Close the tab", also({key(SDLK_W, kP)}, {key(SDLK_F4, kP)}));
    sent("Open Settings (init.lua)", {key(SDLK_COMMA, kP)});
    own("Choose a theme\xE2\x80\xA6", {}, [this] { theme_choose(); });
    sent("Quit", {key(SDLK_Q, kP)});
    // Finding.
    own("Command search", {key(SDLK_P, kP | kS)}, [this] { palette_open(); });
    sent("Go to file", {key(SDLK_P, kP)});
    sent("Search in files", {key(SDLK_F, kP | kS)});
    own("Find in this file", also({key(SDLK_F, kP)}, {key(SDLK_F, kA)}), [this] { find_in_file(); });
    own("Replace in this file", {key(SDLK_F, kP | kA)}, [this] { find_in_file(true); });
    own("Replace in files", {key(SDLK_H, kP | kS)}, [this] { find_replace_in_files(); });
    // (Their keys work while a find box is open, and are its own then: Option
    // and a letter types a character everywhere else.)
    c.push_back({"Find: regular expressions on or off", {key(SDLK_R, kA)}, false, [this] { find_toggle(true); }});
    c.push_back({"Find: match case on or off", {key(SDLK_C, kA)}, false, [this] { find_toggle(false); }});
    own("Find the next match", {mac ? key(SDLK_G, kP) : key(SDLK_F3)}, [this] { Commands::find_next(*this, true); });
    own("Find the previous match", {mac ? key(SDLK_G, kP | kS) : key(SDLK_F3, kS)}, [this] { Commands::find_next(*this, false); });
    // (Or of the references found, while their list is the last one made.)
    own("Go to the next hit of Search in files", {key(SDLK_F4)}, [this] { if (!lsp_step(1)) find_step(1); });
    own("Go to the previous hit of Search in files", {key(SDLK_F4, kS)}, [this] { if (!lsp_step(-1)) find_step(-1); });
    // Editing.
    sent("Undo", {key(SDLK_Z, kP)});
    sent("Redo", {key(SDLK_Z, kP | kS), key(SDLK_Y, kP)});
    sent("Cut", also({key(SDLK_X, kP)}, {key(SDLK_X, kP | kS)}));
    sent("Copy", also({key(SDLK_C, kP)}, {key(SDLK_C, kP | kS)}));
    sent("Paste", also({key(SDLK_V, kP)}, {key(SDLK_V, kP | kS)}));
    sent("Select all", also({key(SDLK_A, kP)}, {key(SDLK_A, kP | kS)}));
    // The view.
    sent("Show or hide the side panel", also({key(SDLK_B, kP)}, {key(SDLK_B, kA)}));
    sent("Show Files", {key(SDLK_E, kP | kS)});
    tab("Show Problems", {key(SDLK_M, kP | kS)}, Commands::kProblems);
    tab("Show Git", {key(SDLK_G, ctl | kS)}, Commands::kGit);
    tab("Show Profile", {key(SDLK_R, kP | kS)}, Commands::kProfile);
    sent("Show or hide who changed each line (git blame)", {key(SDLK_B, kP | kA)});
    sent("Make the text bigger", {key(SDLK_EQUALS, kP)});
    sent("Make the text smaller", {key(SDLK_MINUS, kP)});
    sent("Reset text size", {key(SDLK_0, kP)});
    // Tabs.
    sent("Go to the next tab", also({key(SDLK_TAB, kC), key(SDLK_RIGHTBRACKET, kP | kS)}, {key(SDLK_PAGEDOWN, kC)}));
    sent("Go to the previous tab", also({key(SDLK_TAB, kC | kS), key(SDLK_LEFTBRACKET, kP | kS)}, {key(SDLK_PAGEUP, kC)}));
    sent("Go to the last tab", {key(SDLK_9, kP)});
    for (int n = 1; n <= 8; ++n) sent("Go to tab " + std::to_string(n), {key(static_cast<SDL_Keycode>(SDLK_0 + n), kP)});
    // (Keys a MacBook has first: it has no Page Up or Page Down.)
    own("Move the tab left", {key(SDLK_LEFTBRACKET, kP | kA | kS), key(SDLK_PAGEUP, kC | kS)}, [this] { tab_shift(-1); });
    own("Move the tab right", {key(SDLK_RIGHTBRACKET, kP | kA | kS), key(SDLK_PAGEDOWN, kC | kS)}, [this] { tab_shift(1); });
    // Splits.
    sent("Split to the right", {key(SDLK_BACKSLASH, kP)});
    sent("Split below", {key(SDLK_BACKSLASH, kP | kS)});
    sent("Close the split", {key(SDLK_W, kP | kS)});
    sent("Go to the split on the left", {key(SDLK_LEFT, kP | kA)});
    sent("Go to the split on the right", {key(SDLK_RIGHT, kP | kA)});
    sent("Go to the split above", {key(SDLK_UP, kP | kA)});
    sent("Go to the split below", {key(SDLK_DOWN, kP | kA)});
    own("Move the tab to the split on the left", {key(SDLK_LEFT, kP | kA | kS)}, [this] { tab_to_split(kEdgeLeft); });
    own("Move the tab to the split on the right", {key(SDLK_RIGHT, kP | kA | kS)}, [this] { tab_to_split(kEdgeRight); });
    own("Move the tab to the split above", {key(SDLK_UP, kP | kA | kS)}, [this] { tab_to_split(kEdgeTop); });
    own("Move the tab to the split below", {key(SDLK_DOWN, kP | kA | kS)}, [this] { tab_to_split(kEdgeBottom); });
    // (Where Ctrl with an arrow moves by words: Alt+Shift with it.)
    const u8 grow = mac ? kP | kC : kA | kS;
    own("Make the split wider", {key(SDLK_RIGHT, grow)}, [this] { split_grow(true, 1); });
    own("Make the split narrower", {key(SDLK_LEFT, grow)}, [this] { split_grow(true, -1); });
    own("Make the split taller", {key(SDLK_DOWN, grow)}, [this] { split_grow(false, 1); });
    own("Make the split shorter", {key(SDLK_UP, grow)}, [this] { split_grow(false, -1); });
    // Code.
    sent("Show completions", {key(SDLK_SPACE, kC)});
    sent("Show the function's parameters", {key(SDLK_SPACE, kC | kS)});
    sent("Show what this is", also({key(SDLK_I, kP)}, {key(SDLK_I, kP | kS)}));
    sent("Go to the definition", {key(SDLK_F12)});
    sent("Find references", {key(SDLK_F12, kS)});
    own("Go to the implementation", {key(SDLK_F12, kP)}, [this] { lsp_places(Lsp::kImplementation); });
    own("Go to the type definition", {key(SDLK_F12, kP | kS)}, [this] { lsp_places(Lsp::kTypeDefinition); });
    own("Peek the definition", {key(SDLK_F12, kA | kS)}, [this] { peek_open(); });
    own("Go to the declaration", {}, [this] { lsp_places(Lsp::kDeclaration); });
    own("Switch between header and source", {key(SDLK_F12, kA)}, [this] { lsp_switch_header(); });
    own("Show the file's outline", {key(SDLK_O, kP | kA)}, [this] { lsp_outline(); });
    own("Go to a symbol in the project", {key(SDLK_P, kP | kA)}, [this] { lsp_find_symbols(); });
    own("Search everywhere", {key(SDLK_K, kP)}, [this] { find_everywhere(); });
    own("Expand the selection", {mac ? key(SDLK_RIGHT, kP | kS | kC) : key(SDLK_RIGHT, kA | kS)}, [this] { select_grow(true); });
    own("Move the lines up", {key(SDLK_UP, kA)}, [this] { lines_move(-1); });
    own("Move the lines down", {key(SDLK_DOWN, kA)}, [this] { lines_move(1); });
    own("Copy the lines down", mac ? std::vector<Chord>{key(SDLK_DOWN, kA | kS)} : std::vector<Chord>{}, [this] { lines_copy(1); });  // (elsewhere Alt+Shift+Down makes the split taller)
    own("Copy the lines up", mac ? std::vector<Chord>{key(SDLK_UP, kA | kS)} : std::vector<Chord>{}, [this] { lines_copy(-1); });
    own("Fold the block at the cursor", {key(SDLK_LEFTBRACKET, kP | kA)}, [this] { fold_close(); });
    own("Unfold the block at the cursor", {key(SDLK_RIGHTBRACKET, kP | kA)}, [this] { fold_open(); });
    own("Fold or unfold the block at the cursor", {}, [this] { fold_toggle(); });
    own("Fold every block", {}, [this] { fold_all(true); });
    own("Unfold every block", {}, [this] { fold_all(false); });
    own("Shrink the selection", {mac ? key(SDLK_LEFT, kP | kS | kC) : key(SDLK_LEFT, kA | kS)}, [this] { select_grow(false); });
    sent("Rename the symbol\xE2\x80\xA6", {key(SDLK_F2)});
    sent("Format the file, or the selected lines", {key(SDLK_F, kA | kS)});
    sent("Fixes and refactorings here (code actions)", {key(SDLK_RETURN, kA)});  // (Cmd+. stops the build)
    own("Show the calls to this function", {}, [this] { hier_show(Lsp::kCallsIn); });
    own("Show the calls from this function", {}, [this] { hier_show(Lsp::kCallsOut); });
    own("Show the supertypes of this type", {}, [this] { hier_show(Lsp::kSupertypes); });
    own("Show the subtypes of this type", {}, [this] { hier_show(Lsp::kSubtypes); });
    sent("Go back", {mac ? key(SDLK_MINUS, kC) : key(SDLK_LEFT, kA)});
    sent("Go forward", {mac ? key(SDLK_MINUS, kC | kS) : key(SDLK_RIGHT, kA)});
    own("Install the grammar for this file's language", {}, [this] { grammar_install(); });
    own("Go to the next problem", {key(SDLK_F8)}, [this] { problem_step(1); });
    own("Go to the previous problem", {key(SDLK_F8, kS)}, [this] { problem_step(-1); });
    // Markdown.
    // (Where Ctrl+Shift+V pastes, as in a terminal: Ctrl+Alt+V.)
    own("Preview beside the file (Markdown, or an SVG's picture)", {key(SDLK_V, kCtrlShared ? kP | kA : kP | kS)}, [this] { preview_open(); });
    own("Toggle soft wrap", {key(SDLK_Z, kA)}, [this] { wrap_toggle(); });
    own("Show the file's local history", {}, [this] { history_show(); });
    own("Show the assembly beside the file", {}, [this] { asm_open(); });
    // Terminals: in the split along the bottom.
    own("Show or hide the terminal", {key(SDLK_GRAVE, ctl)}, [this] { term_toggle(); });
    own("New terminal", {key(SDLK_GRAVE, ctl | kS)}, [this] { term_new(); });
    own("Open the Python console", {}, [this] { py_console(true); });
    own("Send the lines to the Python console", {}, [this] { py_send_lines(); });
    own("Run the cell (# %%) in the Python console", {}, [this] { nb_run_cell(); });
    // Debugging.
    own("Start or continue debugging", {key(SDLK_F5)}, [this] { debug_start(); });
    own("Stop debugging", {key(SDLK_F5, kS)}, [this] { debug_command("stop"); });
    own("Pause the program being debugged", {key(SDLK_F6)}, [this] { debug_command("pause"); });
    own("Add or remove a breakpoint", {key(SDLK_F9)}, [this] { debug_toggle_break(); });
    own("Set a breakpoint's condition and hit count\xE2\x80\xA6", {key(SDLK_F9, kS)}, [this] { debug_edit_break(); });
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
    own("Edit the run configurations", {}, [this] { run_config_edit(); });
    own("Attach the debugger to a process\xE2\x80\xA6", {}, [this] { debug_attach_pick(); });
    own("Debug a core file\xE2\x80\xA6", {}, [this] { debug_core_pick(); });
    own("Break at a function\xE2\x80\xA6", {}, [this] { debug_break_function(); });
    own("Add the next match to the carets", {key(SDLK_D, kP)}, [this] { caret_add_match(false); });
    own("Add carets to every match", {key(SDLK_L, kP | kS)}, [this] { caret_add_match(true); });
    own("Add a caret on the line above", mac ? std::vector<Chord>{key(SDLK_UP, kC | kS)} : std::vector<Chord>{}, [this] { caret_add_line(-1); });
    own("Add a caret on the line below", mac ? std::vector<Chord>{key(SDLK_DOWN, kC | kS)} : std::vector<Chord>{}, [this] { caret_add_line(1); });
    own("Insert a snippet\xE2\x80\xA6", {}, [this] { snippet_pick(); });
    own("Compare with a profile\xE2\x80\xA6", {}, [this] { prof_compare_pick(); });
    own("Leave one caret", {}, [this] {
        pane_->doc->ed.clear_carets();
        dirty_ = true;
    });
    own("Start another debugging session", {}, [this] { debug_start_another(); });
    own("Run to the cursor", {key(SDLK_F10, kC)}, [this] { debug_run_to_cursor(); });
    own("Jump to the cursor's line", {}, [this] { debug_jump_to_cursor(); });
    own("Show the disassembly", {}, [this] { debug_disasm_open(); });
    own("Show memory at\xE2\x80\xA6", {}, [this] { debug_memory_pick(); });
    own("Next page of memory", {}, [this] { debug_memory_page(1); });
    own("Previous page of memory", {}, [this] { debug_memory_page(-1); });
    own("Write to memory at the cursor\xE2\x80\xA6", {}, [this] { debug_memory_write(); });
    own("Run the tests", {}, [this] { test_run({}); });
    own("Run the failed tests again", {}, [this] { test_run_failed(); });
    own("Run the tests with coverage", {}, [this] { test_run({}, true); });
    own("Show or hide coverage", {}, [this] { test_coverage_toggle(); });
    own("Run the test at the cursor", {}, [this] { test_run_here(); });
    own("Debug the test at the cursor", {}, [this] { test_debug_here(); });
    own("Look for the tests again", {}, [this] { test_find(); });
    own("Show Tests", {}, [this] {
        if (!panel_px()) panel_toggle();
        panel_show_tests();
    });
    own("Put the blit command on the PATH", {}, [this] { install_command(); });
    // Profiling.
    own("Record a profile of the Profile tab's program", {}, [this] {
        Commands::show_tab(*this, Commands::kProfile, false);
        prof_record();
    });
    own("Record a memory profile of the Profile tab's program", {}, [this] {
        Commands::show_tab(*this, Commands::kProfile, false);
        prof_set("kind", "memory");
        prof_record();
    });
    own("Record a profile of the Profile tab's program's waiting", {}, [this] {
        Commands::show_tab(*this, Commands::kProfile, false);
        prof_set("kind", "waiting");
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
    git("Git: Stash", "stash");
    git("Git: Pop the latest stash", "pop");
    git_named("Git: Rebase onto branch\xE2\x80\xA6", "Rebase onto branch", "rebase");
    git_named("Git: Cherry-pick a commit\xE2\x80\xA6", "Cherry-pick commit", "pick");
    git("Git: Continue the rebase or cherry-pick", "continue");
    git_named("Git: New tag\xE2\x80\xA6", "New tag", "tag");
    git_named("Git: New worktree\xE2\x80\xA6", "New worktree for branch", "worktree");
    git("Git: Update the submodules", "submodules");
    // (In a diff tab, Enter stages the hunk or the lines chosen, or
    // unstages them; in a file, these take the change the cursor is on.)
    own("Git: Stage the change at the cursor", {}, [this] {
        if (Commands::git_ready(*this)) git_stage_here(true);
    });
    own("Git: Unstage the change at the cursor", {}, [this] {
        if (Commands::git_ready(*this)) git_stage_here(false);
    });
    own("Git: Diffs in two columns or one", {}, [this] { git_view_toggle(); });
    own("Git: Search the history\xE2\x80\xA6", {}, [this] {
        if (!Commands::git_ready(*this)) return;
        find_choose(kFindInput, "Search the history", git_searched(), [this](const std::string& text) {
            Commands::show_tab(*this, Commands::kGit, false);
            git_command("find", text);
        });
    });
    own("Git: Compare the file with\xE2\x80\xA6", {}, [this] {
        if (Commands::git_ready(*this)) git_compare_pick();
    });
    own("Git: History of all branches", {}, [this] {
        if (!Commands::git_ready(*this)) return;
        Commands::show_tab(*this, Commands::kGit, false);
        git_command("allbranches", {});
    });
    own("Git: Rebase interactively onto\xE2\x80\xA6", {}, [this] {
        if (!Commands::git_ready(*this)) return;
        find_choose(kFindInput, "Rebase onto", git_upstream(), [this](const std::string& base) {
            if (!base.empty()) git_rebase_open(base);
        });
    });
    own("Git: Merge the file's conflicts", {}, [this] {
        if (Commands::git_ready(*this) && !pane_->doc->ed.path.empty()) git_merge_open(full_path(pane_->doc->ed.path));
    });
    own("Git: Resolve the simple conflicts", {}, [this] {
        if (Commands::git_ready(*this)) git_merge_take(0, true);
    });
    own("Git: The file's history", {}, [this] {
        if (Commands::git_ready(*this)) git_history(false);
    });
    own("Git: The history of the lines chosen", {}, [this] {
        if (Commands::git_ready(*this)) git_history(true);
    });
}

// Cmd+F: a search in the file in front, starting from the selection (its
// first line) or else the word at the cursor, which is put there selected,
// so that typing replaces it. With modes it is Vim's / with that typed (as
// a pattern that finds it as it stands); without, and for Cmd+Option+F in
// either case, the box that finds (and replaces) in the file.
void App::find_in_file(bool replace) {
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
    if (cfg_.modal && !replace) {
        if (find_open_) find_close(false);
        vim_.open_command(ed, '/', regex_escape(what), true);
        vim_after(Vim::kHandled);
        return;
    }
    find_file_open(replace, what);
}

// The commands' keys this file handles (the others are their owners').
bool App::command_key(const SDL_KeyboardEvent& k) {
    if (!commands_) return false;
    if (git_view_key(k)) return true;  // a diff tab's own keys, and those of what a change's mark offers (appgit.cpp)
    const u8 m = mods_of(k.mod);
    const SDL_Keycode key = key_of(k);
    // (The panel's own keys for another of its tabs: which tab a command
    // showed last no longer says which has the keyboard.)
    for (const Commands::Command& c : commands_->all) {
        if (!c.here) continue;
        for (const Chord& ch : c.keys)
            if (ch.key == key && ch.mods == m) {
                c.run();
                return true;
            }
    }
    return false;
}

bool App::command_key_now(std::string_view title, SDL_KeyboardEvent* out) {
    if (!commands_) return false;
    const Commands::Command* found = nullptr;
    for (int pass = 0; pass < 2 && !found; ++pass)
        for (const Commands::Command& c : commands_->all)
            if (pass == 0 ? c.title == title : std::string_view(c.title).starts_with(title)) {
                found = &c;
                break;
            }
    if (!found || found->keys.empty()) return false;
    auto event = [](const Chord& ch) {
        SDL_KeyboardEvent e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key = ch.key;
        e.mod = keymod_of(ch.mods);
        e.down = true;
        return e;
    };
    *out = event(found->keys[0]);
    for (const Chord& ch : found->keys)
        if (const SDL_KeyboardEvent e = event(ch); !vim_owns_key(e)) {
            *out = e;
            break;
        }
    return true;
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
        command_sending_ = true;  // (the editor's, even where Vim has the key)
        on_key(ev);
        command_sending_ = false;
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
