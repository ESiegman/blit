// The side panel: a strip down the left of the window with a row of tabs at
// the top and, below it, whatever the chosen tab shows. "Files" is a tree of
// the project folder; "Problems" lists what the language server reports.
//
// The panel is one more grid of cells, drawn like a popup. Its cells are
// rebuilt only when something it shows has changed, and the text view is
// simply told to start further right.
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.hpp"
#include "watch.hpp"
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "tree.hpp"

namespace blit {

namespace {

const char* const kTabs[] = {"Files", "Problems", "Git", "Profile", "Debug", "Build", "Tests"};
constexpr u32 kTabFiles = 0, kTabProblems = 1, kTabGit = 2, kTabProfile = 3, kTabDebug = 4, kTabBuild = 5, kTabTests = 6, kTabCount = 7;
// What a tab shrinks to when the names do not all fit: its icon in a Nerd
// Font (a folder, a warning, a branch, a chart, a bug, a wrench, a flask),
// else a mark of its own.
const u32 kTabIcons[] = {0xE5FF, 0xF071, 0xE725, 0xF080, 0xF188, 0xF0AD, 0xF0C3};
const char* const kTabIconText[] = {"\xEE\x97\xBF", "\xEF\x81\xB1", "\xEE\x9C\xA5", "\xEF\x82\x80", "\xEF\x86\x88", "\xEF\x82\xAD", "\xEF\x83\x83"};
const char* const kTabMarks[] = {"F", "!", "G", "P", "D", "B", "T"};
constexpr u32 kHeadRows = 2;  // the tab bar and the line under it that names the project folder

const char* const kMenu[] = {"New file", "New folder", "Rename", "Move to Trash", "Make it the project folder",
                             "Go to the parent folder", "Choose a folder\xE2\x80\xA6", "Refresh"};
const char* const kMenuKeys[] = {"Cmd+N", "Cmd+Shift+N", "F2", kCtrlShared ? "Delete" : "Cmd+Backspace", "", "Cmd+Up", "", "Cmd+R"};
enum : u32 { kMenuNewFile, kMenuNewFolder, kMenuRename, kMenuTrash, kMenuSetRoot, kMenuParent, kMenuChoose, kMenuRefresh, kMenuCount };

constexpr u32 kNoCell = 0;  // the one cell of a box that is all margin (a line)
constexpr u32 kFieldTone = 1;  // the background of a field typed into (Popup::tones; the Git tab's message)

// A row that is picked: lit as selected while the panel has the keyboard,
// in the quieter tone of the mouse's while it does not.
void light_row(TextBox& box, u32 row, bool focused) {
    if (focused) return box.select_row(row);
    if (row >= box.rows) return;
    for (u32 c = 0; c < box.cols; ++c) box.cells[static_cast<size_t>(row) * box.cols + c] |= kCellHover;
}

// The Problems tab's rows under its first line. With problems from the
// build: each file's name and its problems, then (when the language servers
// report any) a blank row and their heading. Then the language servers'
// problems, every file's they have reported on (open or not): each file's
// name and its problems.
struct ProblemRow {
    enum Kind : u8 { kFile, kBuild, kGap, kHead, kLspFile, kDiag } kind;
    u32 index;  // kFile: its first problem; kBuild: the problem; kLspFile: the file; kDiag: the language servers' problem (counted across files)
};

size_t count_problems(const std::vector<ProblemFile>& files) {
    size_t n = 0;
    for (const ProblemFile& f : files) n += f.items.size();
    return n;
}

std::vector<ProblemRow> problem_rows(const std::vector<BuildProblem>& b, const std::vector<ProblemFile>& files) {
    std::vector<ProblemRow> rows;
    for (u32 i = 0; i < b.size(); ++i) {
        if (i == 0 || b[i].group != b[i - 1].group) rows.push_back({ProblemRow::kFile, i});
        rows.push_back({ProblemRow::kBuild, i});
    }
    if (!b.empty() && count_problems(files)) {
        rows.push_back({ProblemRow::kGap, 0});
        rows.push_back({ProblemRow::kHead, 0});
    }
    u32 n = 0;
    for (u32 f = 0; f < files.size(); ++f) {
        if (files[f].items.empty()) continue;
        rows.push_back({ProblemRow::kLspFile, f});
        for (size_t i = 0; i < files[f].items.size(); ++i) rows.push_back({ProblemRow::kDiag, n++});
    }
    return rows;
}

// The language servers' problem i (counted across files), and its file.
const ProblemFile::Item* lsp_problem(const std::vector<ProblemFile>& files, size_t i, const ProblemFile** file) {
    for (const ProblemFile& f : files) {
        if (i < f.items.size()) {
            *file = &f;
            return &f.items[i];
        }
        i -= f.items.size();
    }
    return nullptr;
}

// A tab whose text a language server could have (not a terminal, a
// profile, a preview, a picture or a PDF).
bool server_text_doc(const Doc& d) { return !d.read_only && !d.term && !d.prof && !d.md && !d.image && !d.pdf; }

// The row of a problem (numbered the build's first, then the language server's).
u32 row_of_problem(const std::vector<ProblemRow>& rows, u32 problem, size_t builds) {
    for (u32 r = 0; r < rows.size(); ++r)
        if ((rows[r].kind == ProblemRow::kBuild && rows[r].index == problem) || (rows[r].kind == ProblemRow::kDiag && rows[r].index + builds == problem)) return r;
    return 0;
}

// A build problem's file as the tab names it: from the project folder, or from ~.
std::string shown_path(const std::string& path, const std::string* root) {
    if (root && path.size() > root->size() + 1 && path.starts_with(*root) && path[root->size()] == '/') return path.substr(root->size() + 1);
    const char* home = SDL_getenv("HOME");
    const size_t h = home ? std::strlen(home) : 0;
    if (h > 1 && path.size() > h && path.starts_with(home) && path[h] == '/') return "~" + path.substr(h);
    return path;
}

}  // namespace

struct App::Side {
    bool open = false, focus = false;
    int cols = 32;       // width, in cells
    u32 tab = kTabFiles;
    int tab_scroll = 0;  // cells the tab bar is scrolled left by
    int tabs_width = 0;  // cells all the tab labels take
    // Where each tab's label starts and how wide it is, as last drawn (cells,
    // before the scroll); and the tab and width the bar was last scrolled
    // for, so that the tab shown is brought into view when either changes.
    int tab_x[kTabCount] = {}, tab_w[kTabCount] = {};
    u32 tab_seen = ~0u, tab_cols = 0;
    bool g_pending = false;  // a g was pressed: another goes to the top of the list

    FileTree tree;
    bool rooted = false;   // a working directory has been chosen
    std::string selected;  // path of the selected row; paths, not nodes: nodes come and go
    std::vector<std::string> chosen;  // more than one chosen (Cmd+click, Shift+click): their paths, the selected one among them
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
    u64 menu_drawn = ~0ull;  // what the menu's cells were made for (the row chosen, the font)
    SDL_Rect menu_rect{};

    u32 problem = 0, problem_top = 0;  // the problem picked (the build's first, then the language server's), the first row shown
    bool problem_show = false;         // the one picked is brought into view at the next drawing

    bool resizing = false;
    int hover_row = -1, hover_tab = -1;  // the row and the tab (of the panel's own) the mouse is over

    // The cells as last drawn, and what they were drawn from.
    TextBox box;
    float tones[8][4] = {};  // Popup::tones: [kFieldTone] the fields'
    u64 state = 1, drawn_state = 0, drawn_tree = ~0ull, drawn_diag = ~0ull, drawn_git = 0, drawn_prof = 0, drawn_debug = 0, drawn_build = 0, drawn_bprob = 0, drawn_tests = 0;
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
    if (!s.open || !font_ || focus_mode_) return 0;
    const int cw = font_->cell_w();
    return std::min(s.cols * cw + 2 * look().edge, std::max(0, px_w_ - 8 * cw));
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
        // (Asked every frame while no folder is open: the folders above are
        // looked at again for the same file once two seconds have passed.)
        const u64 now = now_ns();
        if (dir == workdir_for_ && now - workdir_ns_ < 2'000'000'000ull) return workdir_found_;
        workdir_for_ = dir;
        workdir_ns_ = now;
        workdir_found_ = [&] {
        static const char* const build_files[] = {"CMakeLists.txt", "meson.build", "Makefile", "configure.ac", "pyproject.toml", "setup.py"};
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
        }();
        return workdir_found_;
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

// (Distraction-free and presentation modes draw no panel: asked for, it
// brings back what they hide, and is shown, open as it was or opened.)
void App::panel_toggle() {
    Side& s = *side_;
    if (focus_mode_ != kFocusNone) {
        focus_toggle(focus_mode_);
        if (s.open) return;
    }
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
    if (on && (!s.open || focus_mode_ != kFocusNone)) panel_toggle();
    if (s.focus == on) return;
    s.focus = on;
    if (!on) s.input = Side::kNoInput, s.menu = false;
    if (on) lsp_close(kPopAll);
    s.touch();
    dirty_ = true;
}

bool App::panel_has_keyboard() const { return side_->open && side_->focus; }
static_assert(kTabFiles == 0 && kTabProblems == 1 && kTabGit == 2 && kTabProfile == 3 && kTabDebug == 4 && kTabBuild == 5 && kTabTests == 6, "the order app.hpp gives for panel_tab()");
u32 App::panel_tab() const { return side_->tab < kTabCount ? side_->tab : kTabFiles; }

// The working directory: the folder the Files tab shows. There is none
// until the panel has been opened or a folder chosen.
const std::string* App::panel_root() const { return side_->rooted ? &side_->tree.root() : nullptr; }

// Shows the Problems tab and gives it the keyboard.
void App::panel_show_problems() {
    Side& s = *side_;
    s.tab = kTabProblems;
    panel_focus(true);
    s.touch();
    dirty_ = true;
}

// The Problems tab's row for a problem gone to (by F8, or from the tab):
// picked, and brought into view.
void App::panel_problem_seen(size_t i) {
    Side& s = *side_;
    s.problem = static_cast<u32>(i);
    s.problem_show = true;
    s.touch();
    dirty_ = true;
}

// F8 (Shift+F8, back): the next of the build's problems, across files;
// with none, the next of the language servers', from the cursor on, across
// files (in the order the Problems tab has them), round the ends.
void App::problem_step(int by) {
    if (build_step(by)) return;
    const std::vector<ProblemFile>& files = lsp_.problems();
    const size_t n = count_problems(files);
    if (n == 0) {
        notify("No problems");
        return;
    }
    // Where the cursor is, among them: the file in front's place in the
    // list, and the line and byte in it.
    const Doc& doc = *pane_->doc;
    const Buffer& buf = doc.ed.buf;
    const u64 cur = doc.ed.cursor(), cur_line = buf.line_of(cur), cur_col = cur - buf.line_start(cur_line);
    const std::string here = doc.ed.path.empty() || is_remote(doc.ed.path) ? doc.ed.path : full_path(doc.ed.path);
    size_t pick = n, at = 0;
    bool found_file = false;
    for (size_t f = 0; f < files.size() && pick == n; ++f) {
        const ProblemFile& pf = files[f];
        const bool this_one = pf.key == doc.id || (!pf.key && pf.path == here);
        if (this_one) {
            found_file = true;
            if (by > 0) {
                for (size_t i = 0; i < pf.items.size() && pick == n; ++i)
                    if (pf.items[i].line > cur_line || (pf.items[i].line == cur_line && pf.items[i].col > cur_col)) pick = at + i;
                if (pick == n) pick = (at + pf.items.size()) % n;  // (the next file's first)
            } else {
                for (size_t i = pf.items.size(); i-- > 0 && pick == n;)
                    if (pf.items[i].line < cur_line || (pf.items[i].line == cur_line && pf.items[i].col < cur_col)) pick = at + i;
                if (pick == n) pick = (at + n - 1) % n;  // (the file before's last)
            }
        }
        at += pf.items.size();
    }
    if (!found_file) pick = by > 0 ? 0 : n - 1;
    lsp_go_problem(pick);
    panel_problem_seen(build_problems().size() + pick);
    const ProblemFile* file = nullptr;
    if (const ProblemFile::Item* it = lsp_problem(files, pick, &file))
        notify("Problem " + std::to_string(pick + 1) + " of " + std::to_string(n) + ": " + it->message.substr(0, it->message.find('\n')));
}

// The language servers' problem i, counted across files: its file comes to
// the front (opened, if it is not), at its line.
void App::lsp_go_problem(size_t i) {
    const std::vector<ProblemFile>& files = lsp_.problems();
    const ProblemFile* file = nullptr;
    const ProblemFile::Item* it = lsp_problem(files, i, &file);
    if (!it) return;
    const std::string path = file->path;
    const u64 key = file->key;
    const u32 line = it->line, col = it->col;
    Pane* where = nullptr;
    size_t at = 0;
    if (key && find_doc(key, &where, &at)) {
        if (pane_->doc->id != key) {
            remember_place();
            focus_pane(where);
            tab_show(at);
        }
        show_place(line, col);
    } else {
        go_to_place(path, line + 1, col + 1);
    }
}

// (Tests.) The Run button's words, and the Problems tab's rows as text: a
// file's name, its problems ("E 3 message"), the one picked marked ">".
std::string App::problems_dump() {
    term_sync();
    const std::vector<BuildProblem>& bp = build_problems();
    const std::vector<ProblemFile>& files = lsp_.problems();
    const size_t n = count_problems(files);
    std::string out = "button: " + build_label() + " | " + (bp.empty() ? "file " + std::to_string(n) : "Build " + std::to_string(bp.size()));
    const Side& s = *side_;
    for (const ProblemRow& row : problem_rows(bp, files)) {
        out += " | ";
        if (row.kind == ProblemRow::kFile) out += "file " + shown_path(bp[row.index].path, panel_root());
        else if (row.kind == ProblemRow::kHead) out += "Problems " + std::to_string(n);
        else if (row.kind == ProblemRow::kLspFile) out += "file " + shown_path(files[row.index].path, panel_root()) + (files[row.index].key ? "" : " (not open)");
        else if (row.kind == ProblemRow::kBuild) {
            const BuildProblem& p = bp[row.index];
            out += std::string(row.index == s.problem ? ">" : "") + (p.severity == 1 ? "E " : "W ") + std::to_string(p.line) + (p.col ? ":" + std::to_string(p.col) : "") + " " + p.message;
        } else if (row.kind == ProblemRow::kDiag) {
            const ProblemFile* file = nullptr;
            const ProblemFile::Item* it = lsp_problem(files, row.index, &file);
            static const char* const letter[] = {"E ", "W ", "I ", "H "};
            out += std::string(row.index + bp.size() == s.problem ? ">" : "") + letter[(it->severity - 1) & 3] + std::to_string(it->line + 1) + " " +
                   it->message.substr(0, it->message.find('\n'));
        }
    }
    return out;
}

void App::panel_show_debug() {
    Side& s = *side_;
    s.tab = kTabDebug;
    panel_focus(true);
    s.touch();
    dirty_ = true;
}

void App::panel_show_tests() {
    Side& s = *side_;
    s.tab = kTabTests;
    panel_focus(true);
    s.touch();
    dirty_ = true;
}

void App::panel_show_build() {
    Side& s = *side_;
    s.tab = kTabBuild;
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

// Whether the panel shows, its tab (by name: a session outlives the order
// of the tabs) and its width in cells, as a session keeps them; and the
// same brought back. It comes back without the keyboard.
void App::panel_session(bool* open, std::string* tab, int* cols) const {
    const Side& s = *side_;
    *open = s.open;
    *tab = kTabs[s.tab < kTabCount ? s.tab : kTabFiles];
    *cols = s.cols;
}

void App::panel_session_set(bool open, const std::string& tab, int cols) {
    Side& s = *side_;
    for (u32 i = 0; i < kTabCount; ++i)
        if (tab == kTabs[i]) s.tab = i;
    if (cols > 0) s.cols = std::clamp(cols, 12, 200);
    s.focus = s.menu = false;
    if (open && !s.rooted) panel_set_workdir(default_workdir());
    s.open = open;  // (not panel_toggle(): the views stay where the session put them)
    if (s.tab == kTabGit) git_.refresh();
    s.touch();
    pane_->cache_rev = ~0ull;
    dirty_ = true;
}

// Takes in folders the tree has finished reading, and what changes made to
// another machine's files came to: one that failed says why on the bottom
// line; a file or folder renamed there takes its open files' tabs with it.
void App::sync_panel() {
    Side& s = *side_;
    if (s.tree.poll()) {
        dirty_ = dirty_ || s.open;
        find_stale();  // (a folder read: it may have news)
        watch_follow();  // (a folder read: watched too)
    }
    for (FileTree::Change& c : s.tree.take_changes()) {
        if (!c.error.empty()) {
            notify(c.error);
        } else if (c.kind == FileTree::Change::kRenamed) {
            renamed_on_disk(c.path, c.to);
            if (s.selected == c.path) s.selected = c.to;
        } else if (c.kind == FileTree::Change::kTrashed && s.selected == c.path) {
            s.selected.clear();
        }
        s.touch();
        dirty_ = true;
    }
}

bool App::panel_busy() const { return side_->tree.busy(); }

// ---------------------------------------------------------------- the disk watched

// The working folder watched (on this machine), and each folder the tree
// has read, for what changes in them.
void App::watch_follow() {
    Side& s = *side_;
    if (!watch_) {
        watch_ = std::make_unique<DirWatch>();
        watch_->set_notify([] {
            SDL_Event ev{};
            ev.type = SDL_EVENT_USER;
            ev.user.code = 25;
            SDL_PushEvent(&ev);
        });
    }
    const std::string root = s.rooted && !is_remote(s.tree.root()) && cfg_.watch ? s.tree.root() : std::string();
    if (root != watch_->root()) {
        watch_->watch(root);
        // (The system names folders by their real paths: /private/tmp for /tmp.)
        char real[PATH_MAX];
        watch_real_ = !root.empty() && ::realpath(root.c_str(), real) ? real : root;
    }
    if (root.empty()) return;
    for (const FileTree::Row& r : s.tree.rows())
        if (r.node->dir && r.node->open) watch_->add(s.tree.path_of(r.node));
    watch_->add(root);
}

namespace {

// Git's own folder (.git under the top folder), or a folder in it: not one
// whose name only starts so (.github), nor a .git above the top folder.
bool in_git_dir(std::string_view dir, std::string_view root) {
    if (root.size() > 1 && dir.starts_with(root)) dir.remove_prefix(root.size());
    for (size_t at = dir.find("/.git"); at != std::string_view::npos; at = dir.find("/.git", at + 1))
        if (at + 5 == dir.size() || dir[at + 5] == '/') return true;
    return false;
}

}  // namespace

// Something changed on disk: the folders it changed in read again (those
// the tree has read); the open files and git looked at again a moment
// later, once it has settled.
void App::watch_take() {
    if (!watch_) return;
    Side& s = *side_;
    const std::string root = s.tree.root();
    const std::vector<std::string> heard = watch_->take();
    if (heard.empty()) return;
    bool files = false;
    for (std::string dir : heard) {
        if (!watch_real_.empty() && watch_real_ != root && dir.starts_with(watch_real_)) dir = root + dir.substr(watch_real_.size());
        if (in_git_dir(dir, root)) {  // (git's own: what it says may have changed)
            files = true;
            continue;
        }
        files = true;
        FileTree::Node* node = s.tree.find(dir);
        if (node && node->dir && node->loaded) s.tree.refresh(node);
    }
    if (files && !watch_due_ns_) watch_due_ns_ = now_ns() + 300'000'000ull;
    ++watch_events_;
    dirty_ = true;
}

void App::watch_tick() {
    if (!watch_due_ns_ || now_ns() < watch_due_ns_) return;
    watch_due_ns_ = 0;
    reload_changed();  // (an open file another program wrote)
    if (cfg_.git) git_.refresh();
}

// For the tests: the tree's rows, each a path under the top folder (a
// folder's ending in /) and where its middle is in the window; "*" chosen,
// ">" selected.
std::string App::tree_dump() {
    Side& s = *side_;
    std::string out = "tree:";
    const std::string root = s.tree.root();
    const int ch = font_->cell_h();
    const std::vector<FileTree::Row>& rows = s.tree.rows();
    for (size_t i = s.top; i < rows.size(); ++i) {
        const std::string path = s.tree.path_of(rows[i].node);
        std::string name = path.starts_with(root + "/") ? path.substr(root.size() + 1) : path;
        if (rows[i].node->dir) name += "/";
        const int y = look().pad_y + static_cast<int>(kHeadRows + i - s.top) * ch + ch / 2;
        out += " ";
        if (path == s.selected) out += ">";
        if (std::ranges::find(s.chosen, path) != s.chosen.end()) out += "*";
        out += name + "@" + std::to_string(static_cast<int>(y / density_));
    }
    return out;
}

int App::watch_wait_ms() const {
    if (!watch_due_ns_) return -1;
    const u64 now = now_ns();
    return watch_due_ns_ <= now ? 0 : static_cast<int>((watch_due_ns_ - now) / 1'000'000 + 1);
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
        if (ok && !is_remote(s.input_at)) {  // (another machine's is renamed on the tree's thread: see sync_panel)
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
    // Several chosen (the one asked about among them): all of them.
    std::vector<std::string> all = std::ranges::find(s.chosen, path) != s.chosen.end() && s.chosen.size() > 1 ? s.chosen : std::vector<std::string>{path};
    const std::string who = all.size() > 1 ? std::to_string(all.size()) + " items" : "\"" + node->name + "\"";
    const std::string what = (bin ? "Move " : "Delete ") + who + (bin ? " to the Trash?" : "? This cannot be undone.");
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
    for (const std::string& p : all) {
        // (One in a folder chosen with it goes with that folder, where it is in it.)
        if (std::ranges::any_of(all, [&](const std::string& other) { return p.starts_with(other + "/"); })) continue;
        FileTree::Node* n = s.tree.find(p);
        if (n && p != s.tree.root() && !s.tree.trash(n, &err)) {
            tell(SDL_MESSAGEBOX_ERROR, err.c_str());
            break;
        }
    }
    s.chosen.clear();
    if (!is_remote(path)) s.selected.clear();  // (another machine's: once it is in the Trash there, see sync_panel)
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
    // Show or hide (with Option too it is git's blame column). Where Vim may
    // have Ctrl+B (kCtrlShared), Alt+B as well.
    const bool alt_only = (k.mod & SDL_KMOD_ALT) && !(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI));
    if (k.key == SDLK_B && !shift && ((cmd && !(k.mod & SDL_KMOD_ALT)) || (kCtrlShared && alt_only))) {
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
    if (s.tab == kTabGit && git_panel_key(k)) return true;  // (first: Esc there may mean "not this message", and letters are the message's)
    if (k.key == SDLK_ESCAPE) {
        panel_focus(false);
        return true;
    }
    if (k.key == SDLK_TAB && (k.mod & SDL_KMOD_CTRL)) {  // the panel's next tab, or with Shift the one before
        s.tab = (s.tab + (shift ? kTabCount - 1 : 1)) % kTabCount;
        if (s.tab == kTabGit) git_.refresh();
        s.touch();
        dirty_ = true;
        return true;
    }
    // Every list here also goes down and up with j and k, and to its top
    // and bottom with gg and G, as with Down, Up, Home and End. (Not while a
    // name or a message is typed: those took the keys above.)
    if (!(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT))) {
        const bool g_before = s.g_pending;
        s.g_pending = false;
        SDL_KeyboardEvent to = k;
        to.mod = 0;
        switch (k.key) {
            case SDLK_J: to.key = SDLK_DOWN; return panel_key(to);
            case SDLK_K: to.key = SDLK_UP; return panel_key(to);
            case SDLK_G:
                if (!shift && !g_before) {
                    s.g_pending = true;
                    return true;
                }
                to.key = shift ? SDLK_END : SDLK_HOME;
                return panel_key(to);
            default: break;
        }
    }
    if (s.tab == kTabGit) return !cmd;
    if (s.tab == kTabProfile) return prof_panel_key(k) || !cmd;
    if (s.tab == kTabDebug) return debug_panel_key(k) || !cmd;
    if (s.tab == kTabBuild) return build_panel_key(k) || !cmd;
    if (s.tab == kTabTests) return test_panel_key(k) || !cmd;

    if (s.tab == kTabProblems) {
        const u32 nb = static_cast<u32>(build_problems().size()), n = nb + static_cast<u32>(count_problems(lsp_.problems()));
        switch (k.key) {
            case SDLK_UP: s.problem = s.problem ? s.problem - 1 : 0; break;
            case SDLK_DOWN: s.problem = n ? std::min(s.problem + 1, n - 1) : 0; break;
            case SDLK_HOME: s.problem = 0; break;
            case SDLK_END: s.problem = n ? n - 1 : 0; break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (s.problem < nb) {
                    build_go(s.problem);
                    panel_focus(false);
                } else if (s.problem < n) {
                    lsp_go_problem(s.problem - nb);
                    panel_focus(false);
                }
                return true;
            default: return !cmd;
        }
        s.problem_show = true;
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
        case SDLK_ESCAPE:
            if (!s.chosen.empty()) {  // (what was chosen let go of, the selected row kept)
                s.chosen.clear();
                s.touch();
                dirty_ = true;
                return true;
            }
            break;
        case SDLK_R:
            if (cmd) return s.tree.refresh(), true;
            break;
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
    const Look l = look();

    if (s.menu) {
        const bool inside = px >= s.menu_rect.x && px < s.menu_rect.x + s.menu_rect.w && py >= s.menu_rect.y &&
                            py < s.menu_rect.y + s.menu_rect.h;
        if (inside && b.button == SDL_BUTTON_LEFT) {
            const int row = (py - s.menu_rect.y - l.pad_y) / ch;
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
    const int row = (py - l.pad_y) / ch, col = static_cast<int>(std::floor(static_cast<double>(px - l.edge) / cw));

    if (row == 0 && b.button == SDL_BUTTON_LEFT) {  // the tab bar: a tab is as wide as its rounded bar
        const int at = col + s.tab_scroll;
        for (u32 t = 0; t < kTabCount; ++t)
            if (at >= s.tab_x[t] - 1 && at <= s.tab_x[t] + s.tab_w[t]) s.tab = t;
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
    if (s.tab == kTabDebug) {
        debug_panel_click(row, col, b);
        return true;
    }
    if (s.tab == kTabBuild) {
        build_panel_click(row, col, b);
        return true;
    }
    if (s.tab == kTabTests) {
        test_panel_click(row, col, b);
        return true;
    }
    if (s.tab == kTabProblems) {
        const std::vector<BuildProblem>& bp = build_problems();
        const std::vector<ProblemFile>& files = lsp_.problems();
        const std::vector<ProblemRow> pr = problem_rows(bp, files);
        const u32 i = s.problem_top + static_cast<u32>(std::max(0, row - static_cast<int>(kHeadRows)));
        if (row >= static_cast<int>(kHeadRows) && i < pr.size()) {
            ProblemRow r = pr[i];
            if (r.kind == ProblemRow::kLspFile && i + 1 < pr.size()) r = pr[i + 1];  // (a file's row: its first)
            if (r.kind == ProblemRow::kFile || r.kind == ProblemRow::kBuild) {  // (a file's row: its first)
                build_go(r.index);
            } else if (r.kind == ProblemRow::kDiag) {
                s.problem = static_cast<u32>(bp.size()) + r.index;
                lsp_go_problem(r.index);
            }
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
        const std::string path = s.tree.path_of(node);
        const SDL_Keymod mods = SDL_GetModState();
        // Cmd+click: one more chosen (or one fewer); Shift+click: the rows
        // from the one selected to this one.
        if (mods & kPrimaryMod) {
            if (s.chosen.empty() && !s.selected.empty()) s.chosen.push_back(s.selected);
            if (const auto it = std::ranges::find(s.chosen, path); it == s.chosen.end()) {
                s.chosen.push_back(path);
                s.selected = path;
            } else {  // (one fewer: the last of the others selected; one left, just it)
                s.chosen.erase(it);
                if (!s.chosen.empty()) s.selected = s.chosen.back();
                if (s.chosen.size() == 1) s.chosen.clear();
            }
            return true;
        }
        if ((mods & SDL_KMOD_SHIFT) && !s.selected.empty()) {
            const int from = row_of(rows, s.tree, s.selected);
            if (from >= 0) {
                s.chosen.clear();
                for (int k = std::min(from, index); k <= std::max(from, index); ++k) s.chosen.push_back(s.tree.path_of(rows[static_cast<size_t>(k)].node));
                s.selected = path;
                return true;
            }
        }
        if (std::ranges::find(s.chosen, path) == s.chosen.end()) s.chosen.clear();  // (a plain click on one of those chosen keeps them, to drag)
        s.selected = path;
        // A file is opened, a folder opened or closed, when the button
        // comes up, unless by then it has been dragged: off to a split (a
        // file), or onto a folder of the tree (moved there: see panel_drop).
        drag_arm(kDragFile, 0, s.selected, b.x, b.y);
    }
    return true;
}

// What was dragged from the tree (and the others chosen with it), let go
// over the tree: moved into the folder it was let go on (a file's: its
// folder). False if it was not let go over a row of the tree.
bool App::panel_drop(const std::string& path, float x, float y) {
    Side& s = *side_;
    if (!s.open || s.tab != kTabFiles) return false;
    const int px = static_cast<int>(x * density_), py = static_cast<int>(y * density_);
    if (px >= panel_px()) return false;
    const std::vector<FileTree::Row>& rows = s.tree.rows();
    const int row = (py - look().pad_y) / font_->cell_h();
    const int index = row - static_cast<int>(kHeadRows) + static_cast<int>(s.top);
    FileTree::Node* into = nullptr;
    if (row >= static_cast<int>(kHeadRows) && index >= 0 && index < static_cast<int>(rows.size())) {
        FileTree::Node* n = rows[static_cast<size_t>(index)].node;
        into = n->dir ? n : n->parent;
    } else if (row >= static_cast<int>(kHeadRows)) {
        into = s.tree.find(s.tree.root());  // (below the last row: the top folder)
    }
    if (!into) return true;
    std::vector<std::string> moving = std::ranges::find(s.chosen, path) != s.chosen.end() ? s.chosen : std::vector<std::string>{path};
    const std::string dest = s.tree.path_of(into);
    std::string err;
    for (const std::string& from : moving) {
        FileTree::Node* node = s.tree.find(from);
        if (!node || node == into || parent_of(from) == dest) continue;
        // (One in a folder chosen with it goes with that folder, where it is in it.)
        if (std::ranges::any_of(moving, [&](const std::string& other) { return from.starts_with(other + "/"); })) continue;
        if (!s.tree.move(node, into, &err)) break;
        const std::string to = dest + "/" + from.substr(from.find_last_of('/') + 1);
        renamed_on_disk(from, to);  // (an open file, or one in a folder moved, follows it)
        if (s.selected == from) s.selected = to;
    }
    s.chosen.clear();
    if (!err.empty()) tell(SDL_MESSAGEBOX_ERROR, err.c_str());
    s.touch();
    dirty_ = true;
    return true;
}

// True while the mouse is the panel's business (resizing, or over it).
bool App::panel_mouse_motion(const SDL_MouseMotionEvent& m) {
    Side& s = *side_;
    if (!s.open) return false;
    const int px = static_cast<int>(m.x * density_), cw = font_->cell_w();
    if (s.resizing && !(m.state & SDL_BUTTON_LMASK)) s.resizing = false;  // (the button went up where this window did not see it)
    if (s.resizing) {
        const int cols = std::clamp((px - 2 * look().edge) / cw, 12, std::max(12, (px_w_ - 12 * cw) / cw));
        if (cols != s.cols) {
            s.cols = cols;
            s.touch();
            pane_->cache_rev = ~0ull;
            dirty_ = true;
        }
        return true;
    }
    if (s.menu && px >= s.menu_rect.x && px < s.menu_rect.x + s.menu_rect.w) {
        const int row = (static_cast<int>(m.y * density_) - s.menu_rect.y - look().pad_y) / font_->cell_h();
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
    const int row = (py - look().pad_y) / font_->cell_h();
    if (row == 0) {
        // Over the tab bar either wheel direction slides the tabs.
        const float d = std::fabs(w.x) > std::fabs(w.y) ? w.x : -w.y;
        s.tab_scroll = std::clamp(s.tab_scroll + static_cast<int>(std::lround(d * 3)), 0, std::max(0, s.tabs_width - static_cast<int>(s.box.cols)));
    } else if (s.tab == kTabGit) {
        git_panel_wheel(w.y);
    } else if (s.tab == kTabFiles) {
        // (A name being typed for something new is one more row.)
        const size_t rows = s.tree.rows().size() + (s.input == Side::kNewFile || s.input == Side::kNewFolder ? 1 : 0);
        const i64 top = static_cast<i64>(s.top) - static_cast<i64>(std::lround(w.y * 3));
        s.top = static_cast<u32>(std::clamp<i64>(top, 0, last_top(rows, s.body_rows)));
    } else {
        const i64 top = static_cast<i64>(s.problem_top) - static_cast<i64>(std::lround(w.y * 3));
        s.problem_top = static_cast<u32>(std::clamp<i64>(top, 0, last_top(problem_rows(build_problems(), lsp_.problems()).size(), s.body_rows)));
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
    const Look lk = look();
    const int pad_x = lk.edge, pad_y = lk.pad_y, width = panel_px();
    const u32 cols = static_cast<u32>(std::max(1, (width - 2 * pad_x) / cw)), rows = static_cast<u32>(std::max(1, (h - 2 * pad_y) / ch));
    s.body_rows = rows > kHeadRows ? rows - kHeadRows : 0;

    if (s.file_as_given != pane_->doc->ed.path) {
        s.file_as_given = pane_->doc->ed.path;
        s.file_full = full_path(pane_->doc->ed.path);
    }
    const std::string& open_file = s.file_full;
    const bool stale = s.drawn_state != s.state || s.drawn_tree != s.tree.generation() || s.drawn_font != font_->generation() ||
                       s.drawn_rows != rows || s.drawn_cols != cols || s.drawn_file != open_file ||
                       (s.tab == kTabProblems && (s.drawn_diag != lsp_.problems_generation() || s.drawn_bprob != build_prob_gen_)) || (s.tab == kTabGit && s.drawn_git != git_gen_) ||
                       (s.tab == kTabProfile && s.drawn_prof != prof_gen_) || (s.tab == kTabDebug && s.drawn_debug != debug_gen_) ||
                       (s.tab == kTabBuild && s.drawn_build != build_gen_) || (s.tab == kTabTests && s.drawn_tests != test_gen_);
    if (stale) {
        s.drawn_state = s.state;
        s.drawn_tree = s.tree.generation();
        s.drawn_font = font_->generation();
        s.drawn_rows = rows;
        s.drawn_cols = cols;
        s.drawn_file = open_file;
        s.drawn_diag = lsp_.problems_generation();
        s.drawn_bprob = build_prob_gen_;
        s.drawn_git = git_gen_;
        s.drawn_prof = prof_gen_;
        s.drawn_debug = debug_gen_;
        s.drawn_build = build_gen_;
        s.drawn_tests = test_gen_;
        TextBox& box = s.box;
        box.resize(cols, rows);
        Font& font = *font_;

        // The tabs, the one shown on a rounded bar. When their names do not
        // all fit, the others shrink to an icon (or a mark); when even that
        // is too wide they slide, the one shown is kept in view, and a mark
        // at either end says there are more.
        const int room = static_cast<int>(cols);
        int names = -2;
        for (const char* t : kTabs) names += static_cast<int>(std::strlen(t)) + 2;
        const bool compact = names > room;
        bool icons = true;
        for (const u32 cp : kTabIcons) icons = icons && font.has(cp);
        std::string labels[kTabCount];
        for (u32 t = 0, x = 0; t < kTabCount; ++t) {
            if (!compact) labels[t] = kTabs[t];
            else if (t != s.tab) labels[t] = icons ? kTabIconText[t] : kTabMarks[t];
            else labels[t] = icons ? std::string(kTabIconText[t]) + " " + kTabs[t] : std::string(kTabs[t]);
            s.tab_x[t] = static_cast<int>(x);
            s.tab_w[t] = static_cast<int>(width_of(labels[t]));
            x += static_cast<u32>(s.tab_w[t]) + 2;
        }
        s.tabs_width = s.tab_x[kTabCount - 1] + s.tab_w[kTabCount - 1];
        if (s.tab != s.tab_seen || cols != s.tab_cols) {  // (a cell to spare at either end, for the marks)
            s.tab_seen = s.tab;
            s.tab_cols = cols;
            const int a0 = s.tab_x[s.tab], a1 = a0 + s.tab_w[s.tab];
            if (a0 - s.tab_scroll < 1) s.tab_scroll = a0 - 1;
            if (a1 - s.tab_scroll > room - 1) s.tab_scroll = a1 - room + 1;
        }
        s.tab_scroll = std::clamp(s.tab_scroll, 0, std::max(0, s.tabs_width - room));
        for (u32 t = 0; t < kTabCount; ++t) {
            const int at = s.tab_x[t] - s.tab_scroll, len = s.tab_w[t];
            if (at + len <= 0 || at >= room) continue;
            const Ink in = t == s.tab ? ink::kAccentStrong : ink::kMuted;
            std::string_view label = labels[t];
            int col = at;
            if (compact && icons) {  // (the icon one cell wide, as the file tree's are)
                if (col >= 0) box.cells[static_cast<size_t>(col)] = (font.glyph(kTabIcons[t]) & (Font::kWide - 1)) | (in.ink << kCellInkShift);
                label.remove_prefix(std::strlen(kTabIconText[t]));
                ++col;
            }
            // (Cut at the left edge by whole characters.)
            while (col < 0 && !label.empty()) {
                const Decoded d = decode_utf8(reinterpret_cast<const u8*>(label.data()), label.size());
                col += static_cast<int>(cp_width(d.cp));
                label.remove_prefix(std::max<size_t>(d.len, 1));
            }
            if (!label.empty() && col < room) put(box, font, 0, static_cast<u32>(std::max(col, 0)), label, in);
            // The one shown on a rounded bar, and the one under the mouse on a fainter one.
            if (t == s.tab || static_cast<int>(t) == s.hover_tab)
                for (int c = std::max(at - 1, 0); c <= at + len && c < room; ++c) box.cells[static_cast<size_t>(c)] |= t == s.tab ? kCellSelected : kCellHover;
        }
        if (s.tab_scroll > 0) put(box, font, 0, 0, "\xE2\x80\xB9", ink::kMuted);  // a left-pointing mark
        if (s.tabs_width - s.tab_scroll > room) put(box, font, 0, cols - 1, "\xE2\x80\xBA", ink::kMuted);

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
            put(box, font, 1, 3, fit_text(slash == std::string::npos || root.size() == 1 ? root : root.substr(slash + 1), cols > 3 ? cols - 3 : 0), ink::kStrong);

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
                    if (n->dir) put(box, font, r, c0, n->open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", ink::kMuted);  // a small triangle, down or right
                    icon(r, c0 + 2, file_icon(n->name, n->dir, n->open));
                    const u32 fits = cols > c0 + 5 ? cols - c0 - 5 : 0;
                    const Ink in = is_open ? ink::kAccentStrong : ink::kText;
                    if (width_of(n->name) <= fits) put(box, font, r, c0 + 5, n->name, in);
                    else put(box, font, r, c0 + 5, fit_text(n->name, fits), in);
                    if (static_cast<int>(i) == selected) light_row(box, r, s.focus);
                    else if (!s.chosen.empty() && std::ranges::find(s.chosen, s.tree.path_of(n)) != s.chosen.end()) light_row(box, r, false);
                }
                ++r;
                if (input_after == static_cast<int>(i) && r < rows) input_line(r++, input_depth, s.input == Side::kNewFolder);
            }
            if (tree_rows.empty() && s.input == Side::kNoInput && rows > kHeadRows) put(box, font, kHeadRows, 0, "This folder is empty", ink::kMuted);
        } else if (s.tab == kTabGit) {
            git_panel_draw(box, s.focus);
        } else if (s.tab == kTabProfile) {
            prof_panel_draw(box, s.focus);
        } else if (s.tab == kTabDebug) {
            debug_panel_draw(box, s.focus);
        } else if (s.tab == kTabBuild) {
            build_panel_draw(box, s.focus);
        } else if (s.tab == kTabTests) {
            test_panel_draw(box, s.focus);
        } else {
            // The build's problems, across files, then what the language
            // servers report, across files too.
            const std::vector<BuildProblem>& bp = build_problems();
            const std::vector<ProblemFile>& files = lsp_.problems();
            const size_t nl = count_problems(files);
            auto heading = [&](u32 r, const std::string& title, size_t n) {  // a name, and how many
                const std::string count = std::to_string(n);
                const u32 c = put(box, font, r, 0, fit_text(title, cols > count.size() + 1 ? cols - static_cast<u32>(count.size()) - 1 : 0), ink::kHeading);
                put(box, font, r, c + 1, count, ink::kMuted);
            };
            auto lsp_heading = [&](u32 r) {
                if (nl) heading(r, "Problems", nl);
                else if (!lsp_.attached() && server_text_doc(*pane_->doc)) put(box, font, r, 0, fit_text("No language server for this file", cols), ink::kMuted);
                else put(box, font, r, 0, fit_text("No problems", cols), ink::kMuted);
            };
            if (!bp.empty()) heading(1, "Build", bp.size());
            else lsp_heading(1);
            const std::vector<ProblemRow> pr = problem_rows(bp, files);
            const u32 n = static_cast<u32>(bp.size() + nl);
            s.problem = n ? std::min(s.problem, n - 1) : 0;
            s.problem_top = std::min(s.problem_top, last_top(pr.size(), s.body_rows));
            if ((s.focus || s.problem_show) && n) {  // the chosen one stays in view (a file's first, with its file)
                u32 at = row_of_problem(pr, s.problem, bp.size());
                if (at > 0 && (pr[at - 1].kind == ProblemRow::kFile || pr[at - 1].kind == ProblemRow::kLspFile)) --at;
                if (at < s.problem_top) s.problem_top = at;
                const u32 bottom = row_of_problem(pr, s.problem, bp.size());
                if (s.body_rows > 0 && bottom >= s.problem_top + s.body_rows) s.problem_top = bottom - s.body_rows + 1;
            }
            s.problem_show = false;
            const std::string* root = panel_root();
            auto file_row = [&](u32 r, const std::string& path, bool open) {  // its icon, its name, and its folder
                const std::string shown = shown_path(path, root);
                const size_t sl = shown.rfind('/');
                const std::string file = sl == std::string::npos ? shown : shown.substr(sl + 1);
                icon(r, 0, file_icon(file, false, false));
                const u32 c = put(box, font, r, 3, fit_text(file, cols > 3 ? cols - 3 : 0), open ? ink::kText : ink::kMuted);
                if (sl != std::string::npos && sl > 0 && c + 3 < cols) put(box, font, r, c + 1, fit_text(shown.substr(0, sl), cols - c - 1, true), ink::kMuted);
            };
            u32 r = kHeadRows;
            for (size_t i = s.problem_top; i < pr.size() && r < rows; ++i, ++r) {
                const ProblemRow& row = pr[i];
                if (row.kind == ProblemRow::kFile) {
                    file_row(r, bp[row.index].path, true);
                } else if (row.kind == ProblemRow::kBuild) {  // under it: how bad, the line, what
                    const BuildProblem& p = bp[row.index];
                    box.put(font, r, 2, p.severity == 1 ? "E" : "W", kInkDiagnostic + p.severity - 1, Font::kBold);
                    const u32 c = put(box, font, r, 4, std::to_string(p.line), ink::kMuted);
                    const std::string_view message = std::string_view(p.message).substr(0, p.message.find('\n'));
                    put(box, font, r, c + 1, fit_text(message, cols > c + 1 ? cols - c - 1 : 0), ink::kText);
                    if (row.index == s.problem) light_row(box, r, s.focus);
                } else if (row.kind == ProblemRow::kHead) {
                    lsp_heading(r);
                } else if (row.kind == ProblemRow::kLspFile) {
                    file_row(r, files[row.index].path, files[row.index].key != 0);
                } else if (row.kind == ProblemRow::kDiag) {
                    const ProblemFile* file = nullptr;
                    const ProblemFile::Item& x = *lsp_problem(files, row.index, &file);
                    const u32 sev = (x.severity - 1u) & 3u;
                    static const char* const letter[] = {"E", "W", "I", "H"};
                    box.put(font, r, 2, letter[sev], kInkDiagnostic + sev, Font::kBold);
                    const u32 c = put(box, font, r, 4, std::to_string(x.line + 1), ink::kMuted);
                    const std::string_view message = std::string_view(x.message).substr(0, x.message.find('\n'));
                    put(box, font, r, c + 1, fit_text(message, cols > c + 1 ? cols - c - 1 : 0), ink::kText);
                    if (row.index + bp.size() == s.problem) light_row(box, r, s.focus);
                }
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
    // A field typed into is a tone of its own: the text's background, where
    // that is not the panel's; else a little of the way to the text's colour.
    float apart = 0;
    for (int c = 0; c < 3; ++c) apart += std::fabs(theme_.bg[c] - theme_.popup_bg[c]);
    for (int c = 0; c < 3; ++c) s.tones[kFieldTone][c] = apart > 0.03f ? theme_.bg[c] : theme_.popup_bg[c] + (theme_.fg[c] - theme_.popup_bg[c]) * 0.05f;
    s.tones[kFieldTone][3] = 1;
    p.tones = &s.tones[0][0];
    // Lit rows and tabs are rounded bars that reach into the margin, to the
    // inset from the panel's edge; the panel casts a soft shadow on what is
    // beside it, and a line marks its edge.
    const int line = lk.hair;
    if (lk.pill > 0) {
        p.pill = lk.pill;
        p.pill_dx = static_cast<float>(line);
        p.pill_dy = std::round(0.5f * scale_);
        p.pill_out = std::max(0.0f, static_cast<float>(pad_x - lk.inset));
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
    Font& font = *font_;
    const Look l = look();
    const int cw = font.cell_w(), ch = font.cell_h();
    // What each item does, and on the right its key. The cells are made
    // again only when the item chosen or the font has changed.
    const u64 key = (static_cast<u64>(font.generation()) << 32) | s.menu_sel;
    if (key != s.menu_drawn) {
        s.menu_drawn = key;
        std::string keys[kMenuCount];
        u32 widest = 0, widest_key = 0;
        for (u32 i = 0; i < kMenuCount; ++i) {
            keys[i] = key_symbols(kMenuKeys[i], font);
            widest = std::max(widest, width_of(i == kMenuTrash && !FileTree::has_trash() ? "Delete" : kMenu[i]));
            widest_key = std::max(widest_key, width_of(keys[i]));
        }
        const u32 cols = widest + (widest_key ? 3 + widest_key : 0);
        s.menu_box.resize(cols, kMenuCount);
        for (u32 i = 0; i < kMenuCount; ++i) {
            put(s.menu_box, font, i, 0, i == kMenuTrash && !FileTree::has_trash() ? "Delete" : kMenu[i], ink::kText);
            put(s.menu_box, font, i, cols - width_of(keys[i]), keys[i], ink::kMuted);
            if (i == s.menu_sel) s.menu_box.select_row(i);
        }
    }
    Popup& p = fd.popups[fd.popup_count++];
    p.cols = s.menu_box.cols;
    p.rows = s.menu_box.rows;
    p.cells = s.menu_box.cells.data();
    p.pad_x = l.edge;
    p.pad_y = l.pad_y;
    p.w = static_cast<int>(p.cols) * cw + 2 * p.pad_x;
    p.h = static_cast<int>(p.rows) * ch + 2 * p.pad_y;
    p.x = std::clamp(s.menu_x, 0, std::max(0, w - p.w));
    p.y = std::clamp(s.menu_y, 0, std::max(0, h - p.h));
    p.border_px = static_cast<float>(l.hair);
    s.menu_rect = {p.x, p.y, p.w, p.h};
    fx_float(p, kFxMenu);
}

// Notes what of the panel the mouse is over (px < 0: nothing): one of its
// tabs, or a row. True if that changed.
bool App::panel_hover(int px, int py) {
    Side& s = *side_;
    int row = -1, tab = -1;
    if (s.open && px >= 0 && px < panel_px() && !s.menu && !s.resizing && font_) {
        const Look l = look();
        const int cw = font_->cell_w(), ch = font_->cell_h();
        const int r = (py - l.pad_y) / ch, col = static_cast<int>(std::floor(static_cast<double>(px - l.edge) / cw));
        if (py >= l.pad_y && r == 0) {
            const int at = col + s.tab_scroll;
            for (u32 t = 0; t < kTabCount; ++t)
                if (at >= s.tab_x[t] - 1 && at <= s.tab_x[t] + s.tab_w[t]) tab = static_cast<int>(t);
        } else if (py >= l.pad_y && r >= static_cast<int>(kHeadRows)) {
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
