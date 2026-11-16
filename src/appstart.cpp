// The start screen: what blit shows when it starts with no file to open.
// Two tabs: Start, and New project (a kind of project, chosen and set up,
// then made: see project.cpp; init.lua may add kinds with blit.project.add).
// Its name, what can be done first (a new file, a file opened by its path,
// a file found by name or by what is in it, a folder to work in, the
// settings, quitting), and the files and folders opened lately. Each has a
// key of its own; the arrows and Enter, or the mouse, do as well. Esc, or
// "New file", leaves an empty file to type into. It goes for good once its
// empty file is no longer the one in front (something was opened, or typed).
//
// The files and folders opened lately are kept in blit's data folder
// (`recent` beside the grammars and plugins), but only by a blit with a
// window of its own: scripted and benchmark runs neither read nor write
// it, unless BLIT_RECENT_FILE names a file for them to use instead.
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "app.hpp"
#include "fsio.hpp"
#include "grammars.hpp"
#include "libraries.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

enum Act : u8 { kNew, kProject, kOpen, kFind, kSearch, kFolder, kRemote, kSettings, kQuit, kRecent };

// The New project tab's rows.
enum Form : u8 { kFormKind, kFormName, kFormWhere, kFormGit, kFormField, kFormCreate };

struct Recent {
    bool dir = false;
    std::string path;
};

std::string home_short(const std::string& path) {
    const char* home = SDL_getenv("HOME");
    if (!home || !*home) return path;
    const std::string h(home);
    if (path == h) return "~";
    if (path.size() > h.size() && path.starts_with(h) && path[h.size()] == '/') return "~" + path.substr(h.size());
    return path;
}

bool exists(const std::string& path, bool dir) {
    if (is_remote(path)) return true;  // (another machine's: not asked about until it is opened)
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && (dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}

}  // namespace

struct App::StartUi {
    bool on = false;
    u64 doc = 0;  // the empty document it stands in front of
    std::vector<Recent> recent;  // the latest first (all that are kept; those still there are shown)
    // What is on it: each thing that can be chosen, and the row it is on.
    struct Item {
        Act act;
        std::string label, key, shortcut;
        Recent recent;
        u32 row = 0;
    };
    std::vector<Item> items;
    size_t sel = 0;
    int hover = -1;
    u32 left = 0, width = 0;  // the columns the items take
    // Its cells, and what they were made for.
    TextBox box;
    u64 key = 0;
    int x = 0, y = 0, pad_x = 0, pad_y = 0;
    bool folder_asked = false;  // the folder box was opened from here: the panel shows the folder chosen
    bool summoned = false;      // shown again later (the New project command), in a tab of its own
    std::string root_seen;
    u32 tabs_row = 0, tab_c[4] = {};  // the tabs Start and New project: their row, and their columns [c0, c1) each

    // The New project tab: the kinds of project, the one chosen, what was
    // set for each, and the rows of the form.
    int page = 0;  // 0: Start, 1: New project
    std::vector<ProjectKind> kinds;
    size_t kind = 0;
    std::map<std::string, ProjectValues> values;  // by kind
    std::string name, where;
    bool git = true;
    struct Row {
        Form what;
        u32 field = 0;  // kFormField: which of the kind's
        u32 row = 0;    // on the screen (~0: scrolled out of sight)
    };
    std::vector<Row> form;
    size_t form_sel = 0, form_top = 0;
    std::string error, made;
    u64 gen = 1;  // counts up whenever the form changes
    // What the machine the project is to go on has, found on a thread of its
    // own (there, each look is a round trip): vcpkg (its folder as that machine
    // has it and as blit reads it, its ports, the commit it is at), ROCm (the
    // same, and the packages it has), its GPUs, uv, pkg-config's modules.
    struct Facts {
        std::string for_where;
        std::string vcpkg_root, vcpkg_access, vcpkg_baseline;
        std::vector<VcpkgPort> vcpkg_ports;
        std::string rocm_root, rocm_access;
        std::vector<VcpkgPort> rocm_packages;
        std::string gpus, uv_version;
        bool uv = false;
        std::vector<VcpkgPort> pkgconfig;
        bool done = false, applied = false;
    };
    std::mutex facts_mu;
    Facts facts;  // (facts_mu while the worker runs)
    std::jthread facts_worker;
    std::vector<VcpkgPort> pypi;  // PyPI's most downloaded packages, once looked up
    std::vector<std::string> touched;  // "kind\tkey": set by hand, so what is found leaves it alone

    const ProjectKind* chosen() const { return kind < kinds.size() ? &kinds[kind] : nullptr; }
    ProjectValues& kind_values() { return values[kinds[kind].name]; }
    // The rows there are now (a field may be shown only for some values of another).
    void rows_again() {
        std::vector<Row> rows = {{kFormKind}, {kFormName}, {kFormWhere}, {kFormGit}};
        if (const ProjectKind* k = chosen()) {
            const ProjectValues& v = values[k->name];
            for (u32 i = 0; i < k->fields.size(); ++i)
                if (k->fields[i].shown(v)) rows.push_back({kFormField, i});
        }
        rows.push_back({kFormCreate});
        const Row was = form_sel < form.size() ? form[form_sel] : Row{kFormKind};
        form = std::move(rows);
        form_sel = 0;
        for (size_t i = 0; i < form.size(); ++i)
            if (form[i].what == was.what && form[i].field == was.field) form_sel = i;
        ++gen;
    }
};

// Where the list of files and folders opened lately is kept, or nothing if
// this run keeps none (see the top of the file).
static std::string recent_file(const Options& opt, bool windowed) {
    if (const char* p = SDL_getenv("BLIT_RECENT_FILE"); p && *p) return p;
    if (!windowed || opt.bench || opt.script || opt.shot || opt.off_w > 0) return {};
    return grammar_dir() + "/recent";
}

void App::start_init() {
    if (!start_) start_ = std::make_shared<StartUi>();
    StartUi& s = *start_;
    // The list, as kept: a line for each, "d" or "f", a tab, the path.
    if (const std::string file = recent_file(opt_, win_ != nullptr); !file.empty()) {
        size_t n = 0;
        if (char* text = static_cast<char*>(SDL_LoadFile(file.c_str(), &n))) {
            std::string_view all(text, n);
            while (!all.empty()) {
                const size_t nl = all.find('\n');
                const std::string_view line = all.substr(0, nl);
                all = nl == std::string_view::npos ? std::string_view{} : all.substr(nl + 1);
                if (line.size() > 2 && line[1] == '\t' && (line[0] == 'd' || line[0] == 'f')) s.recent.push_back({line[0] == 'd', std::string(line.substr(2))});
            }
            SDL_free(text);
        }
    }
    s.on = cfg_.start_screen && !opt_.file && !opt_.info;
    s.doc = pane_->doc->id;
}

void App::recent_add(const std::string& path, bool dir) {
    if (!start_ || path.empty()) return;
    const std::string file = recent_file(opt_, win_ != nullptr);
    if (file.empty()) return;
    StartUi& s = *start_;
    const std::string full = full_path(path);
    std::erase_if(s.recent, [&](const Recent& r) { return r.path == full; });
    s.recent.insert(s.recent.begin(), {dir, full});
    if (s.recent.size() > 30) s.recent.resize(30);
    std::string out;
    for (const Recent& r : s.recent) out += std::string(r.dir ? "d\t" : "f\t") + r.path + "\n";
    const size_t slash = file.find_last_of('/');
    if (slash != std::string::npos) SDL_CreateDirectory(file.substr(0, slash).c_str());
    const std::string tmp = file + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), out.data(), out.size())) std::rename(tmp.c_str(), file.c_str());
    s.key = 0;  // (shown afresh)
}

bool App::start_shown() {
    if (!start_ || !start_->on) return false;
    StartUi& s = *start_;
    const Doc& d = *pane_->doc;
    if (d.id != s.doc || !d.ed.path.empty() || d.ed.buf.size() > 0 || d.ed.modified() || (panes_.size() != 1 && !s.summoned)) s.on = false;  // (for good)
    return s.on;
}

// Something chosen on it.
void App::start_do(size_t i) {
    StartUi& s = *start_;
    if (i >= s.items.size()) return;
    const StartUi::Item it = s.items[i];
    dirty_ = true;
    switch (it.act) {
        case kNew: s.on = false; break;
        case kProject: project_page(true); break;
        case kOpen: find_open(kFindOpen); break;
        case kFind: find_open(kFindNames); break;
        case kSearch: find_open(kFindText); break;
        case kFolder:
            s.folder_asked = true;
            find_open(kFindFolder);
            break;
        case kRemote:  // a folder on another machine: the box that opens folders, at the machines ~/.ssh/config names
            s.folder_asked = true;
            find_open(kFindFolder, "ssh://");
            break;
        case kSettings: open_config(); break;
        case kQuit: request_quit(); break;
        case kRecent:
            if (it.recent.dir) {
                panel_set_workdir(it.recent.path);
                recent_add(it.recent.path, true);
                s.sel = static_cast<size_t>(kRecent);  // (it is the first of those opened lately now; the actions come before it)
                if (!panel_px()) panel_toggle();  // (the folder chosen, shown)
            } else {
                open_path(it.recent.path.c_str());
            }
            break;
    }
}

bool App::start_key(const SDL_KeyboardEvent& k) {
    if (find_open_ || panel_has_keyboard() || !start_shown()) return false;
    StartUi& s = *start_;
    const bool ctrl = k.mod & SDL_KMOD_CTRL;
    if (k.key == SDLK_TAB && ctrl) {  // the other tab
        project_page(s.page == 0);
        return true;
    }
    if (s.page == 1) {
        const bool cmd = k.mod & kPrimaryMod;
        if (cmd && (k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER)) {
            project_create();
            return true;
        }
        if (cmd && !(ctrl && (k.key == SDLK_N || k.key == SDLK_P))) return false;
        const size_t n = s.form.size();
        auto move = [&](i64 by) {
            if (n) s.form_sel = static_cast<size_t>(((static_cast<i64>(s.form_sel) + by) % static_cast<i64>(n) + static_cast<i64>(n)) % static_cast<i64>(n));
            ++s.gen;
            dirty_ = true;
        };
        switch (k.key) {
            case SDLK_ESCAPE: project_page(false); break;
            case SDLK_UP: move(-1); break;
            case SDLK_DOWN: move(1); break;
            case SDLK_TAB: move(k.mod & SDL_KMOD_SHIFT ? -1 : 1); break;
            case SDLK_N:
                if (ctrl) move(1);
                break;
            case SDLK_P:
                if (ctrl) move(-1);
                break;
            case SDLK_HOME: move(-static_cast<i64>(s.form_sel)); break;
            case SDLK_END: move(static_cast<i64>(n) - 1 - static_cast<i64>(s.form_sel)); break;
            case SDLK_LEFT: project_change(-1); break;
            case SDLK_RIGHT: project_change(1); break;
            case SDLK_BACKSPACE:
            case SDLK_DELETE:  // a library taken away (the last), a line of text emptied
                if (s.form_sel < n && s.form[s.form_sel].what == kFormField) {
                    const ProjectField& f = s.chosen()->fields[s.form[s.form_sel].field];
                    std::string& v = s.kind_values()[f.key];
                    if (f.kind == ProjectField::kLibraries) {
                        const size_t sp = v.find_last_of(' ');
                        v = sp == std::string::npos ? std::string() : v.substr(0, sp);
                    } else if (f.kind == ProjectField::kText) {
                        v.clear();
                    }
                    ++s.gen;
                    dirty_ = true;
                }
                break;
            case SDLK_SPACE: project_change(1); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (s.form_sel < n && s.form[s.form_sel].what == kFormCreate) project_create();
                else project_edit();
                break;
            default: break;  // (letters: start_text())
        }
        return true;
    }
    if ((k.mod & kPrimaryMod) && !(ctrl && (k.key == SDLK_N || k.key == SDLK_P))) return false;  // the application's shortcuts
    const size_t n = s.items.size();
    auto move = [&](i64 by) {
        if (n) s.sel = static_cast<size_t>(((static_cast<i64>(s.sel) + by) % static_cast<i64>(n) + static_cast<i64>(n)) % static_cast<i64>(n));
        dirty_ = true;
    };
    switch (k.key) {
        case SDLK_ESCAPE:
            s.on = false;
            dirty_ = true;
            break;
        case SDLK_UP: move(-1); break;
        case SDLK_DOWN:
        case SDLK_TAB: move(1); break;
        case SDLK_N:
            if (ctrl) move(1);
            break;
        case SDLK_P:
            if (ctrl) move(-1);
            break;
        case SDLK_HOME: s.sel = 0, dirty_ = true; break;
        case SDLK_END: s.sel = n ? n - 1 : 0, dirty_ = true; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: start_do(s.sel); break;
        default: break;  // (a letter's text comes next: start_text())
    }
    return true;
}

bool App::start_text(const char* text) {
    if (find_open_ || panel_has_keyboard() || !start_shown()) return false;
    StartUi& s = *start_;
    const std::string_view t(text);
    if (s.page == 1) {  // typed on a row of text: that text, typed into it afresh
        if (t != " " && s.form_sel < s.form.size()) project_edit(std::string(t));
        return true;
    }
    if (t == "j" || t == "k") {
        if (!s.items.empty()) s.sel = (s.sel + (t == "j" ? 1 : s.items.size() - 1)) % s.items.size();
        dirty_ = true;
        return true;
    }
    for (size_t i = 0; i < s.items.size(); ++i)
        if (!s.items[i].key.empty() && t == s.items[i].key) {
            s.sel = i;
            start_do(i);
            break;
        }
    return true;  // (nothing else typed goes anywhere while it shows)
}

// The item at a point of the window, or -1.
int App::start_item_at(int px, int py) const {
    const StartUi& s = *start_;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    if (px < s.x + s.pad_x || py < s.y + s.pad_y) return -1;
    const u32 col = static_cast<u32>((px - s.x - s.pad_x) / cw), row = static_cast<u32>((py - s.y - s.pad_y) / ch);
    if (col < s.left || col >= s.left + s.width) return -1;
    for (size_t i = 0; i < s.items.size(); ++i)
        if (s.items[i].row == row) return static_cast<int>(i);
    return -1;
}

bool App::start_mouse_down(const SDL_MouseButtonEvent& b) {
    if (!start_shown()) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (pane_at(px, py) != pane_ || py < pane_->y0) return false;
    StartUi& s = *start_;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    if (b.button == SDL_BUTTON_LEFT && px >= s.x + s.pad_x && py >= s.y + s.pad_y) {
        const u32 col = static_cast<u32>((px - s.x - s.pad_x) / cw), row = static_cast<u32>((py - s.y - s.pad_y) / ch);
        if (row == s.tabs_row && col >= s.tab_c[0] && col < s.tab_c[1]) project_page(false);
        else if (row == s.tabs_row && col >= s.tab_c[2] && col < s.tab_c[3]) project_page(true);
        else if (s.page == 1) {
            for (size_t i = 0; i < s.form.size(); ++i)
                if (s.form[i].row == row && col >= s.left && col < s.left + s.width) {
                    s.form_sel = i;
                    ++s.gen;
                    if (s.form[i].what == kFormCreate) project_create();
                    else if (s.form[i].what == kFormKind || s.form[i].what == kFormGit) project_change(1);
                    else if (s.form[i].what == kFormField && s.chosen()->fields[s.form[i].field].kind != ProjectField::kText) project_change(1);
                    else project_edit();
                    break;
                }
        } else if (const int i = start_item_at(px, py); i >= 0) {
            s.sel = static_cast<size_t>(i);
            start_do(static_cast<size_t>(i));
        }
    }
    dirty_ = true;
    return true;
}

bool App::start_mouse_motion(const SDL_MouseMotionEvent& m) {
    if (!start_shown()) return false;
    const int px = static_cast<int>(m.x * density_), py = static_cast<int>(m.y * density_);
    if (pane_at(px, py) != pane_) return false;
    const int i = start_->page == 0 ? start_item_at(px, py) : -1;
    if (i != start_->hover) {
        start_->hover = i;
        dirty_ = true;
    }
    return true;
}

void App::start_layout(FrameDesc& fd) {
    if (!start_shown() || fd.popup_count >= std::size(fd.popups)) return;
    StartUi& s = *start_;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    const Pane& p = *pane_;
    const int w = p.x1 - p.x0, h = p.y1 - p.y0;
    if (w < cw * 8 || h < ch * 4) return;
    const u32 cols = static_cast<u32>(w / cw), rows = static_cast<u32>(h / ch);
    const std::string* panel_root_now = panel_root();
    const std::string root = panel_root_now ? *panel_root_now : default_workdir();
    if (s.folder_asked && root != s.root_seen && !s.root_seen.empty() && !find_open_) {  // a folder was chosen in the box opened from here: shown
        s.folder_asked = false;
        if (!panel_px()) panel_toggle();
    }
    s.root_seen = root;

    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
    mix(cols);
    mix(rows);
    mix(font.generation());
    mix(s.sel);
    mix(static_cast<u64>(s.hover + 1));
    mix(std::hash<std::string>{}(root));
    mix(s.recent.size());
    mix(static_cast<u64>(s.page));
    mix(s.gen);
    if (s.page == 1) {  // (what the machine has, once it is found)
        std::lock_guard lock(s.facts_mu);
        mix(s.facts.done);
    }
    if (s.page == 1 && (key != s.key || s.items.empty())) {
        s.key = key;
        s.box.resize(cols, rows);
        project_cells(cols, rows);
    } else if (key != s.key || s.items.empty()) {
        s.key = key;
        const bool apple = kPrimaryMod == SDL_KMOD_GUI;
        const bool glyphs = apple && font.has(0x2318) && font.has(0x21E7);  // (⌘ and ⇧, or their names)
        const std::string cmd = glyphs ? "\xE2\x8C\x98" : apple ? "Cmd+" : "Ctrl+", shift = glyphs ? "\xE2\x87\xA7" : "Shift+";
        s.items.clear();
        s.items.push_back({kNew, "New file", "n", "", {}});
        s.items.push_back({kProject, "New project\xE2\x80\xA6", "p", "", {}});
        s.items.push_back({kOpen, "Open a file\xE2\x80\xA6", "o", cmd + "O", {}});
        s.items.push_back({kFind, "Find a file in the folder", "f", cmd + "P", {}});
        s.items.push_back({kSearch, "Search the folder's files", "s", cmd + shift + "F", {}});
        s.items.push_back({kFolder, "Open a folder\xE2\x80\xA6", "d", "", {}});
        s.items.push_back({kRemote, "Connect to a server\xE2\x80\xA6", "r", "", {}});
        s.items.push_back({kSettings, "Settings (init.lua)", "c", cmd + ",", {}});
        s.items.push_back({kQuit, "Quit", "q", cmd + "Q", {}});
        const size_t actions = s.items.size();
        u32 shown = 0;
        for (const Recent& r : s.recent) {
            if (shown == 9) break;
            if (!exists(r.path, r.dir)) continue;
            s.items.push_back({kRecent, "", std::to_string(++shown), "", r});
        }
        s.sel = std::min(s.sel, s.items.size() - 1);

        // How it is laid out, top to bottom, and what goes when there is not room.
        const bool logo = font.has(0x2588) && font.has(0x2580) && font.has(0x2584);
        const u32 recent_rows = shown ? static_cast<u32>(shown) + 2 : 0;  // (a blank row and a heading before them)
        u32 need = (logo ? 3 : 1) + 2 + static_cast<u32>(actions) + recent_rows + 2;
        bool tagline = true, footer = true;
        u32 recent_room = shown;
        const u32 below = rows > 3 ? rows - 3 : 0;  // (below the tabs)
        if (need > below && footer) footer = false, need -= 2;
        if (need > below && tagline) tagline = false, need -= 2;
        while (need > below && recent_room > 0) --recent_room, --need;
        s.items.resize(actions + recent_room);
        s.sel = std::min(s.sel, s.items.size() - 1);
        s.width = std::min<u32>(cols > 4 ? cols - 4 : cols, 64);
        s.left = (cols - s.width) / 2;
        s.box.resize(cols, rows);
        u32 r = 3 + (below > need ? (below - need) / 2 : 0);
        auto centred = [&](u32 row, std::string_view text, u32 ink, u32 style = 0) {
            const u32 n = width_of(text);
            s.box.put(font, row, cols > n ? (cols - n) / 2 : 0, text, ink, style);
        };
        if (logo) {  // the name, in blocks of half a cell
            centred(r++, "\u2588   \u2588 \u2580 \u2584\u2588\u2584", kInkAccent);  // █   █ ▀ ▄█▄
            centred(r++, "\u2588\u2580\u2584 \u2588 \u2588  \u2588 ", kInkAccent);    // █▀▄ █ █  █
            centred(r++, "\u2588\u2584\u2580 \u2588 \u2588  \u2588\u2584", kInkAccent);  // █▄▀ █ █  █▄
        } else {
            centred(r++, "blit", kInkAccent, Font::kBold);
        }
        if (tagline) {
            centred(r + 1, "a fast text editor", kInkMuted);
            r += 2;
        }
        r += 1;
        // The columns: the labels from the left, the keys and then the
        // shortcuts against the right; with too little room the shortcuts
        // go, and then the labels are cut.
        u32 label_w = 0, cut_w = 0;
        for (size_t i = 0; i < actions; ++i) {
            label_w = std::max(label_w, width_of(s.items[i].label));
            cut_w = std::max(cut_w, width_of(s.items[i].shortcut));
        }
        const u32 right = s.left + s.width - 2;  // (two blank columns inside each end of a lit row)
        const bool with_cuts = s.left + 2 + label_w + 3 + 1 + 3 + cut_w <= right;
        const u32 cut_col = with_cuts ? right - cut_w : right, key_col = with_cuts ? cut_col - 3 : right - 1;
        const u32 label_room = key_col > s.left + 4 ? key_col - s.left - 4 : 1;
        auto clipped = [&](const std::string& text, u32 n) {
            if (width_of(text) <= n) return text;
            size_t at = 0;
            for (u32 k = 0; k + 1 < n && at < text.size(); ++k) at += decode_utf8(reinterpret_cast<const u8*>(text.data()) + at, text.size() - at).len;
            return text.substr(0, at) + "\xE2\x80\xA6";
        };
        for (size_t i = 0; i < s.items.size(); ++i) {
            StartUi::Item& it = s.items[i];
            if (i == actions) {  // the heading of those opened lately
                s.box.put(font, r + 1, s.left + 2, "Opened lately", kInkMuted);
                r += 2;
            }
            it.row = r;
            if (it.act == kRecent) {
                s.box.put(font, r, s.left + 2, it.key, kInkAccent);
                const std::string& path = it.recent.path;
                const size_t slash = path.find_last_of('/');
                const std::string name = (slash == std::string::npos ? path : path.substr(slash + 1)) + (it.recent.dir ? "/" : "");
                const std::string where = home_short(slash == std::string::npos ? std::string() : path.substr(0, std::max<size_t>(slash, 1)));
                const u32 end = s.box.put(font, r, s.left + 5, name, 0, it.recent.dir ? Font::kBold : 0);
                if (end + 2 < s.left + s.width) {
                    // (the folder it is in, cut from its start if it is long)
                    const u32 room = s.left + s.width - end - 3;
                    std::string shown_where = where;
                    if (width_of(shown_where) > room && room > 1) {
                        size_t at = 0;
                        for (u32 cut = width_of(shown_where) - room + 1; cut > 0 && at < shown_where.size(); --cut)
                            at += decode_utf8(reinterpret_cast<const u8*>(shown_where.data()) + at, shown_where.size() - at).len;
                        shown_where = "\xE2\x80\xA6" + shown_where.substr(at);
                    }
                    s.box.put(font, r, end + 2, shown_where, kInkMuted);
                }
            } else {
                s.box.put(font, r, s.left + 2, clipped(it.label, label_room), 0);
                s.box.put(font, r, key_col, it.key, kInkAccent);
                if (with_cuts && !it.shortcut.empty()) s.box.put(font, r, cut_col, it.shortcut, kInkMuted);
            }
            const u32 lit = i == s.sel ? kCellSelected : static_cast<int>(i) == s.hover ? kCellHover : 0;
            if (lit)
                for (u32 c = s.left; c < s.left + s.width && c < cols; ++c) s.box.cells[static_cast<size_t>(r) * cols + c] |= lit;
            ++r;
        }
        if (footer) {  // the folder and the settings file, shortened from the start of the folder's path if need be
            std::string folder = home_short(root);
            const std::string settings = "   \xC2\xB7   settings " + home_short(init_path_);
            const u32 room = cols > 2 ? cols - 2 : cols;
            const bool with_settings = 7 + 20 + width_of(settings) <= room;
            const u32 most = room - 7 - (with_settings ? width_of(settings) : 0);
            if (width_of(folder) > most && most > 2) {
                size_t at = 0;
                for (u32 cut = width_of(folder) - most + 1; cut > 0 && at < folder.size(); --cut)
                    at += decode_utf8(reinterpret_cast<const u8*>(folder.data()) + at, folder.size() - at).len;
                if (const size_t slash = folder.find('/', at); slash != std::string::npos) at = slash;  // (whole folders go)
                folder = "\xE2\x80\xA6" + folder.substr(at);
            }
            centred(r + 1, "folder " + folder + (with_settings ? settings : std::string()), kInkMuted);
        }
    }
    if (s.box.cols == cols && rows > 2) {  // the tabs, along the top
        const char* const names[2] = {"Start", "New project"};
        const u32 total = width_of(names[0]) + 4 + width_of(names[1]);
        u32 c = cols > total ? (cols - total) / 2 : 0;
        s.tabs_row = 1;
        for (int t = 0; t < 2; ++t) {
            const u32 n = width_of(names[t]);
            for (u32 x = c > 0 ? c - 1 : 0; x < c + n + 1 && x < cols; ++x) s.box.cells[cols + x] &= ~(kCellSelected | kCellHover);
            s.box.put(font, 1, c, names[t], t == s.page ? kInkAccent : kInkMuted, t == s.page ? Font::kBold : 0);
            if (t == s.page)
                for (u32 x = c > 0 ? c - 1 : 0; x < c + n + 1 && x < cols; ++x) s.box.cells[cols + x] |= kCellSelected;
            s.tab_c[2 * t] = c > 0 ? c - 1 : 0;
            s.tab_c[2 * t + 1] = c + n + 1;
            c += n + 4;
        }
    }
    s.x = p.x0;
    s.y = p.y0;
    s.pad_x = (w - static_cast<int>(cols) * cw) / 2;
    s.pad_y = (h - static_cast<int>(rows) * ch) / 2;
    Popup& q = fd.popups[fd.popup_count++];
    q.x = p.x0;
    q.y = p.y0;
    q.w = w;
    q.h = h;
    q.pad_x = s.pad_x;
    q.pad_y = s.pad_y;
    q.cols = s.box.cols;
    q.rows = s.box.rows;
    q.cells = s.box.cells.data();
    q.border_px = 0;
    q.bg = theme_.bg;
    const Look lk = look();
    if (lk.pill > 0) {  // (the lit row a rounded bar, as in the side panel)
        q.pill = lk.pill;
        q.pill_dx = static_cast<float>(std::max(1, static_cast<int>(std::lround(scale_))));
        q.pill_dy = std::round(0.5f * scale_);
    }
}

// ---------------------------------------------------------------- the New project tab

// The New project tab from anywhere (Cmd+Option+N): the start screen again,
// in a new empty tab, at that tab.
void App::project_anywhere() {
    if (!start_) return;
    StartUi& s = *start_;
    if (!start_shown()) {
        tab_new();
        s.doc = pane_->doc->id;
        s.on = s.summoned = true;
    }
    project_page(true);
}

void App::project_page(bool on) {
    StartUi& s = *start_;
    s.page = on ? 1 : 0;
    if (on) {
        // The kinds there are now (init.lua may have added some since), each with what was set for it.
        const std::string was = s.chosen() ? s.chosen()->name : std::string();
        s.kinds = script_project_kinds();
        s.kind = 0;
        for (size_t i = 0; i < s.kinds.size(); ++i) {
            if (s.kinds[i].name == was) s.kind = i;
            ProjectValues& v = s.values[s.kinds[i].name];
            for (const auto& [k, d] : project_defaults(s.kinds[i].fields)) v.try_emplace(k, d);
        }
        if (s.where.empty()) {
            const char* home = SDL_getenv("HOME");
            const std::string h = home && *home ? home : "/";
            s.where = fs_kind(h + "/Dev") == 2 ? h + "/Dev" : h;
        }
        s.rows_again();
        project_facts(false);
    }
    s.error.clear();
    s.key = 0;
    dirty_ = true;
}

// The chosen row's value a step along: the next choice (or the one before), a switch the other way.
void App::project_change(int by) {
    StartUi& s = *start_;
    if (s.form_sel >= s.form.size() || s.kinds.empty()) return;
    const StartUi::Row row = s.form[s.form_sel];
    auto step = [&](const std::vector<std::string>& choices, std::string& v) {
        if (choices.empty()) return;
        size_t at = 0;
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i] == v) at = i;
        const i64 n = static_cast<i64>(choices.size());
        v = choices[static_cast<size_t>(((static_cast<i64>(at) + by) % n + n) % n)];
    };
    switch (row.what) {
        case kFormKind: {
            const i64 n = static_cast<i64>(s.kinds.size());
            s.kind = static_cast<size_t>(((static_cast<i64>(s.kind) + by) % n + n) % n);
            break;
        }
        case kFormGit: s.git = !s.git; break;
        case kFormField: {
            const ProjectField& f = s.chosen()->fields[row.field];
            std::string& v = s.kind_values()[f.key];
            if (f.kind == ProjectField::kSwitch) v = v == "true" ? "false" : "true";
            else if (f.kind == ProjectField::kChoice) step(f.choices, v);
            else return;
            s.touched.push_back(s.chosen()->name + "\t" + f.key);
            break;
        }
        default: return;
    }
    s.error.clear();
    s.rows_again();
    dirty_ = true;
}

// A row of text, typed into (in the box along the top): typed, if given, is what it starts with.
void App::project_edit(const std::string& typed) {
    StartUi& s = *start_;
    if (s.form_sel >= s.form.size() || s.kinds.empty()) return;
    const StartUi::Row row = s.form[s.form_sel];
    auto done = [this](std::string key) {
        return [this, key](const std::string& v) {
            project_set(key, v);
            dirty_ = true;
        };
    };
    switch (row.what) {
        case kFormName: find_choose(kFindInput, "Name", typed.empty() ? s.name : typed, done("name")); break;
        case kFormWhere: {  // a folder: the box that opens folders, starting where it is set
            std::string start = typed.empty() ? home_short(s.where) + "/" : typed;
            find_choose(kFindFolder, "Make it in", start, done("where"));
            break;
        }
        case kFormField: {
            const ProjectField& f = s.chosen()->fields[row.field];
            if (f.kind == ProjectField::kLibraries) {
                project_libraries(typed);
                return;
            }
            if (f.kind != ProjectField::kText) {
                if (typed.empty()) project_change(1);
                return;
            }
            find_choose(kFindInput, f.label, typed.empty() ? s.kind_values()[f.key] : typed, done(f.key));
            break;
        }
        case kFormKind:
        case kFormGit:
            if (typed.empty()) project_change(1);
            break;
        case kFormCreate: break;
    }
}

// What the machine the project goes on has: looked for on a thread (on
// another machine that is round trips); `wait`: until it is done.
void App::project_facts(bool wait) {
    StartUi& s = *start_;
    bool fresh = false;
    {
        std::lock_guard lock(s.facts_mu);
        fresh = s.facts.for_where != s.where;
    }
    if (fresh) {
        if (s.facts_worker.joinable()) s.facts_worker.join();
        {
            std::lock_guard lock(s.facts_mu);
            s.facts = {};
            s.facts.for_where = s.where;
        }
        const std::string where = s.where;
        s.facts_worker = std::jthread([&s, where] {
            StartUi::Facts f;
            f.for_where = where;
            std::string host, there;
            const bool remote = split_remote(where, &host, &there);
            auto access = [&](const std::string& path) { return path.empty() ? path : remote ? remote_path(host, path) : path; };
            f.vcpkg_root = vcpkg_root(where);
            f.vcpkg_access = access(f.vcpkg_root);
            f.vcpkg_ports = vcpkg_ports(f.vcpkg_access, !remote);  // (a description each: a file each, too many round trips there)
            f.vcpkg_baseline = vcpkg_baseline(f.vcpkg_access);
            f.rocm_root = rocm_root(where);
            f.rocm_access = access(f.rocm_root);
            f.rocm_packages = rocm_packages(f.rocm_access);
            const std::string dir = remote ? where : std::string();
            if (!f.rocm_root.empty()) f.gpus = gpu_targets(dir, f.rocm_root);
            std::string out;
            if (fs_run(dir, "uv --version", &out, 5000) && out.starts_with("uv ")) {
                f.uv = true;
                f.uv_version = out.substr(3, out.find_first_of(" \n", 3) - 3);
            }
            f.pkgconfig = pkgconfig_modules(dir);
            f.done = true;
            {
                std::lock_guard lock(s.facts_mu);
                if (s.facts.for_where == where) s.facts = std::move(f);
            }
            SDL_Event ev{};
            ev.type = SDL_EVENT_USER;
            ev.user.code = 16;  // (a frame: the form says what was found)
            SDL_PushEvent(&ev);
        });
    }
    if (wait && s.facts_worker.joinable()) s.facts_worker.join();
    ++s.gen;
}

// What was found, as defaults: the GPUs there, uv if it is there (for the
// fields not set by hand).
void App::project_facts_apply() {
    StartUi& s = *start_;
    std::lock_guard lock(s.facts_mu);
    if (!s.facts.done || s.facts.applied) return;
    s.facts.applied = true;
    auto set = [&](const std::string& kind, const std::string& key, const std::string& value) {
        if (value.empty() || std::find(s.touched.begin(), s.touched.end(), kind + "\t" + key) != s.touched.end()) return;
        if (const auto it = s.values.find(kind); it != s.values.end()) it->second[key] = value;
    };
    set("HIP", "gpu", s.facts.gpus);
    set("Python", "manager", s.facts.uv ? "uv" : "pip");
    ++s.gen;
}

// A row of libraries: those of its source to choose from, those chosen
// ticked; choosing one adds it (or takes it away), and the list comes back
// for the next.
void App::project_libraries(const std::string& typed) {
    StartUi& s = *start_;
    if (s.form_sel >= s.form.size() || s.form[s.form_sel].what != kFormField) return;
    const ProjectField& field = s.chosen()->fields[s.form[s.form_sel].field];
    const std::string key = field.key, source = field.source;
    project_facts(true);
    const std::vector<VcpkgPort>* list = nullptr;
    std::string prompt, missing;
    if (source == "vcpkg") {
        list = &s.facts.vcpkg_ports, prompt = "Library (vcpkg)";
        if (s.facts.vcpkg_root.empty()) missing = "vcpkg is not to be found: VCPKG_ROOT says where it is (or clone it to ~/vcpkg).";
    } else if (source == "rocm") {
        list = &s.facts.rocm_packages, prompt = "ROCm library";
        if (s.facts.rocm_root.empty()) missing = "ROCm is not to be found: ROCM_PATH says where it is (else /opt/rocm).";
    } else if (source == "pkg-config") {
        list = &s.facts.pkgconfig, prompt = "pkg-config module";
        if (list->empty()) missing = "pkg-config has no modules there (or is not installed).";
    } else if (source == "pypi") {
        if (s.pypi.empty()) s.pypi = pypi_top(grammar_dir() + "/cache");
        list = &s.pypi, prompt = "PyPI package";
    }
    if (!list || !missing.empty()) {
        s.error = list ? missing : "No lookup called " + source + ".";
        s.key = 0;
        dirty_ = true;
        return;
    }
    std::vector<std::string> chosen;
    {
        std::string word;
        for (const char c : s.kind_values()[key] + " ") {
            if (c != ' ') word += c;
            else if (!word.empty()) chosen.push_back(std::move(word)), word.clear();
        }
    }
    // (PyPI's list is its most downloaded: any other is named as it is.)
    const bool by_name = source == "pypi";
    std::vector<std::string> items;
    items.reserve(list->size() + 1);
    if (by_name) items.push_back("  Another package, by its exact name\xE2\x80\xA6");
    for (const VcpkgPort& p : *list) {
        const bool on = std::find(chosen.begin(), chosen.end(), p.name) != chosen.end();
        items.push_back(std::string(on ? "\xE2\x9C\x93 " : "  ") + p.name + (p.version.empty() ? "" : "  " + p.version) + (p.description.empty() ? "" : "  \xE2\x80\x94 " + p.description));
    }
    auto toggle = [this, key](const std::string& name) {
        StartUi& t = *start_;
        std::string names = " " + t.kind_values()[key] + " ";
        if (const size_t at = names.find(" " + name + " "); at != std::string::npos) names.erase(at, name.size() + 1);
        else names += name + " ";
        while (names.starts_with(' ')) names.erase(0, 1);
        while (names.ends_with(' ')) names.pop_back();
        project_set(key, names);
        project_libraries();  // (the next)
    };
    find_choose_item(prompt, std::move(items), [this, list, by_name, toggle](int i) {
        if (by_name && i == 0) {
            find_choose(kFindInput, "PyPI package", "", [toggle](const std::string& asked) {
                std::string name;
                for (const char c : asked)
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '[' || c == ']' || c == ',') name += c;
                if (!name.empty()) toggle(name);
            });
            return;
        }
        const size_t at = static_cast<size_t>(i - (by_name ? 1 : 0));
        if (i < 0 || at >= list->size()) return;
        toggle((*list)[at].name);
    }, typed);
}

// A value set by its key: kind, name, where, git, or one of the kind's fields.
void App::project_set(const std::string& key, const std::string& value) {
    StartUi& s = *start_;
    if (s.kinds.empty()) project_page(true);
    if (key == "kind") {
        for (size_t i = 0; i < s.kinds.size(); ++i)
            if (s.kinds[i].name == value) s.kind = i;
    } else if (key == "name") {
        s.name = value;
    } else if (key == "where") {
        std::string w = value;
        const char* home = SDL_getenv("HOME");
        if (home && *home && (w == "~" || w.starts_with("~/"))) w = home + w.substr(1);
        while (w.size() > 1 && w.back() == '/') w.pop_back();
        s.where = w;
        project_facts(false);
    } else if (key == "git") {
        s.git = value == "true" || value == "on" || value == "1";
    } else if (const ProjectKind* k = s.chosen()) {
        for (const ProjectField& f : k->fields)
            if (f.key == key) {
                ProjectValues& v = s.kind_values();
                v[key] = f.kind == ProjectField::kSwitch ? (value == "true" || value == "on" || value == "1" ? "true" : "false") : value;
                s.touched.push_back(k->name + "\t" + key);
                if (f.kind == ProjectField::kLibraries && (f.source == "vcpkg" || f.source == "rocm")) {  // how CMake uses each, from its usage notes
                    project_facts(true);
                    std::string word;
                    for (const char c : value + " ") {
                        if (c != ' ') {
                            word += c;
                        } else if (!word.empty()) {
                            if (!v.contains("usage:" + word))
                                v["usage:" + word] = f.source == "rocm" ? rocm_usage(s.facts.rocm_access, word) : vcpkg_usage(s.facts.vcpkg_access, word);
                            word.clear();
                        }
                    }
                }
            }
    }
    s.error.clear();
    s.rows_again();
    s.key = 0;
    dirty_ = true;
}

namespace {

// A name a folder (and the build systems' targets) can have.
bool good_name(const std::string& n) {
    if (n.empty() || !std::isalnum(static_cast<unsigned char>(n[0]))) return false;
    for (const char c : n)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

}  // namespace

// The project made: its folder and files written, the folder made the one
// worked in, its main file opened, and what it runs once made run in the
// build's output terminal.
bool App::project_create() {
    StartUi& s = *start_;
    const ProjectKind* k = s.chosen();
    s.error.clear();
    s.key = 0;
    dirty_ = true;
    if (!k) return false;
    if (!good_name(s.name)) {
        s.error = s.name.empty() ? "It needs a name first." : "A name of letters, digits, '-', '_' and '.', starting with a letter or digit.";
        return false;
    }
    const std::string dir = s.where + "/" + s.name;
    if (fs_kind(dir) == 1 || (fs_kind(dir) == 2 && !fs_list(dir).empty())) {
        s.error = home_short(dir) + " is there already.";
        return false;
    }
    ProjectValues values = s.kind_values();
    values["name"] = s.name;
    values["git"] = s.git ? "true" : "false";
    project_facts(true);
    project_facts_apply();
    values = s.kind_values();
    values["name"] = s.name;
    values["git"] = s.git ? "true" : "false";
    values["vcpkg_root"] = s.facts.vcpkg_root;
    values["vcpkg_baseline"] = s.facts.vcpkg_baseline;
    values["rocm_root"] = s.facts.rocm_root;
    values["uv_version"] = s.facts.uv_version;
    std::string err, open, then;
    if (!fs_mkdirs(dir, &err)) {
        s.error = "Cannot make " + home_short(dir) + ": " + err;
        return false;
    }
    if (k->scripted) {
        if (!script_project_create(k->name, dir, values, &open, &then, &err)) {
            s.error = err;
            return false;
        }
        if (!open.empty() && open[0] != '/' && !is_remote(open)) open = dir + "/" + open;
    } else {
        const ProjectPlan plan = k->plan(values);
        for (const ProjectFile& f : plan.files) {
            const std::string path = dir + "/" + f.path;
            const size_t slash = path.find_last_of('/');
            if ((slash > dir.size() && !fs_mkdirs(path.substr(0, slash), &err)) || !fs_write(path, f.text, &err)) {
                s.error = "Cannot write " + home_short(path) + ": " + err;
                return false;
            }
        }
        if (!plan.open.empty()) open = dir + "/" + plan.open;
        then = plan.then;
    }
    if (s.git) then = "git init -q" + (then.empty() ? std::string() : "; " + then);
    s.made = dir;
    panel_set_workdir(dir);
    recent_add(dir, true);
    if (!panel_px()) panel_toggle();
    if (!open.empty()) open_path(open.c_str());
    s.on = false;
    if (!then.empty()) build_command(dir, then, "Setting up " + s.name);
    build_changed();
    return true;
}

// The New project tab's cells (start_layout).
void App::project_cells(u32 cols, u32 rows) {
    StartUi& s = *start_;
    project_facts_apply();
    Font& font = *font_;
    TextBox& box = s.box;
    s.width = std::min<u32>(cols > 4 ? cols - 4 : cols, 76);
    s.left = (cols - s.width) / 2;
    const ProjectKind* k = s.chosen();
    if (!k) {
        box.put(font, 3, s.left + 2, "No kinds of project (init.lua removed them all).", kInkMuted);
        return;
    }
    const ProjectValues& v = s.values[k->name];
    const bool dots = font.has(0x25CF) && font.has(0x25CB), quotes = font.has(0x2039) && font.has(0x203A);
    auto label_of = [&](const StartUi::Row& r) -> std::string {
        switch (r.what) {
            case kFormKind: return "Kind";
            case kFormName: return "Name";
            case kFormWhere: return "Location";
            case kFormGit: return "Git repository";
            case kFormField: return k->fields[r.field].label;
            case kFormCreate: return "Create";
        }
        return {};
    };
    u32 label_w = 0;
    for (const StartUi::Row& r : s.form) label_w = std::max(label_w, width_of(label_of(r)));
    label_w = std::min(label_w, s.width / 2);
    const u32 value_col = s.left + 2 + label_w + 3, right = s.left + s.width - 1;
    auto cut = [&](const std::string& text, u32 n) {
        if (width_of(text) <= n) return text;
        size_t at = 0;
        for (u32 c = 0; c + 1 < n && at < text.size(); ++c) at += decode_utf8(reinterpret_cast<const u8*>(text.data()) + at, text.size() - at).len;
        return text.substr(0, at) + "\xE2\x80\xA6";
    };
    auto put_switch = [&](u32 row, bool on) {
        box.put(font, row, value_col, dots ? (on ? "\xE2\x97\x8F on" : "\xE2\x97\x8B off") : (on ? "[x] on" : "[ ] off"), on ? kInkAccent : kInkMuted);
    };
    auto put_choice = [&](u32 row, const std::string& value) {
        u32 c = box.put(font, row, value_col, quotes ? "\xE2\x80\xB9 " : "< ", kInkMuted);
        c = box.put(font, row, c, value, 0, Font::kBold);
        return box.put(font, row, c, quotes ? " \xE2\x80\xBA" : " >", kInkMuted);
    };

    // What will be made, for the lines under the form.
    std::vector<std::string> writes;
    std::string then;
    if (k->scripted) {
        writes.push_back("what init.lua's create function writes");
    } else {
        ProjectValues full = v;
        full["name"] = s.name.empty() ? "name" : s.name;
        full["git"] = s.git ? "true" : "false";
        const ProjectPlan plan = k->plan(full);
        for (const ProjectFile& f : plan.files) writes.push_back(f.path);
        then = plan.then;
    }
    if (s.git) then = "git init -q" + (then.empty() ? std::string() : "; " + then);

    // The rows, with as many as fit shown (the chosen one always).
    const u32 foot = 7;  // a blank, what it writes (2), what it runs, a blank, an error, the keys
    const u32 top = 3, body = rows > top + foot + 2 ? rows - top - foot : 2;
    const size_t n = s.form.size();
    if (s.form_sel < s.form_top) s.form_top = s.form_sel;
    if (s.form_sel >= s.form_top + body) s.form_top = s.form_sel - body + 1;
    if (s.form_top + body > n) s.form_top = n > body ? n - body : 0;
    for (StartUi::Row& r : s.form) r.row = ~0u;
    u32 y = top;
    for (size_t i = s.form_top; i < n && y < top + body; ++i, ++y) {
        StartUi::Row& r = s.form[i];
        r.row = y;
        const std::string label = cut(label_of(r), label_w);
        if (r.what == kFormCreate) {
            box.put(font, y, s.left + 2, label, kInkAccent, Font::kBold);
            box.put(font, y, value_col, cut(home_short(s.where + "/" + (s.name.empty() ? std::string("\xE2\x80\xA6") : s.name)), right - value_col), kInkMuted);
        } else {
            box.put(font, y, s.left + 2, label, 0);
            switch (r.what) {
                case kFormKind: {
                    const u32 c = put_choice(y, k->name);
                    if (c + 3 < right && !k->description.empty()) box.put(font, y, c + 2, cut(k->description, right - c - 2), kInkMuted);
                    break;
                }
                case kFormName:
                    if (s.name.empty()) box.put(font, y, value_col, "type a name", kInkMuted);
                    else box.put(font, y, value_col, cut(s.name, right - value_col), 0, Font::kBold);
                    break;
                case kFormWhere: box.put(font, y, value_col, cut(home_short(s.where), right - value_col), 0); break;
                case kFormGit: put_switch(y, s.git); break;
                case kFormField: {
                    const ProjectField& f = k->fields[r.field];
                    const auto it = v.find(f.key);
                    const std::string val = it == v.end() ? std::string() : it->second;
                    if (f.kind == ProjectField::kSwitch) {
                        put_switch(y, val == "true");
                    } else if (f.kind == ProjectField::kChoice) {
                        put_choice(y, val);
                    } else if (f.kind == ProjectField::kLibraries) {
                        std::string shown;
                        for (const char c : val) shown += c == ' ' ? std::string(", ") : std::string(1, c);
                        std::lock_guard lock(s.facts_mu);
                        const StartUi::Facts& fa = s.facts;
                        const char* none = f.source == "vcpkg" && fa.vcpkg_root.empty()   ? "vcpkg not found (VCPKG_ROOT)"
                                           : f.source == "rocm" && fa.rocm_root.empty()   ? "ROCm not found (ROCM_PATH)"
                                           : f.source == "pkg-config" && fa.pkgconfig.empty() ? "pkg-config not found"
                                                                                           : nullptr;
                        if (f.source == "pypi") none = nullptr;
                        if (!shown.empty()) box.put(font, y, value_col, cut(shown, right - value_col), 0, Font::kBold);
                        else if (!fa.done && f.source != "pypi") box.put(font, y, value_col, "looking\xE2\x80\xA6", kInkMuted);
                        else box.put(font, y, value_col, none ? none : "none (Enter looks them up)", none ? kInkDiagnostic : kInkMuted);
                    }
                    else if (val.empty()) box.put(font, y, value_col, "none", kInkMuted);
                    else box.put(font, y, value_col, cut(val, right - value_col), 0);
                    break;
                }
                default: break;
            }
        }
        const u32 lit = i == s.form_sel ? kCellSelected : 0;
        if (lit)
            for (u32 c = s.left; c < s.left + s.width && c < cols; ++c) box.cells[static_cast<size_t>(y) * cols + c] |= lit;
    }
    if (s.form_top > 0) box.put(font, top, right, "\xE2\x86\x91", kInkMuted);
    if (s.form_top + body < n) box.put(font, top + body - 1, right, "\xE2\x86\x93", kInkMuted);

    // Under it: what it writes, what it runs then, what went wrong, the keys.
    y = top + body + 1;
    const u32 text_col = s.left + 2 + 7, room = right > text_col ? right - text_col : 1;
    std::string list;
    for (const std::string& w : writes) list += (list.empty() ? "" : ", ") + w;
    box.put(font, y, s.left + 2, "Writes", kInkMuted);
    std::vector<std::string> lines;
    while (!list.empty() && lines.size() < 2) {
        if (width_of(list) <= room || lines.size() == 1) {
            lines.push_back(cut(list, room));
            break;
        }
        size_t at = 0, last_break = 0;
        for (u32 c = 0; c < room && at < list.size(); ++c) {
            if (list[at] == ' ') last_break = at;
            at += decode_utf8(reinterpret_cast<const u8*>(list.data()) + at, list.size() - at).len;
        }
        if (last_break == 0) last_break = at;
        lines.push_back(list.substr(0, last_break));
        list.erase(0, std::min(list.size(), last_break + 1));
    }
    for (size_t i = 0; i < lines.size(); ++i) box.put(font, y + static_cast<u32>(i), text_col, lines[i], 0);
    y += 2;
    box.put(font, y, s.left + 2, "Runs", kInkMuted);
    box.put(font, y, text_col, then.empty() ? std::string("nothing") : cut(then, room), then.empty() ? kInkMuted : 0);
    y += 2;
    if (!s.error.empty()) box.put(font, y, s.left + 2, cut(s.error, s.width - 2), kInkDiagnostic);
    const std::string keys = "\xE2\x86\x91\xE2\x86\x93 moves \xC2\xB7 \xE2\x86\x90\xE2\x86\x92 changes \xC2\xB7 Enter edits \xC2\xB7 " +
                             std::string(kPrimaryMod == SDL_KMOD_GUI ? "Cmd" : "Ctrl") + "+Enter creates \xC2\xB7 Esc back";
    const u32 kw = width_of(keys);
    if (y + 1 < rows) box.put(font, y + 1, cols > kw ? (cols - kw) / 2 : 0, cut(keys, cols), kInkMuted);
}

// What the New project tab holds, on one line (for the tests).
std::string App::project_dump() {
    StartUi& s = *start_;
    project_facts_apply();
    const ProjectKind* k = s.chosen();
    if (!k) return "no kinds";
    std::string out = "kind=" + k->name + " name=" + s.name + " where=" + home_short(s.where) + " git=" + (s.git ? "on" : "off") + " |";
    const ProjectValues& v = s.values[k->name];
    for (const StartUi::Row& r : s.form)
        if (r.what == kFormField) {
            const std::string& key = k->fields[r.field].key;
            const auto it = v.find(key);
            out += " " + key + "=" + (it == v.end() ? std::string() : it->second);
        }
    if (s.form_sel < s.form.size()) out += " | at " + std::to_string(s.form_sel);
    if (!s.error.empty()) out += " | error: " + s.error;
    if (!s.made.empty()) out += " | made " + home_short(s.made);
    return out;
}

// What it shows, on one line (for the tests): each item, the chosen one marked.
std::string App::start_dump() {
    if (!start_shown()) return "no start screen";
    StartUi& s = *start_;
    if (s.page == 1) return "new project: " + project_dump();
    std::string out = "start";
    for (size_t i = 0; i < s.items.size(); ++i) {
        const StartUi::Item& it = s.items[i];
        out += std::string(i == s.sel ? " >" : " ") + it.key + ":" + (it.act == kRecent ? home_short(it.recent.path) + (it.recent.dir ? "/" : "") : it.label);
    }
    return out;
}

}  // namespace blit
