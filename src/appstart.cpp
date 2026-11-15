// The start screen: what blit shows when it starts with no file to open.
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
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app.hpp"
#include "grammars.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

enum Act : u8 { kNew, kOpen, kFind, kSearch, kFolder, kRemote, kSettings, kQuit, kRecent };

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
    std::string root_seen;
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
    if (d.id != s.doc || !d.ed.path.empty() || d.ed.buf.size() > 0 || d.ed.modified() || panes_.size() != 1) s.on = false;  // (for good)
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
    if (b.button == SDL_BUTTON_LEFT)
        if (const int i = start_item_at(px, py); i >= 0) {
            start_->sel = static_cast<size_t>(i);
            start_do(static_cast<size_t>(i));
        }
    dirty_ = true;
    return true;
}

bool App::start_mouse_motion(const SDL_MouseMotionEvent& m) {
    if (!start_shown()) return false;
    const int px = static_cast<int>(m.x * density_), py = static_cast<int>(m.y * density_);
    if (pane_at(px, py) != pane_) return false;
    const int i = start_item_at(px, py);
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
    if (key != s.key || s.items.empty()) {
        s.key = key;
        const bool apple = kPrimaryMod == SDL_KMOD_GUI;
        const bool glyphs = apple && font.has(0x2318) && font.has(0x21E7);  // (⌘ and ⇧, or their names)
        const std::string cmd = glyphs ? "\xE2\x8C\x98" : apple ? "Cmd+" : "Ctrl+", shift = glyphs ? "\xE2\x87\xA7" : "Shift+";
        s.items.clear();
        s.items.push_back({kNew, "New file", "n", "", {}});
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
        if (need > rows && footer) footer = false, need -= 2;
        if (need > rows && tagline) tagline = false, need -= 2;
        while (need > rows && recent_room > 0) --recent_room, --need;
        s.items.resize(actions + recent_room);
        s.sel = std::min(s.sel, s.items.size() - 1);
        s.width = std::min<u32>(cols > 4 ? cols - 4 : cols, 64);
        s.left = (cols - s.width) / 2;
        s.box.resize(cols, rows);
        u32 r = rows > need ? (rows - need) / 2 : 0;
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

// What it shows, on one line (for the tests): each item, the chosen one marked.
std::string App::start_dump() {
    if (!start_shown()) return "no start screen";
    const StartUi& s = *start_;
    std::string out = "start";
    for (size_t i = 0; i < s.items.size(); ++i) {
        const StartUi::Item& it = s.items[i];
        out += std::string(i == s.sel ? " >" : " ") + it.key + ":" + (it.act == kRecent ? home_short(it.recent.path) + (it.recent.dir ? "/" : "") : it.label);
    }
    return out;
}

}  // namespace blit
