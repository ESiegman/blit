// The side panel: a strip down the left of the window with a row of tabs at
// the top and, below it, whatever the chosen tab shows. "Files" is a tree of
// the working directory; "Problems" lists what the language server reports.
//
// The panel is one more grid of cells, drawn like a popup. Its cells are
// rebuilt only when something it shows has changed, and the text view is
// simply told to start further right.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "tree.hpp"

namespace blit {

namespace {

const char* const kTabs[] = {"Files", "Problems", "Git", "Profile"};
constexpr u32 kTabFiles = 0, kTabProblems = 1, kTabGit = 2, kTabProfile = 3, kTabCount = 4;
constexpr u32 kHeadRows = 2;  // the tab bar and the line under it that names the working directory

const char* const kMenu[] = {"New File", "New Folder", "Rename", "Move to Trash", "Set as Working Directory",
                             "Go to Parent Folder", "Choose Folder...", "Refresh"};
enum : u32 { kMenuNewFile, kMenuNewFolder, kMenuRename, kMenuTrash, kMenuSetRoot, kMenuParent, kMenuChoose, kMenuRefresh, kMenuCount };

constexpr u32 kNoCell = 0;  // the one cell of a box that is all margin (a line)

}  // namespace

struct App::Side {
    bool open = false, focus = false;
    int cols = 32;       // width, in cells
    u32 tab = kTabFiles;
    int tab_scroll = 0;  // cells the tab bar is scrolled left by
    int tabs_width = 0;  // cells all the tab labels take

    FileTree tree;
    bool rooted = false;   // a working directory has been chosen
    std::string selected;  // path of the selected row; paths, not nodes: nodes come and go
    u32 top = 0;           // first row shown
    u32 body_rows = 0;     // rows the last layout had room for

    // Typing a name, for a new file or folder or a rename.
    enum Input : u8 { kNoInput, kNewFile, kNewFolder, kRename } input = kNoInput;
    std::string input_text;
    std::string input_at;  // the folder the new thing goes in, or the path being renamed

    // The menu a right click opens.
    bool menu = false;
    int menu_x = 0, menu_y = 0;
    u32 menu_sel = 0;
    std::string menu_target;
    TextBox menu_box;
    SDL_Rect menu_rect{};

    u32 problem = 0, problem_top = 0;

    bool resizing = false;
    int hover_row = -1, hover_tab = -1;  // the row and the tab (of the panel's own) the mouse is over

    // The cells as last drawn, and what they were drawn from.
    TextBox box;
    u64 state = 1, drawn_state = 0, drawn_tree = ~0ull, drawn_diag = ~0ull, drawn_git = 0, drawn_prof = 0;
    u32 drawn_font = ~0u, drawn_rows = 0, drawn_cols = 0;
    std::string drawn_file;
    // The open file's full path, worked out when the file changes rather
    // than every frame (asking for the current directory is a system call).
    std::string file_as_given = "\x01", file_full;
    void touch() { ++state; }
};

void App::panel_create() {
    side_ = std::make_shared<Side>();
    side_->tree.set_notify([] {  // on the tree's reader thread: wake the event loop
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        ev.user.code = 6;
        SDL_PushEvent(&ev);
    });
}

// ---------------------------------------------------------------- state

// The panel's width in pixels; 0 while it is closed.
int App::panel_px() const {
    const Side& s = *side_;
    if (!s.open || !font_) return 0;
    const int cw = font_->cell_w();
    return std::min(s.cols * cw + 2 * (cw / 2 + 1), std::max(0, px_w_ - 8 * cw));
}

// The settings decide the width, and at startup whether the panel shows;
// after that Cmd+B does.
void App::panel_apply_config(bool at_start) {
    Side& s = *side_;
    if (s.cols != std::clamp(cfg_.panel_width, 12, 200)) {
        s.cols = std::clamp(cfg_.panel_width, 12, 200);
        s.touch();
        pane_->cache_rev = ~0ull;
        dirty_ = true;
    }
    if (at_start && cfg_.panel_open && !s.open) panel_toggle();
}

// Reads the open folders again: files may have come and gone.
void App::panel_refresh() {
    if (side_->open && side_->rooted) side_->tree.refresh();
}

// The folder a file tree starts from when none was chosen: the project the
// open file belongs to, else the file's own folder, else where blit was
// started. A project is the nearest folder above the file under version
// control, or failing that the nearest one with a build file in it. The
// home folder is never taken for a project (some people keep theirs under
// version control), nor anything above it.
std::string App::default_workdir() const {
    const char* home_env = SDL_getenv("HOME");
    std::string home = home_env ? home_env : "";
    while (home.size() > 1 && home.back() == '/') home.pop_back();
    if (!pane_->doc->ed.path.empty()) {
        const std::string dir = parent_of(full_path(pane_->doc->ed.path));
        if (is_remote(dir)) return dir;  // (another machine's: its own folder, without a round trip for each one above it)
        static const char* const build_files[] = {"CMakeLists.txt", "Makefile", "Cargo.toml", "package.json", "pyproject.toml",
                                                  "go.mod",         "build.zig", "meson.build"};
        std::string by_build_file;
        std::string at = dir;
        for (int up = 0; up < 40 && at.size() > 1 && at != home; ++up) {
            SDL_PathInfo info;
            for (const char* vc : {"/.git", "/.hg", "/.svn"})
                if (SDL_GetPathInfo((at + vc).c_str(), &info)) return at;
            if (by_build_file.empty())
                for (const char* f : build_files)
                    if (SDL_GetPathInfo((at + "/" + f).c_str(), &info)) by_build_file = at;
            at = parent_of(at);
        }
        return by_build_file.empty() ? dir : by_build_file;
    }
    char* cwd = SDL_GetCurrentDirectory();
    std::string out = cwd ? cwd : "";
    SDL_free(cwd);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    if (out.empty() || out == "/") out = home;
    return out;
}

void App::panel_set_workdir(const std::string& path) {
    Side& s = *side_;
    if (path.empty()) return;
    if (script_) script_dir(path);  // (DirChanged, at the next tick)
    s.tree.set_root(path);
    s.rooted = true;
    s.selected.clear();
    s.top = 0;
    s.input = Side::kNoInput;
    s.touch();
    dirty_ = true;
}

void App::panel_toggle() {
    Side& s = *side_;
    s.open = !s.open;
    if (s.open && !s.rooted) panel_set_workdir(default_workdir());
    if (!s.open) s.focus = s.menu = false;
    s.touch();
    pane_->cache_rev = ~0ull;
    scroll_to_cursor();  // the text area changed width
    dirty_ = true;
}

void App::panel_focus(bool on) {
    Side& s = *side_;
    if (on && !s.open) panel_toggle();
    if (s.focus == on) return;
    s.focus = on;
    if (!on) s.input = Side::kNoInput, s.menu = false;
    if (on) lsp_close(kPopAll);
    s.touch();
    dirty_ = true;
}

bool App::panel_has_keyboard() const { return side_->open && side_->focus; }

// The working directory: the folder the Files tab shows. There is none
// until the panel has been opened or a folder chosen.
const std::string* App::panel_root() const { return side_->rooted ? &side_->tree.root() : nullptr; }

// Shows the Git tab and gives it the keyboard.
void App::panel_show_problems() {
    Side& s = *side_;
    s.tab = kTabProblems;
    panel_focus(true);
    s.touch();
    dirty_ = true;
}

void App::panel_show_profile() {
    Side& s = *side_;
    s.tab = kTabProfile;
    panel_focus(true);
    s.touch();
    dirty_ = true;
}

void App::panel_show_git() {
    Side& s = *side_;
    s.tab = kTabGit;
    panel_focus(true);
    git_.refresh();
    s.touch();
    dirty_ = true;
}

// Takes in folders the tree has finished reading.
void App::sync_panel() {
    Side& s = *side_;
    if (s.tree.poll()) dirty_ = dirty_ || s.open;
}

// ---------------------------------------------------------------- the file tree

namespace {

// How far down a list of `count` rows may be scrolled where `room` of them
// fit: until its last row is the bottom one, and not at all if they all fit.
u32 last_top(size_t count, u32 room) { return count > room ? static_cast<u32>(count - room) : 0; }

// Index of the row for a path, or -1.
int row_of(const std::vector<FileTree::Row>& rows, FileTree& tree, const std::string& path) {
    if (path.empty()) return -1;
    const FileTree::Node* node = tree.find(path);
    if (!node) return -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].node == node) return static_cast<int>(i);
    return -1;
}

}  // namespace

void App::panel_select(int row) {
    Side& s = *side_;
    const std::vector<FileTree::Row>& rows = s.tree.rows();
    if (rows.empty()) return;
    row = std::clamp(row, 0, static_cast<int>(rows.size()) - 1);
    s.selected = s.tree.path_of(rows[static_cast<size_t>(row)].node);
    // Keep it in view (once a frame has said how many rows there are).
    if (static_cast<u32>(row) < s.top) s.top = static_cast<u32>(row);
    if (s.body_rows > 0 && static_cast<u32>(row) >= s.top + s.body_rows) s.top = static_cast<u32>(row) - s.body_rows + 1;
    s.touch();
    dirty_ = true;
}

// Opens the file, or opens or closes the folder, at a path in the tree.
void App::panel_activate(const std::string& path) {
    Side& s = *side_;
    FileTree::Node* node = s.tree.find(path);
    if (!node) return;
    if (node->dir) {
        s.tree.toggle(node);
        s.touch();
        dirty_ = true;
        return;
    }
    open_path(path.c_str());  // its tab, or a new one
    panel_focus(false);
}

void App::panel_begin_input(u8 kind) {
    Side& s = *side_;
    FileTree::Node* node = s.tree.find(s.menu ? s.menu_target : s.selected);
    s.menu = false;
    if (kind == Side::kRename) {
        if (!node) return;
        s.input_at = s.tree.path_of(node);
        s.input_text = node->name;
    } else {
        // In the selected folder, or beside the selected file.
        FileTree::Node* folder = node && node->dir ? node : node ? node->parent : nullptr;
        s.input_at = s.tree.path_of(folder);
        if (folder && folder->dir && !folder->open) s.tree.toggle(folder);
        s.input_text.clear();
    }
    s.input = static_cast<Side::Input>(kind);
    s.focus = true;
    s.touch();
    dirty_ = true;
}

void App::panel_commit_input() {
    Side& s = *side_;
    const Side::Input kind = s.input;
    s.input = Side::kNoInput;
    s.touch();
    dirty_ = true;
    std::string err;
    bool ok = true;
    if (kind == Side::kRename) {
        FileTree::Node* node = s.tree.find(s.input_at);
        const std::string to = parent_of(s.input_at) + "/" + s.input_text;
        ok = node && s.tree.rename(node, s.input_text, &err);
        if (ok) {
            renamed_on_disk(s.input_at, to);  // an open file, or a folder above one, may be what was renamed
            s.selected = to;
        }
    } else if (kind != Side::kNoInput) {
        ok = s.tree.create(s.tree.find(s.input_at), s.input_text, kind == Side::kNewFolder, &err);
        if (ok) s.selected = s.input_at + "/" + s.input_text;
    }
    if (!ok && !err.empty()) tell(SDL_MESSAGEBOX_ERROR, err.c_str());
}

void App::panel_trash(const std::string& path) {
    Side& s = *side_;
    FileTree::Node* node = s.tree.find(path);
    if (!node || path == s.tree.root()) return;
    const bool bin = FileTree::has_trash();
    const std::string what = (bin ? "Move \"" : "Delete \"") + node->name + (bin ? "\" to the Trash?" : "\"? This cannot be undone.");
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, bin ? "Move to Trash" : "Delete"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING;
    box.window = win_;
    box.title = "blit";
    box.message = what.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int hit = 0;
#if BLIT_DEV
    if (!win_) hit = 1;  // scripted runs have nobody to ask
    else
#endif
    SDL_ShowMessageBox(&box, &hit);
    if (hit != 1) return;
    std::string err;
    if (!s.tree.trash(node, &err)) tell(SDL_MESSAGEBOX_ERROR, err.c_str());
    s.selected.clear();
    s.touch();
    dirty_ = true;
}

void App::panel_menu_do(u32 item) {
    Side& s = *side_;
    const std::string target = s.menu_target;
    FileTree::Node* node = s.tree.find(target);
    switch (item) {
        case kMenuNewFile: return panel_begin_input(Side::kNewFile);
        case kMenuNewFolder: return panel_begin_input(Side::kNewFolder);
        case kMenuRename: return panel_begin_input(Side::kRename);
        default: break;
    }
    s.menu = false;
    s.touch();
    dirty_ = true;
    switch (item) {
        case kMenuTrash: panel_trash(target); break;
        case kMenuSetRoot:
            if (node) {
                panel_set_workdir(node->dir ? target : parent_of(target));
                recent_add(node->dir ? target : parent_of(target), true);
            }
            break;
        case kMenuParent: panel_set_workdir(parent_of(s.tree.root())); break;
        case kMenuChoose: find_open(kFindFolder); break;  // typed, with the folders listed (appfind.cpp)
        case kMenuRefresh: s.tree.refresh(); break;
        default: break;
    }
}

// ---------------------------------------------------------------- input

// A key press while the panel has the keyboard (or one of the panel's own
// shortcuts). True if it was used.
bool App::panel_key(const SDL_KeyboardEvent& k) {
    Side& s = *side_;
    const bool cmd = k.mod & kPrimaryMod, shift = k.mod & SDL_KMOD_SHIFT;
    if (cmd && !shift && !(k.mod & SDL_KMOD_ALT) && k.key == SDLK_B) {  // show or hide (with Option it is git's blame column)
        panel_toggle();
        return true;
    }
    if (cmd && shift && k.key == SDLK_E) {  // to the file tree, or back to the text
        const bool to_panel = !(s.focus && s.tab == kTabFiles);
        if (to_panel) s.tab = kTabFiles;
        panel_focus(to_panel);
        return true;
    }
    if (!s.open || !s.focus) return false;

    if (s.input != Side::kNoInput) {
        switch (k.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER: panel_commit_input(); return true;
            case SDLK_ESCAPE:
                s.input = Side::kNoInput;
                break;
            case SDLK_BACKSPACE:
                while (!s.input_text.empty() && is_cont(static_cast<u8>(s.input_text.back()))) s.input_text.pop_back();
                if (!s.input_text.empty()) s.input_text.pop_back();
                break;
            default: return !cmd;  // typing goes to the name; shortcuts still work
        }
        s.touch();
        dirty_ = true;
        return true;
    }
    if (s.menu) {
        switch (k.key) {
            case SDLK_UP: s.menu_sel = (s.menu_sel + kMenuCount - 1) % kMenuCount; break;
            case SDLK_DOWN: s.menu_sel = (s.menu_sel + 1) % kMenuCount; break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER: panel_menu_do(s.menu_sel); return true;
            default: s.menu = false; break;
        }
        s.touch();
        dirty_ = true;
        return true;
    }
    if (s.tab == kTabGit && git_panel_key(k)) return true;  // (first: Esc there may mean "not this message")
    if (k.key == SDLK_ESCAPE) {
        panel_focus(false);
        return true;
    }
    if (k.key == SDLK_TAB && (k.mod & SDL_KMOD_CTRL)) {
        s.tab = (s.tab + 1) % kTabCount;
        if (s.tab == kTabGit) git_.refresh();
        s.touch();
        dirty_ = true;
        return true;
    }
    if (s.tab == kTabGit) return !cmd;
    if (s.tab == kTabProfile) return prof_panel_key(k) || !cmd;

    if (s.tab == kTabProblems) {
        const std::vector<Diagnostic>& d = lsp_.diagnostics();
        const u32 n = static_cast<u32>(d.size());
        switch (k.key) {
            case SDLK_UP: s.problem = s.problem ? s.problem - 1 : 0; break;
            case SDLK_DOWN: s.problem = n ? std::min(s.problem + 1, n - 1) : 0; break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (s.problem < n) {
                    pane_->doc->ed.move_to(std::min<u64>(d[s.problem].start, pane_->doc->ed.buf.size()), false);
                    scroll_to_cursor();
                    panel_focus(false);
                }
                return true;
            default: return !cmd;
        }
        s.touch();
        dirty_ = true;
        return true;
    }

    const std::vector<FileTree::Row>& rows = s.tree.rows();
    const int at = row_of(rows, s.tree, s.selected), page = std::max(1, static_cast<int>(s.body_rows) - 1);
    FileTree::Node* node = at >= 0 ? rows[static_cast<size_t>(at)].node : nullptr;
    if (cmd && k.key == SDLK_UP) {  // the folder above the working directory
        s.menu_target = s.tree.root();
        panel_menu_do(kMenuParent);
        return true;
    }
    const int cw = font_->cell_w(), ch = font_->cell_h();
    switch (k.key) {
        case SDLK_UP: panel_select(at < 0 ? 0 : at - 1); return true;
        case SDLK_DOWN: panel_select(at + 1); return true;
        case SDLK_PAGEUP: panel_select(at - page); return true;
        case SDLK_PAGEDOWN: panel_select(std::max(at, 0) + page); return true;
        case SDLK_HOME: panel_select(0); return true;
        case SDLK_END: panel_select(static_cast<int>(rows.size()) - 1); return true;
        case SDLK_RIGHT:
            if (node && node->dir && !node->open) panel_activate(s.selected);
            else if (node && node->dir) panel_select(at + 1);
            return true;
        case SDLK_LEFT:
            if (node && node->dir && node->open) panel_activate(s.selected);
            else if (node && node->parent && !s.tree.path_of(node->parent).empty() && s.tree.path_of(node->parent) != s.tree.root())
                panel_select(row_of(rows, s.tree, s.tree.path_of(node->parent)));
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (node) panel_activate(s.selected);
            return true;
        case SDLK_F2:
            if (node) panel_begin_input(Side::kRename);
            return true;
        case SDLK_N:
            if (cmd) return panel_begin_input(shift ? Side::kNewFolder : Side::kNewFile), true;
            break;
        case SDLK_DELETE:
            if (node) panel_trash(s.selected);
            return true;
        case SDLK_BACKSPACE:
            if (cmd && node) panel_trash(s.selected);
            return true;
        case SDLK_R:
            if (cmd) return s.tree.refresh(), true;
            break;
        case SDLK_F5: s.tree.refresh(); return true;
        case SDLK_F10:
        case SDLK_APPLICATION:  // the menu a right click opens, for the row chosen (Shift+F10, or the Menu key)
            if (k.key == SDLK_F10 && !shift) break;
            {
                const int row = at >= 0 ? at - static_cast<int>(s.top) + static_cast<int>(kHeadRows) : static_cast<int>(kHeadRows);
                s.menu_target = node ? s.selected : s.tree.root();
                s.menu = true;
                s.menu_sel = 0;
                s.menu_x = cw * 4;
                s.menu_y = std::max(0, row + 1) * ch + ch / 4;
                s.touch();
                dirty_ = true;
            }
            return true;
        default: break;
    }
    // Other shortcuts (save, quit, zoom) still reach the editor; plain keys
    // do not type into the text while the panel has the keyboard.
    return !cmd;
}

// Typed text while the panel has the keyboard. True if it was used.
bool App::panel_text(std::string_view text) {
    Side& s = *side_;
    if (!s.open || !s.focus) return false;
    if (s.tab == kTabGit) return git_panel_text(text);
    if (s.input != Side::kNoInput && s.input_text.size() < 255) {
        s.input_text += text;
        s.touch();
        dirty_ = true;
    }
    return true;
}

bool App::panel_mouse_down(const SDL_MouseButtonEvent& b) {
    Side& s = *side_;
    if (!s.open) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    const int cw = font_->cell_w(), ch = font_->cell_h(), width = panel_px();

    if (s.menu) {
        const bool inside = px >= s.menu_rect.x && px < s.menu_rect.x + s.menu_rect.w && py >= s.menu_rect.y &&
                            py < s.menu_rect.y + s.menu_rect.h;
        if (inside && b.button == SDL_BUTTON_LEFT) {
            const int row = (py - s.menu_rect.y - ch / 4) / ch;
            if (row >= 0 && row < static_cast<int>(kMenuCount)) panel_menu_do(static_cast<u32>(row));
            return true;
        }
        s.menu = false;
        s.touch();
        dirty_ = true;
        if (inside) return true;
    }
    // The few pixels either side of the panel's edge drag its width.
    if (b.button == SDL_BUTTON_LEFT && std::abs(px - width) <= std::max(3, static_cast<int>(3 * scale_))) {
        s.resizing = true;
        return true;
    }
    if (px >= width) {
        if (s.focus) panel_focus(false);
        return false;
    }
    if (b.button != SDL_BUTTON_LEFT && b.button != SDL_BUTTON_RIGHT) return true;
    s.focus = true;
    lsp_close(kPopAll);
    s.touch();
    dirty_ = true;
    if (s.input != Side::kNoInput) {
        // A click elsewhere finishes the name being typed. It is not also a
        // click on a row: that line had pushed the rows under it down by one.
        panel_commit_input();
        return true;
    }
    const int row = (py - ch / 4) / ch, col = (px - (cw / 2 + 1)) / cw;

    if (row == 0 && b.button == SDL_BUTTON_LEFT) {  // the tab bar
        int at = -s.tab_scroll;
        for (u32 t = 0; t < kTabCount; ++t) {
            const int w = static_cast<int>(std::strlen(kTabs[t])) + 2;
            if (col >= at && col < at + w) s.tab = t;
            at += w;
        }
        if (s.tab == kTabGit) git_.refresh();
        return true;
    }
    if (s.tab == kTabGit) {
        git_panel_click(row, col, b);
        return true;
    }
    if (s.tab == kTabProfile) {
        prof_panel_click(row, col, b);
        return true;
    }
    if (s.tab == kTabProblems) {
        const u32 i = s.problem_top + static_cast<u32>(std::max(0, row - static_cast<int>(kHeadRows)));
        if (row >= static_cast<int>(kHeadRows) && i < lsp_.diagnostics().size()) {
            s.problem = i;
            pane_->doc->ed.move_to(std::min<u64>(lsp_.diagnostics()[i].start, pane_->doc->ed.buf.size()), false);
            scroll_to_cursor();
        }
        return true;
    }
    const std::vector<FileTree::Row>& rows = s.tree.rows();
    const int index = row - static_cast<int>(kHeadRows) + static_cast<int>(s.top);
    const bool on_row = row >= static_cast<int>(kHeadRows) && index >= 0 && index < static_cast<int>(rows.size());
    if (b.button == SDL_BUTTON_RIGHT) {
        if (on_row) s.selected = s.tree.path_of(rows[static_cast<size_t>(index)].node);
        s.menu_target = on_row ? s.selected : s.tree.root();
        s.menu = true;
        s.menu_sel = 0;
        s.menu_x = px;
        s.menu_y = py;
        return true;
    }
    if (row == 1) {  // the working directory's line: choose another
        s.menu_target = s.tree.root();
        panel_menu_do(kMenuChoose);
        return true;
    }
    if (on_row) {
        const FileTree::Node* node = rows[static_cast<size_t>(index)].node;
        s.selected = s.tree.path_of(node);
        // A folder opens or closes at once. A file is opened when the
        // button comes up, unless by then it has been dragged off to a
        // split (see splits_mouse_up).
        if (node->dir) panel_activate(s.selected);
        else drag_arm(kDragFile, 0, s.selected, b.x, b.y);
    }
    return true;
}

// True while the mouse is the panel's business (resizing, or over it).
bool App::panel_mouse_motion(const SDL_MouseMotionEvent& m) {
    Side& s = *side_;
    if (!s.open) return false;
    const int px = static_cast<int>(m.x * density_), cw = font_->cell_w();
    if (s.resizing) {
        const int cols = std::clamp((px - 2 * (cw / 2 + 1)) / cw, 12, std::max(12, (px_w_ - 12 * cw) / cw));
        if (cols != s.cols) {
            s.cols = cols;
            s.touch();
            pane_->cache_rev = ~0ull;
            dirty_ = true;
        }
        return true;
    }
    if (s.menu && px >= s.menu_rect.x && px < s.menu_rect.x + s.menu_rect.w) {
        const int row = (static_cast<int>(m.y * density_) - s.menu_rect.y - font_->cell_h() / 4) / font_->cell_h();
        if (row >= 0 && row < static_cast<int>(kMenuCount) && static_cast<u32>(row) != s.menu_sel) {
            s.menu_sel = static_cast<u32>(row);
            s.touch();
            dirty_ = true;
        }
        return true;
    }
    return px < panel_px();
}

void App::panel_mouse_up() { side_->resizing = false; }

bool App::panel_wheel(const SDL_MouseWheelEvent& w) {
    Side& s = *side_;
    if (!s.open) return false;
    if (mouse_x_ < 0) return false;
    const int px = static_cast<int>(mouse_x_ * density_), py = static_cast<int>(mouse_y_ * density_);
    if (px >= panel_px()) return false;
    const int row = (py - font_->cell_h() / 4) / font_->cell_h();
    if (row == 0) {
        // Over the tab bar either wheel direction slides the tabs.
        const float d = std::fabs(w.x) > std::fabs(w.y) ? w.x : -w.y;
        s.tab_scroll = std::clamp(s.tab_scroll + static_cast<int>(std::lround(d * 3)), 0, std::max(0, s.tabs_width - s.cols));
    } else if (s.tab == kTabGit) {
        git_panel_wheel(w.y);
    } else if (s.tab == kTabFiles) {
        // (A name being typed for something new is one more row.)
        const size_t rows = s.tree.rows().size() + (s.input == Side::kNewFile || s.input == Side::kNewFolder ? 1 : 0);
        const i64 top = static_cast<i64>(s.top) - static_cast<i64>(std::lround(w.y * 3));
        s.top = static_cast<u32>(std::clamp<i64>(top, 0, last_top(rows, s.body_rows)));
    } else {
        const i64 top = static_cast<i64>(s.problem_top) - static_cast<i64>(std::lround(w.y * 3));
        s.problem_top = static_cast<u32>(std::clamp<i64>(top, 0, last_top(lsp_.diagnostics().size(), s.body_rows)));
    }
    s.touch();
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- drawing

// Adds the panel to a frame. Its cells are made again only when something
// they show has changed.
void App::panel_layout(int w, int h, FrameDesc& fd) {
    Side& s = *side_;
    (void)w;
    if (!s.open || fd.popup_count >= std::size(fd.popups)) return;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    const int pad_x = cw / 2 + 1, pad_y = ch / 4, width = panel_px();
    const u32 cols = static_cast<u32>(std::max(1, (width - 2 * pad_x) / cw)), rows = static_cast<u32>(std::max(1, (h - 2 * pad_y) / ch));
    s.body_rows = rows > kHeadRows ? rows - kHeadRows : 0;

    if (s.file_as_given != pane_->doc->ed.path) {
        s.file_as_given = pane_->doc->ed.path;
        s.file_full = full_path(pane_->doc->ed.path);
    }
    const std::string& open_file = s.file_full;
    const bool stale = s.drawn_state != s.state || s.drawn_tree != s.tree.generation() || s.drawn_font != font_->generation() ||
                       s.drawn_rows != rows || s.drawn_cols != cols || s.drawn_file != open_file ||
                       (s.tab == kTabProblems && s.drawn_diag != lsp_.diagnostics_generation()) || (s.tab == kTabGit && s.drawn_git != git_gen_) ||
                       (s.tab == kTabProfile && s.drawn_prof != prof_gen_);
    if (stale) {
        s.drawn_state = s.state;
        s.drawn_tree = s.tree.generation();
        s.drawn_font = font_->generation();
        s.drawn_rows = rows;
        s.drawn_cols = cols;
        s.drawn_file = open_file;
        s.drawn_diag = lsp_.diagnostics_generation();
        s.drawn_git = git_gen_;
        s.drawn_prof = prof_gen_;
        TextBox& box = s.box;
        box.resize(cols, rows);
        Font& font = *font_;

        // The tabs. The chosen one stands out; when they do not all fit
        // they slide, and a mark at either end says there is more.
        s.tabs_width = 0;
        for (const char* t : kTabs) s.tabs_width += static_cast<int>(std::strlen(t)) + 2;
        s.tab_scroll = std::clamp(s.tab_scroll, 0, std::max(0, s.tabs_width - static_cast<int>(cols)));
        int at = -s.tab_scroll;
        for (u32 t = 0; t < kTabCount; ++t) {
            const int len = static_cast<int>(std::strlen(kTabs[t]));
            if (at + len > 0 && at < static_cast<int>(cols)) {
                const std::string_view label(kTabs[t]);
                const size_t skip = at < 0 ? static_cast<size_t>(-at) : 0;
                box.put(font, 0, static_cast<u32>(std::max(at, 0)), label.substr(std::min(skip, label.size())), t == s.tab ? kInkAccent : kInkMuted,
                        t == s.tab ? Font::kBold : 0);
                // The chosen one on a rounded box, and the one under the mouse on a fainter one.
                if (t == s.tab || static_cast<int>(t) == s.hover_tab)
                    for (int c = std::max(at - 1, 0); c <= at + len && c < static_cast<int>(cols); ++c) box.cells[static_cast<size_t>(c)] |= t == s.tab ? kCellSelected : kCellHover;
            }
            at += len + 2;
        }
        if (s.tab_scroll > 0) box.put(font, 0, 0, "\xE2\x80\xB9", kInkMuted);  // a left-pointing mark
        if (s.tabs_width - s.tab_scroll > static_cast<int>(cols)) box.put(font, 0, cols - 1, "\xE2\x80\xBA", kInkMuted);

        const bool nerd = font.has(0xE5FF) && font.has(0xE61D);  // the font brings file icons of its own
        auto icon = [&](u32 row, u32 col, const FileIcon& ic) {
            if (col + 2 > cols) return;
            u32* cell = box.cells.data() + static_cast<size_t>(row) * cols + col;
            if (nerd) {
                cell[0] = (font.glyph(ic.nerd) & (Font::kWide - 1)) | (ic.ink << kCellInkShift);
            } else {
                const u32 slot = font.icon(ic.drawn);
                cell[0] = slot | (ic.ink << kCellInkShift);
                cell[1] = (slot + 1) | (ic.ink << kCellInkShift);
            }
        };

        if (s.tab == kTabFiles) {
            // The working directory, then its tree.
            const std::string& root = s.tree.root();
            const size_t slash = root.find_last_of('/');
            icon(1, 0, file_icon("", true, true));
            box.put(font, 1, 3, slash == std::string::npos || root.size() == 1 ? root : root.substr(slash + 1), 0, Font::kBold);

            const std::vector<FileTree::Row>& tree_rows = s.tree.rows();
            // A name being typed for something new takes a line of its own,
            // right under the folder it will be in.
            int input_after = -2;  // -1: before the first row
            u32 input_depth = 0;
            if (s.input == Side::kNewFile || s.input == Side::kNewFolder) {
                input_after = row_of(tree_rows, s.tree, s.input_at);
                input_depth = input_after >= 0 ? tree_rows[static_cast<size_t>(input_after)].depth + 1 : 0;
            }
            s.top = std::min(s.top, last_top(tree_rows.size() + (input_after != -2 ? 1 : 0), s.body_rows));
            if (input_after == -1) s.top = 0;
            if (input_after >= 0) {  // keep that line, and the folder above it, in view
                const u32 after = static_cast<u32>(input_after);
                if (s.body_rows > 1 && after + 2 > s.top + s.body_rows) s.top = after + 2 - s.body_rows;
                s.top = std::min(s.top, after);
            }
            const int selected = row_of(tree_rows, s.tree, s.selected);
            auto input_line = [&](u32 r, u32 depth, bool folder) {
                const u32 c0 = depth * 2 + 2;
                icon(r, c0, file_icon(s.input_text, folder, false));
                const u32 end = box.put(font, r, c0 + 3, s.input_text, 0);
                if (end < cols) box.cells[static_cast<size_t>(r) * cols + end] |= kCellSelected;  // where the next letter goes
            };
            u32 r = kHeadRows;
            if (input_after == -1 && s.top == 0 && r < rows) input_line(r++, 0, s.input == Side::kNewFolder);
            // (The rows are told apart by their nodes: no path is put together for each.)
            auto node_of = [&](const std::string& path) -> const FileTree::Node* {
                const FileTree::Node* n = path.empty() ? nullptr : s.tree.find(path);
                return n && s.tree.path_of(n) == path ? n : nullptr;  // (that very path, not one that starts the same)
            };
            const FileTree::Node* const open_node = node_of(open_file);
            const FileTree::Node* const renamed = s.input == Side::kRename ? node_of(s.input_at) : nullptr;
            for (size_t i = s.top; i < tree_rows.size() && r < rows; ++i) {
                const FileTree::Node* n = tree_rows[i].node;
                const u32 depth = tree_rows[i].depth, c0 = depth * 2;
                if (renamed && n == renamed) {
                    input_line(r, depth, n->dir);
                } else {
                    const bool is_open = open_node && n == open_node;
                    if (n->dir) box.put(font, r, c0, n->open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", kInkMuted);  // a small triangle, down or right
                    icon(r, c0 + 2, file_icon(n->name, n->dir, n->open));
                    box.put(font, r, c0 + 5, n->name, is_open ? kInkAccent : 0, is_open ? Font::kBold : 0);
                    if (static_cast<int>(i) == selected) box.select_row(r);
                }
                ++r;
                if (input_after == static_cast<int>(i) && r < rows) input_line(r++, input_depth, s.input == Side::kNewFolder);
            }
            if (tree_rows.empty() && s.input == Side::kNoInput && rows > kHeadRows) box.put(font, kHeadRows, 2, "(empty)", kInkMuted);
        } else if (s.tab == kTabGit) {
            git_panel_draw(box, s.focus);
        } else if (s.tab == kTabProfile) {
            prof_panel_draw(box, s.focus);
        } else {
            // What the language server reports about the open file.
            const std::vector<Diagnostic>& d = lsp_.diagnostics();
            box.put(font, 1, 0, d.empty() ? (lsp_.attached() ? "No problems" : "No language server") : std::to_string(d.size()) + (d.size() == 1 ? " problem" : " problems"),
                    kInkMuted);
            s.problem = d.empty() ? 0 : std::min<u32>(s.problem, static_cast<u32>(d.size()) - 1);
            s.problem_top = std::min(s.problem_top, last_top(d.size(), s.body_rows));
            if (s.focus && !d.empty()) {  // the chosen one stays in view
                if (s.problem < s.problem_top) s.problem_top = s.problem;
                if (s.body_rows > 0 && s.problem >= s.problem_top + s.body_rows) s.problem_top = s.problem - s.body_rows + 1;
            }
            u32 r = kHeadRows;
            for (size_t i = s.problem_top; i < d.size() && r < rows; ++i, ++r) {
                const u32 ink = kInkDiagnostic + d[i].severity - 1;
                static const char* const letter[] = {"E", "W", "I", "H"};
                box.put(font, r, 0, letter[d[i].severity - 1], ink, Font::kBold);
                const u64 line = pane_->doc->ed.buf.line_of(std::min<u64>(d[i].start, pane_->doc->ed.buf.size()));
                const u32 c = box.put(font, r, 2, std::to_string(line + 1), kInkMuted);
                box.put(font, r, c + 1, std::string_view(d[i].message).substr(0, d[i].message.find('\n')), 0);
                if (i == s.problem && s.focus) box.select_row(r);
            }
        }
        // The row under the mouse, if there is something on it and it is not the selected one.
        if (s.hover_row >= static_cast<int>(kHeadRows) && s.hover_row < static_cast<int>(rows)) {
            u32* row = box.cells.data() + static_cast<size_t>(s.hover_row) * cols;
            bool any = false, selected = false;
            for (u32 c = 0; c < cols; ++c) any = any || row[c] != 0, selected = selected || (row[c] & kCellSelected);
            if (any && !selected)
                for (u32 c = 0; c < cols; ++c) row[c] |= kCellHover;
        }
    }

    Popup& p = fd.popups[fd.popup_count++];
    p.x = 0;
    p.y = 0;
    p.w = width;
    p.h = h;
    p.pad_x = pad_x;
    p.pad_y = pad_y;
    p.cols = s.box.cols;
    p.rows = s.box.rows;
    p.cells = s.box.cells.data();
    p.border_px = 0;
    // Lit rows and tabs are rounded bars that reach a little into the
    // margin; the panel casts a soft shadow on what is beside it, and a
    // line marks its edge.
    const Look lk = look();
    const int line = std::max(1, static_cast<int>(std::lround(scale_)));
    if (lk.pill > 0) {
        p.pill = lk.pill;
        p.pill_dx = static_cast<float>(line);
        p.pill_dy = std::round(0.5f * scale_);
        p.pill_out = std::max(0.0f, static_cast<float>(pad_x) - std::round(3 * scale_));
    }
    if (lk.shadow > 0) {
        p.shadow = std::round(lk.shadow * 0.55f);
        p.shadow_dx = std::round(p.shadow * 0.3f);
        p.shadow_alpha = 0.7f;
    }
    if (fd.popup_count < std::size(fd.popups)) {
        Popup& edge = fd.popups[fd.popup_count++];
        edge.x = width - line;
        edge.y = 0;
        edge.w = line;
        edge.h = h;
        edge.cols = edge.rows = 1;
        edge.pad_x = edge.w;  // no cell shows: the box is all margin, in the border's colour
        edge.cells = &kNoCell;
        edge.border_px = 0;
        edge.bg = theme_.popup_border;
    }
}

// The right-click menu goes over everything else, so it is added last.
void App::panel_menu_layout(int w, int h, FrameDesc& fd) {
    Side& s = *side_;
    if (!s.open || !s.menu || fd.popup_count >= std::size(fd.popups)) return;
    const int cw = font_->cell_w(), ch = font_->cell_h(), pad_x = cw / 2 + 1, pad_y = ch / 4;
    u32 widest = 0;
    for (const char* item : kMenu) widest = std::max<u32>(widest, static_cast<u32>(std::strlen(item)));
    s.menu_box.resize(widest + 2, kMenuCount);
    for (u32 i = 0; i < kMenuCount; ++i) {
        const char* label = i == kMenuTrash && !FileTree::has_trash() ? "Delete" : kMenu[i];
        s.menu_box.put(*font_, i, 1, label, 0);
        if (i == s.menu_sel) s.menu_box.select_row(i);
    }
    Popup& p = fd.popups[fd.popup_count++];
    p.cols = s.menu_box.cols;
    p.rows = s.menu_box.rows;
    p.cells = s.menu_box.cells.data();
    p.pad_x = pad_x;
    p.pad_y = pad_y;
    p.w = static_cast<int>(p.cols) * cw + 2 * pad_x;
    p.h = static_cast<int>(p.rows) * ch + 2 * pad_y;
    p.x = std::clamp(s.menu_x, 0, std::max(0, w - p.w));
    p.y = std::clamp(s.menu_y, 0, std::max(0, h - p.h));
    p.border_px = static_cast<float>(std::max(1, static_cast<int>(std::lround(scale_))));
    s.menu_rect = {p.x, p.y, p.w, p.h};
    fx_float(p, kFxMenu);
}

// Notes what of the panel the mouse is over (px < 0: nothing): one of its
// tabs, or a row. True if that changed.
bool App::panel_hover(int px, int py) {
    Side& s = *side_;
    int row = -1, tab = -1;
    if (s.open && px >= 0 && px < panel_px() && !s.menu && !s.resizing && font_) {
        const int cw = font_->cell_w(), ch = font_->cell_h();
        const int r = (py - ch / 4) / ch, col = (px - (cw / 2 + 1)) / cw;
        if (py >= ch / 4 && r == 0) {
            int at = -s.tab_scroll;
            for (u32 t = 0; t < kTabCount; ++t) {
                const int len = static_cast<int>(std::strlen(kTabs[t]));
                if (col >= at - 1 && col <= at + len) tab = static_cast<int>(t);
                at += len + 2;
            }
        } else if (py >= ch / 4 && r >= static_cast<int>(kHeadRows)) {
            row = r;
        }
    }
    if (row == s.hover_row && tab == s.hover_tab) return false;
    s.hover_row = row;
    s.hover_tab = tab;
    s.touch();
    return true;
}

}  // namespace blit
