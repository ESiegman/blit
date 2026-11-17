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
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "tree.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

enum Act : u8 { kNew, kProject, kOpen, kFind, kSearch, kFolder, kRemote, kSettings, kQuit, kRecent };

// The New project tab's rows.
enum Form : u8 { kFormKind, kFormName, kFormWhere, kFormGit, kFormField, kFormCreate };

// The New project tab's sections, in the order they are shown. Each of a
// kind's fields goes in one by what it is about (by the keys project.cpp
// gives them); one about none of these (init.lua's own) goes under
// Options, right after the project's own rows.
enum Section : u8 { kSecProject, kSecOptions, kSecBuild, kSecChecks, kSecFiles, kSecAfter, kSections };
constexpr std::string_view kSectionNames[kSections] = {"Project", "Options", "Build", "Checks", "Files", "After"};

Section section_of(const ProjectField& f) {
    if (f.label.find("once made") != std::string::npos) return kSecAfter;  // (configuring, installing the packages)
    if (f.kind == ProjectField::kLibraries) return kSecBuild;
    static constexpr std::string_view checks[] = {"warnings", "sanitizers", "hardening", "tuning", "lto", "ruff", "types"};
    static constexpr std::string_view files[] = {"presets", "clangd", "tidy", "format", "readme", "profiling"};
    static constexpr std::string_view build[] = {"build",  "manager",    "type",        "target", "gpu",   "std",       "python",
                                                 "layout", "backend_uv", "backend_pip", "cli",    "tests", "generator", "install"};
    for (const std::string_view k : checks)
        if (f.key == k) return kSecChecks;
    for (const std::string_view k : files)
        if (f.key == k) return kSecFiles;
    for (const std::string_view k : build)
        if (f.key == k) return kSecBuild;
    return kSecOptions;
}

// What a field is for, on the line above the keys while its row is the
// chosen one: those of project.cpp's kinds by their keys, libraries by
// where they come from, and a choice of init.lua's by its choices.
std::string field_note(const ProjectField& f, Font& font) {
    if (f.kind == ProjectField::kLibraries) {
        const std::string enter = key_symbols("Enter", font);
        if (f.source == "vcpkg") return enter + " searches vcpkg's ports: each one chosen goes in vcpkg.json";
        if (f.source == "rocm") return enter + " searches the ROCm packages installed, linked to the GPU code";
        if (f.source == "pkg-config") return enter + " searches pkg-config's modules";
        if (f.source == "pypi") return enter + " searches PyPI's most downloaded packages, or any by its name";
        return enter + " searches them";
    }
    if (f.key == "install" && section_of(f) == kSecAfter) return "Makes its environment and installs into it once made (network)";
    static constexpr std::pair<std::string_view, std::string_view> notes[] = {
        {"build", "The build system that builds it"},
        {"manager", "What makes its environment and installs its packages"},
        {"type", "An application, a library, or a script of one file (PEP 723)"},
        {"target", "A program is a library and a small main, so its tests can link it"},
        {"gpu", "The GPUs to compile for: this machine's, from rocm_agent_enumerator"},
        {"std", "The language standard it is compiled as"},
        {"python", "The oldest Python it runs on"},
        {"layout", "Its package in src/, or at the top of the folder"},
        {"backend_uv", "What builds it into a package (pyproject.toml's build-system)"},
        {"backend_pip", "What builds it into a package (pyproject.toml's build-system)"},
        {"cli", "A command that runs it, and python -m"},
        {"tests", "A first test, and how the tests are run"},
        {"generator", "What CMake writes the build in"},
        {"install", "Rules for cmake --install: the library, its headers, a CMake package"},
        {"warnings", "How much the compiler warns of"},
        {"sanitizers", "Run-time checks built into the debug build"},
        {"hardening", "_FORTIFY_SOURCE and the stack protector in optimized builds"},
        {"tuning", "Release builds for any machine, or for this one alone (-march=native)"},
        {"lto", "Link-time optimization for release builds, where the compiler has it"},
        {"ruff", "Ruff, set up to lint and format it"},
        {"types", "The type checker it is set up for"},
        {"presets", "Presets for debug, release and ci (where warnings are errors)"},
        {"clangd", "So that clangd knows how each file is compiled"},
        {"tidy", "A .clang-tidy with bugprone, cert, performance and other checks"},
        {"format", "A .clang-format in this style"},
        {"readme", "A README.md with the commands that build and test it"},
        {"profiling", "scripts/profile.sh: the program profiled with rocprofv3"},
        {"configure", "Configures it in the Build tab's terminal once the files are written"},
    };
    for (const auto& [key, note] : notes)
        if (f.key == key) return std::string(note);
    std::string choices;
    if (f.kind == ProjectField::kChoice)
        for (const std::string& c : f.choices) choices += (choices.empty() ? "" : ", ") + c;
    return choices;
}

// The column both tabs are laid out in: kLookPageCols wide (less in a
// narrow window), in the middle, the same for both so that going from one
// to the other does not move it. A lit row's bar reaches a column further
// each way (see in_column).
void page_column(u32 cols, u32& left, u32& width) {
    width = std::min<u32>(kLookPageCols, cols > 4 ? cols - 4 : cols);
    left = (cols - width) / 2;
}

// A list ("a, b, c") in at most `most` lines of `room` columns, broken
// between its items; what does not fit is counted at the end ("+3 more").
std::vector<std::string> listed(const std::vector<std::string>& items, u32 room, size_t most) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < items.size() && lines.size() < most) {
        const bool last = lines.size() + 1 == most;
        std::string line;
        u32 w = 0;
        while (i < items.size()) {
            const u32 sep = line.empty() ? 0 : 2, iw = width_of(items[i]);
            // (Room kept for what ends the line: a comma, or the count of the rest.)
            const size_t after = items.size() - i - 1;
            const u32 tail = after == 0 ? 0 : last ? 2 + width_of("+" + std::to_string(after) + " more") : 1;
            if (w + sep + iw + tail > room) break;
            if (sep) line += ", ";
            line += items[i++];
            w += sep + iw;
        }
        if (line.empty() && !last) line = fit_text(items[i++], room > 1 ? room - 1 : room);  // (one longer than a line)
        if (i < items.size()) {
            if (last) line += (line.empty() ? "+" : ", +") + std::to_string(items.size() - i) + " more";
            else line += ",";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

// Text in at most `most` lines of `room` columns, broken at spaces; the
// last cut with an ellipsis if there is more still.
std::vector<std::string> wrapped(std::string_view text, u32 room, size_t most) {
    std::vector<std::string> lines;
    if (room == 0) return lines;
    while (!text.empty() && lines.size() < most) {
        if (width_of(text) <= room || lines.size() + 1 == most) {
            lines.push_back(fit_text(text, room));
            break;
        }
        const u8* p = reinterpret_cast<const u8*>(text.data());
        size_t at = 0, cut = std::string_view::npos;
        u32 w = 0;
        while (at < text.size()) {
            const Decoded d = decode_utf8(p + at, text.size() - at);
            const u32 dw = cp_width(d.cp);
            if (w + dw > room) break;
            if (d.cp == ' ') cut = at;
            w += dw;
            at += d.len ? d.len : 1;
        }
        if (cut == std::string_view::npos || cut == 0) cut = at > 0 ? at : std::max<size_t>(decode_utf8(p, text.size()).len, 1);  // (no space to break at)
        lines.emplace_back(text.substr(0, cut));
        text.remove_prefix(cut);
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    }
    return lines;
}

struct Recent {
    bool dir = false;
    std::string path;
    // Whether it is still there: asked of the file system once for each
    // change of the list (-1: not yet), not as the mouse moves.
    std::int8_t there = -1;
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

bool still_there(Recent& r) {
    if (r.there < 0) r.there = exists(r.path, r.dir) ? 1 : 0;
    return r.there > 0;
}

}  // namespace

struct App::StartUi {
    bool on = false;
    u64 doc = 0;  // the empty document it stands in front of
    std::vector<Recent> recent;  // the latest first (all that are kept; those still there are shown)
    // The lit rows as last put in the cells.
    size_t lit_sel = ~size_t{0};
    int lit_hover = -2;
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
    u32 left = 0, width = 0;  // the page's column (page_column): where its text starts, and how wide it is
    // A column a lit row takes: the page's, and one more each side.
    bool in_column(u32 col) const { return col + 1 >= left && col < left + width + 1; }
    // Its cells, and what they were made for.
    TextBox box;
    u64 key = 0;
    int x = 0, y = 0, pad_x = 0, pad_y = 0;
    bool folder_asked = false;  // the folder box was opened from here: the panel shows the folder chosen
    bool summoned = false;      // shown again later (the New project command), in a tab of its own
    std::string root_seen;      // the working directory as last seen ("" while there is none)
    bool root_known = false;
    u32 tabs_row = 0, tab_c[4] = {};  // the tabs Start and New project: their row, and their columns [c0, c1) each
    u32 create_c[2] = {};             // the Create button's columns [c0, c1)

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
    std::vector<Row> form;  // section by section (each field in its kind's order), Create last
    size_t form_sel = 0;
    size_t form_top = 0;  // the first line of the form shown, the sections' headings and the blank lines counted
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
    // Its list comes from a thread of its own (a download); one that failed is not tried again this session.
    std::mutex pypi_mu;
    std::vector<VcpkgPort> pypi_got;  // (pypi_mu) what the thread found
    bool pypi_ready = false;          // (pypi_mu) it has finished
    bool pypi_loading = false, pypi_failed = false, pypi_wanted = false;  // wanted: the list is to open when it comes
    std::vector<std::string> touched;  // "kind\tkey": set by hand, so what is found leaves it alone

    const ProjectKind* chosen() const { return kind < kinds.size() ? &kinds[kind] : nullptr; }
    ProjectValues& kind_values() { return values[kinds[kind].name]; }
    // The rows there are now (a field may be shown only for some values of
    // another), in the order they are shown: section by section.
    void rows_again() {
        std::vector<Row> rows = {{kFormKind}, {kFormName}, {kFormWhere}, {kFormGit}};
        if (const ProjectKind* k = chosen()) {
            const ProjectValues& v = values[k->name];
            for (u32 sec = kSecOptions; sec < kSections; ++sec)
                for (u32 i = 0; i < k->fields.size(); ++i)
                    if (section_of(k->fields[i]) == sec && k->fields[i].shown(v)) rows.push_back({kFormField, i});
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
    if (s.on) session_tabs({}, true);  // (the folders' tab counts, read while the window started)
}

void App::recent_add(const std::string& path, bool dir) {
    if (!start_ || path.empty() || restoring_) return;  // (the files a session brings back are not opened by hand)
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
    for (Recent& r : s.recent) r.there = -1;  // (the list has changed: each is asked about again when next wanted)
    s.key = 0;  // (shown afresh)
}

// The files (not folders) opened lately that are still there, the latest
// first, their full paths: Go to file shows them before anything is typed.
// Whether each is there comes from the start screen's answers, so the file
// system is asked once for each change of the list, not on every call.
std::vector<std::string> App::recent_files(size_t most) const {
    std::vector<std::string> out;
    if (!start_) return out;
    for (Recent& r : start_->recent) {
        if (out.size() >= most) break;
        if (!r.dir && still_there(r)) out.push_back(r.path);
    }
    return out;
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
                const bool back = project_open(it.recent.path);  // (its session, if it has one)
                s.sel = static_cast<size_t>(kRecent);  // (it is the first of those opened lately now; the actions come before it)
                if (!back && !panel_px()) panel_toggle();  // (the folder chosen, shown)
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
        const size_t n = s.form.size();  // (in the order shown, section by section: rows_again)
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
    if (kPrimaryMod == SDL_KMOD_GUI && (k.mod & SDL_KMOD_GUI) && !(k.mod & (SDL_KMOD_SHIFT | SDL_KMOD_ALT)) && k.key == SDLK_N) {  // (where Ctrl is the shortcut key, Ctrl+N moves down the list)  // (Cmd+N here is the page's own New file: the empty file it stands in front of)
        s.on = false;
        dirty_ = true;
        return true;
    }
    if ((k.mod & kPrimaryMod) && !(ctrl && (k.key == SDLK_N || k.key == SDLK_P))) return false;  // the application's shortcuts
    if (k.key == SDLK_GRAVE && ctrl && !(k.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) return false;  // and Ctrl+`, the terminals (appterm.cpp)
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
    if (!s.in_column(col)) return -1;
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
                if (s.form[i].row == row && s.in_column(col)) {
                    if (s.form[i].what == kFormCreate && (col < s.create_c[0] || col >= s.create_c[1])) break;  // (the button, not the row)
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
    // A folder chosen in the box opened from here: the side panel shows it.
    // (The working directory is looked at, not copied, frame by frame.)
    const std::string* root_now = panel_root();
    const std::string_view root = root_now ? std::string_view(*root_now) : std::string_view();
    if (s.folder_asked && s.root_known && root != s.root_seen && !find_open_) {
        s.folder_asked = false;
        if (!panel_px()) panel_toggle();
    }
    if (!s.root_known || root != s.root_seen) s.root_seen.assign(root), s.root_known = true;

    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
    mix(cols);
    mix(rows);
    mix(font.generation());
    mix(s.recent.size());
    mix(static_cast<u64>(s.page));
    mix(s.gen);
    mix(cfg_.modal);  // (the line saying it starts in Normal mode)
    mix(session_gen_);  // (the tabs each folder brings back)
    if (s.page == 1) {  // (what the machine has, once it is found)
        std::lock_guard lock(s.facts_mu);
        mix(s.facts.done);
    }
    bool rebuilt = false;
    if (s.page == 1 && (key != s.key || s.items.empty())) {
        s.key = key;
        s.box.resize(cols, rows);
        project_cells(cols, rows);
    } else if (key != s.key || s.items.empty()) {
        s.key = key;
        rebuilt = true;
        // Each thing to do, its key, and its shortcut from anywhere (as
        // the command search and the README write it, in the platform's
        // symbols).
        auto chord = [&](SDL_Keycode k, int mods) { return key_symbols(chord_text(k, static_cast<SDL_Keymod>(kPrimaryMod | mods)), font); };
        s.items.clear();
        s.items.push_back({kNew, "New file", "n", chord(SDLK_N, 0), {}});
        s.items.push_back({kProject, "New project\xE2\x80\xA6", "p", chord(SDLK_N, SDL_KMOD_ALT), {}});
        s.items.push_back({kOpen, "Open a file\xE2\x80\xA6", "o", chord(SDLK_O, 0), {}});
        s.items.push_back({kFind, "Go to file", "f", chord(SDLK_P, 0), {}});
        s.items.push_back({kSearch, "Search in files", "s", chord(SDLK_F, SDL_KMOD_SHIFT), {}});
        s.items.push_back({kFolder, "Open a folder\xE2\x80\xA6", "d", chord(SDLK_O, SDL_KMOD_SHIFT), {}});
        s.items.push_back({kRemote, "Connect to a server\xE2\x80\xA6", "r", "", {}});
        s.items.push_back({kSettings, "Settings (init.lua)", "c", chord(SDLK_COMMA, 0), {}});
        s.items.push_back({kQuit, "Quit", "q", chord(SDLK_Q, 0), {}});
        const size_t actions = s.items.size();
        u32 shown = 0;
        for (Recent& r : s.recent) {
            if (shown == 9) break;
            if (still_there(r)) s.items.push_back({kRecent, "", std::to_string(++shown), "", r});
        }

        // How it is laid out, top to bottom: the name, what it is, what to
        // do, what was opened lately; and the keys along the bottom of the
        // column. What goes when there is not room, the least useful first:
        // what it is, the name in large letters (it is written small), the
        // keys, and last the oldest of what was opened lately.
        bool logo = font.has(0x2588) && font.has(0x2580) && font.has(0x2584);
        bool tagline = true, hint = true;
        u32 recent_room = shown;
        auto need = [&] { return (logo ? 3 : 1) + (tagline ? 2 : 0) + 1 + static_cast<u32>(actions) + (recent_room ? recent_room + 2 : 0); };
        u32 room = rows > 6 ? rows - 6 : 0;  // (below the tabs and a blank row; above a blank row, the keys and a blank row)
        if (need() > room) tagline = false;
        if (need() > room) logo = false;
        if (need() > room) hint = false, room = rows > 4 ? rows - 4 : 0;
        while (need() > room && recent_room > 0) --recent_room;
        s.items.resize(actions + recent_room);
        s.sel = std::min(s.sel, s.items.size() - 1);
        page_column(cols, s.left, s.width);
        s.box.resize(cols, rows);
        u32 r = 3 + (room > need() ? (room - need()) / 2 : 0);
        auto centred = [&](u32 row, std::string_view text, Ink in) {
            const u32 n = width_of(text);
            put(s.box, font, row, cols > n ? (cols - n) / 2 : 0, text, in);
        };
        if (logo) {  // the name, in blocks of half a cell
            centred(r++, "█   █ ▀ ▄█▄", ink::kAccent);  // █   █ ▀ ▄█▄
            centred(r++, "█▀▄ █ █  █ ", ink::kAccent);    // █▀▄ █ █  █
            centred(r++, "█▄▀ █ █  █▄", ink::kAccent);  // █▄▀ █ █  █▄
        } else {
            centred(r++, "blit", ink::kAccentStrong);
        }
        if (tagline) {
            centred(r + 1, "a fast text editor", ink::kMuted);
            r += 2;
        }
        r += 1;
        // One left column for every row: the key that does it, then what it
        // is (a file or folder with its icon, as in the side panel), and the
        // shortcut against the right of the column; with too little room the
        // shortcuts go, and then the labels are cut.
        const u32 key_col = s.left, label_col = s.left + 3, right = s.left + s.width;
        u32 label_w = 0, cut_w = 0;
        for (size_t i = 0; i < actions; ++i) {
            label_w = std::max(label_w, width_of(s.items[i].label));
            cut_w = std::max(cut_w, width_of(s.items[i].shortcut));
        }
        const bool with_cuts = label_col + label_w + 3 + cut_w <= right;
        const u32 label_room = (with_cuts ? right - cut_w - 3 : right) - label_col;
        const bool nerd = font.has(0xE5FF) && font.has(0xE61D);  // the font brings file icons of its own
        auto icon = [&](u32 row, u32 col, const FileIcon& ic) {
            if (row >= rows || col + 2 > cols) return;
            u32* cell = s.box.cells.data() + static_cast<size_t>(row) * cols + col;
            if (nerd) {
                cell[0] = (font.glyph(ic.nerd) & (Font::kWide - 1)) | (ic.ink << kCellInkShift);
            } else {
                const u32 slot = font.icon(ic.drawn);
                cell[0] = slot | (ic.ink << kCellInkShift);
                cell[1] = (slot + 1) | (ic.ink << kCellInkShift);
            }
        };
        for (size_t i = 0; i < s.items.size(); ++i) {
            StartUi::Item& it = s.items[i];
            if (i == actions) {  // the heading of those opened lately
                put(s.box, font, r + 1, key_col, "Recent", ink::kHeading);
                r += 2;
            }
            it.row = r;
            put(s.box, font, r, key_col, it.key, ink::kAccent);
            if (it.act == kRecent) {
                // Its name, and the folder it is in (cut from its start if it is long).
                const std::string& path = it.recent.path;
                const size_t slash = path.find_last_of('/');
                std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
                if (name.empty()) name = path;
                std::string where = home_short(slash == std::string::npos ? std::string() : path.substr(0, std::max<size_t>(slash, 1)));
                if (const int tabs = it.recent.dir ? session_tabs(path) : 0; tabs > 0)  // (what opening it brings back)
                    where += " \xC2\xB7 " + std::to_string(tabs) + (tabs == 1 ? " tab" : " tabs");
                icon(r, label_col, file_icon(name, it.recent.dir, false));
                const u32 name_col = label_col + 3;
                const u32 end = put(s.box, font, r, name_col, fit_text(name, right > name_col ? right - name_col : 0), ink::kText);
                if (!where.empty() && end + 2 + 4 <= right) put(s.box, font, r, end + 2, fit_text(where, right - end - 2, true), ink::kMuted);
            } else {
                put(s.box, font, r, label_col, fit_text(it.label, label_room), ink::kText);
                if (with_cuts && !it.shortcut.empty()) put(s.box, font, r, right - width_of(it.shortcut), it.shortcut, ink::kMuted);
            }
            ++r;
        }
        if (hint) {  // the keys, as many as there is room for
            static constexpr Hint hints[] = {{"Enter", "open"}, {"j k", "move"}, {"Ctrl+Tab", "new project"}};
            size_t n = std::size(hints);
            while (n > 0 && hints_width(std::span(hints, n), font) > s.width) --n;
            put_hints(s.box, font, rows - 2, s.left, std::span(hints, n));
        }
    }
    if (s.page == 0 && s.box.cols == cols && !s.items.empty() && (rebuilt || s.lit_sel != s.sel || s.lit_hover != s.hover)) {  // the lit rows: the one chosen, the one under the mouse
        const u32 c0 = s.left > 0 ? s.left - 1 : 0, c1 = std::min(cols, s.left + s.width + 1);
        for (size_t i = 0; i < s.items.size(); ++i) {
            const u32 row = s.items[i].row;
            if (row >= rows) continue;
            const u32 lit = i == s.sel ? kCellSelected : static_cast<int>(i) == s.hover ? kCellHover : 0;
            for (u32 c = c0; c < c1; ++c) {
                u32& cell = s.box.cells[static_cast<size_t>(row) * cols + c];
                cell = (cell & ~(kCellSelected | kCellHover)) | lit;
            }
        }
        s.lit_sel = s.sel;
        s.lit_hover = s.hover;
    }
    if (s.box.cols == cols && rows > 2) {  // the tabs, along the top
        const char* const names[2] = {"Start", "New project"};
        const u32 total = width_of(names[0]) + 4 + width_of(names[1]);
        u32 c = cols > total ? (cols - total) / 2 : 0;
        s.tabs_row = 1;
        for (int t = 0; t < 2; ++t) {
            const u32 n = width_of(names[t]);
            for (u32 x = c > 0 ? c - 1 : 0; x < c + n + 1 && x < cols; ++x) s.box.cells[cols + x] &= ~(kCellSelected | kCellHover);
            put(s.box, font, 1, c, names[t], t == s.page ? ink::kAccentStrong : ink::kMuted);
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
        // A lit row is lit from the column before its text: its bar starts
        // inside that column, as far before the text as in a floating box
        // (the edge less the inset), or at the column's start if that is
        // nearer.
        q.pill_dx = static_cast<float>(std::max(0, cw - (lk.edge - lk.inset)));
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
            ev.user.code = 19;  // (the form takes what was found: start_sync)
            SDL_PushEvent(&ev);
        });
    }
    if (wait && s.facts_worker.joinable()) s.facts_worker.join();
    ++s.gen;
}

// Something a thread of the start screen has found: the facts become
// defaults, and PyPI's list opens if it was asked for.
void App::start_sync() {
    if (!start_) return;
    StartUi& s = *start_;
    project_facts_apply();
    std::vector<VcpkgPort> got;
    bool arrived = false;
    {
        std::lock_guard lock(s.pypi_mu);
        if (s.pypi_ready) {
            got = std::move(s.pypi_got);
            s.pypi_ready = false;
            arrived = true;
        }
    }
    if (arrived) {
        s.pypi_loading = false;
        if (got.empty()) s.pypi_failed = true;
        else s.pypi = std::move(got);
        if (s.pypi_wanted) {
            s.pypi_wanted = false;
            s.error.clear();
            if (start_shown()) project_libraries();
        }
    }
    ++s.gen;
    dirty_ = true;
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
        if (s.pypi.empty() && !s.pypi_failed) {  // (fetched on a thread: the list opens when it comes)
            if (!s.pypi_loading) {
                s.pypi_loading = true;
                std::thread([sp = start_, cache = grammar_dir() + "/cache"] {
                    std::vector<VcpkgPort> got = pypi_top(cache);
                    {
                        std::lock_guard lock(sp->pypi_mu);
                        sp->pypi_got = std::move(got);
                        sp->pypi_ready = true;
                    }
                    SDL_Event ev{};
                    ev.type = SDL_EVENT_USER;
                    ev.user.code = 19;
                    SDL_PushEvent(&ev);
                }).detach();
            }
            s.pypi_wanted = true;
            s.error = "Fetching PyPI's most downloaded packages\xE2\x80\xA6";
            s.key = 0;
            dirty_ = true;
            return;
        }
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
    if (!project_open(dir) && !panel_px()) panel_toggle();  // (the window's session is the new project's from here)
    if (!open.empty()) open_path(open.c_str());
    s.on = false;
    if (!then.empty()) build_command(dir, then, "Setting up " + s.name);
    build_changed();
    return true;
}

// The New project tab's cells (start_layout): the form, section by
// section; under it, what it writes and what it runs then; along the
// bottom, what the chosen row is for (or what went wrong), and the keys.
void App::project_cells(u32 cols, u32 rows) {
    StartUi& s = *start_;
    project_facts_apply();
    Font& font = *font_;
    TextBox& box = s.box;
    page_column(cols, s.left, s.width);
    const ProjectKind* k = s.chosen();
    if (!k) {
        put(box, font, 3, s.left, "No kinds of project (init.lua removed them all).", ink::kMuted);
        return;
    }
    const ProjectValues& v = s.values[k->name];
    const bool dots = font.has(0x25CF) && font.has(0x25CB), quotes = font.has(0x2039) && font.has(0x203A);
    const std::string enter = key_symbols("Enter", font), create_keys = chord_text(SDLK_RETURN, kPrimaryMod), create = key_symbols(create_keys, font);
    auto label_of = [&](const StartUi::Row& r) -> std::string {
        switch (r.what) {
            case kFormKind: return "Kind";
            case kFormName: return "Name";
            case kFormWhere: return "Location";
            case kFormGit: return "Git repository";
            case kFormField: return k->fields[r.field].label;
            case kFormCreate: return {};
        }
        return {};
    };
    auto section_of_row = [&](const StartUi::Row& r) -> int {
        return r.what == kFormCreate ? -1 : r.what == kFormField ? static_cast<int>(section_of(k->fields[r.field])) : static_cast<int>(kSecProject);
    };

    // The columns: the labels from the page's left edge (taking a little
    // over half of it at most), then one column where every value starts,
    // after room for the chooser's mark, so that no value moves when its
    // row is chosen.
    const u32 right = s.left + s.width;
    u32 label_w = 0;
    for (const StartUi::Row& r : s.form) label_w = std::max(label_w, width_of(label_of(r)));
    label_w = std::min(label_w, s.width * 11 / 20);
    const u32 chooser_col = s.left + label_w + 2, value_col = chooser_col + 2;
    const u32 value_room = right > value_col ? right - value_col : 0;
    // A value that Left and Right change is between ‹ and ›, on the chosen row alone.
    const std::string_view lq = quotes ? "\xE2\x80\xB9" : "<", rq = quotes ? "\xE2\x80\xBA" : ">";
    auto marks = [&](u32 row, u32 end) {
        put(box, font, row, chooser_col, lq, ink::kMuted);
        put(box, font, row, end + 1, rq, ink::kMuted);
    };
    auto put_switch = [&](u32 row, bool on, bool chosen) {
        const u32 end = put(box, font, row, value_col, dots ? (on ? "\xE2\x97\x8F on" : "\xE2\x97\x8B off") : (on ? "[x] on" : "[ ] off"), on ? ink::kText : ink::kMuted);
        if (chosen) marks(row, end);
    };
    auto put_choice = [&](u32 row, const std::string& value, bool chosen) {
        const u32 end = put(box, font, row, value_col, fit_text(value, value_room > 2 ? value_room - 2 : 0), ink::kText);
        if (chosen) marks(row, end);
    };

    // What will be made, for the lines under the form: the files listed
    // between their names (and counted past two lines), the commands broken
    // between words.
    std::vector<std::string> writes;
    std::string then;
    if (!k->scripted) {
        ProjectValues full = v;
        full["name"] = s.name.empty() ? "name" : s.name;
        full["git"] = s.git ? "true" : "false";
        const ProjectPlan plan = k->plan(full);
        for (const ProjectFile& f : plan.files) writes.push_back(f.path);
        then = plan.then;
    }
    if (s.git) then = "git init -q" + (then.empty() ? std::string() : "; " + then);
    const u32 text_col = s.left + 8, text_room = right > text_col ? right - text_col : 1;
    const std::vector<std::string> write_lines = k->scripted ? std::vector<std::string>{"What init.lua's create function writes"} : listed(writes, text_room, 2);
    const std::vector<std::string> run_lines = wrapped(then, text_room, 2);
    const u32 summary_h = static_cast<u32>(std::max<size_t>(write_lines.size(), 1) + std::max<size_t>(run_lines.size(), 1));

    // Where it goes: the form under the tabs, then a blank row and what it
    // writes and runs; along the bottom a blank row, the line about the
    // chosen row, the keys, and a blank row under them. In a window too
    // short for that, what it writes and runs goes first, then the bottom.
    const u32 top = 3;
    const bool foot = rows >= top + 9;
    const u32 msg_row = rows - 3, hint_row = rows - 2;
    const u32 end = foot ? msg_row - 1 : rows;  // (the form and what is under it stay above this row)
    bool summary = true;
    u32 body = end > top + 1 + summary_h ? end - top - 1 - summary_h : 0;
    if (body < 4) summary = false, body = end > top ? end - top : 1;

    // The form's lines: each section's heading over its rows, a blank line
    // between sections, and one before Create.
    enum : u8 { kLineBlank, kLineHeading, kLineRow };
    struct Line {
        u8 what;
        u32 at;  // kLineHeading: the section; kLineRow: the row of the form
    };
    std::vector<Line> lines;
    lines.reserve(s.form.size() + 2 * kSections + 2);
    size_t sel_line = 0;
    int section = -2;
    for (size_t i = 0; i < s.form.size(); ++i) {
        if (const int sec = section_of_row(s.form[i]); sec != section) {
            if (!lines.empty()) lines.push_back({kLineBlank, 0});
            if (sec >= 0) lines.push_back({kLineHeading, static_cast<u32>(sec)});
            section = sec;
        }
        if (i == s.form_sel) sel_line = lines.size();
        lines.push_back({kLineRow, static_cast<u32>(i)});
    }
    // As many lines as fit, the chosen row's always (and its section's
    // heading, when that is just above it).
    const size_t n = lines.size();
    const size_t first = sel_line > 0 && lines[sel_line - 1].what == kLineHeading ? sel_line - 1 : sel_line;
    if (first < s.form_top) s.form_top = first;
    if (sel_line >= s.form_top + body) s.form_top = sel_line - body + 1;
    if (s.form_top + body > n) s.form_top = n > body ? n - body : 0;
    for (StartUi::Row& r : s.form) r.row = ~0u;
    const u32 lit0 = s.left > 0 ? s.left - 1 : 0, lit1 = std::min(cols, right + 1);
    u32 y = top;
    for (size_t li = s.form_top; li < n && y < top + body; ++li, ++y) {
        const Line& line = lines[li];
        if (line.what == kLineHeading) put(box, font, y, s.left, kSectionNames[line.at], ink::kHeading);
        if (line.what != kLineRow) continue;
        const size_t i = line.at;
        StartUi::Row& r = s.form[i];
        r.row = y;
        const bool chosen = i == s.form_sel;
        if (r.what == kFormCreate) {  // a button, in the value column, on a bar of its own: lit when it is chosen
            u32 c = put(box, font, y, value_col, "Create", ink::kAccentStrong);
            c = put(box, font, y, c + 2, create, ink::kAccent);
            s.create_c[0] = value_col - 1;
            s.create_c[1] = std::min(cols, c + 1);
            for (u32 x = s.create_c[0]; x < s.create_c[1]; ++x) box.cells[static_cast<size_t>(y) * cols + x] |= chosen ? kCellSelected : kCellHover;
            continue;
        }
        put(box, font, y, s.left, fit_text(label_of(r), label_w), ink::kText);
        switch (r.what) {
            case kFormKind: put_choice(y, k->name, chosen); break;
            case kFormName:
                if (s.name.empty()) put(box, font, y, value_col, "Type a name", ink::kMuted);
                else put(box, font, y, value_col, fit_text(s.name, value_room), ink::kText);
                break;
            case kFormWhere: put(box, font, y, value_col, fit_text(home_short(s.where), value_room, true), ink::kText); break;
            case kFormGit: put_switch(y, s.git, chosen); break;
            case kFormField: {
                const ProjectField& f = k->fields[r.field];
                const auto it = v.find(f.key);
                const std::string val = it == v.end() ? std::string() : it->second;
                if (f.kind == ProjectField::kSwitch) {
                    put_switch(y, val == "true", chosen);
                } else if (f.kind == ProjectField::kChoice) {
                    put_choice(y, val, chosen);
                } else if (f.kind == ProjectField::kLibraries) {
                    std::string names;
                    for (const char c : val) names += c == ' ' ? std::string(", ") : std::string(1, c);
                    std::lock_guard lock(s.facts_mu);
                    const StartUi::Facts& fa = s.facts;
                    const char* none = f.source == "vcpkg" && fa.vcpkg_root.empty()       ? "vcpkg not found (VCPKG_ROOT)"
                                       : f.source == "rocm" && fa.rocm_root.empty()       ? "ROCm not found (ROCM_PATH)"
                                       : f.source == "pkg-config" && fa.pkgconfig.empty() ? "pkg-config not found"
                                                                                           : nullptr;
                    if (f.source == "pypi") none = nullptr;
                    if (!names.empty()) put(box, font, y, value_col, fit_text(names, value_room), ink::kText);
                    else if (!fa.done && f.source != "pypi") put(box, font, y, value_col, "Looking\xE2\x80\xA6", ink::kMuted);
                    else if (none) put(box, font, y, value_col, fit_text(none, value_room), ink::kError);
                    else put(box, font, y, value_col, fit_text("None (" + enter + " looks them up)", value_room), ink::kMuted);
                } else if (val.empty()) {
                    put(box, font, y, value_col, "None", ink::kMuted);
                } else {
                    put(box, font, y, value_col, fit_text(val, value_room), ink::kText);
                }
                break;
            }
            default: break;
        }
        if (chosen)
            for (u32 c = lit0; c < lit1; ++c) box.cells[static_cast<size_t>(y) * cols + c] |= kCellSelected;
    }
    if (right < cols) {  // (more above, more below: in the margin, past the values)
        if (s.form_top > 0) put(box, font, top, right, "\xE2\x86\x91", ink::kMuted);
        if (s.form_top + body < n) put(box, font, top + body - 1, right, "\xE2\x86\x93", ink::kMuted);
    }

    // Under the form, after a blank row: what it writes, what it runs then.
    if (summary) {
        y += 1;
        put(box, font, y, s.left, "Writes", ink::kMuted);
        if (write_lines.empty()) put(box, font, y++, text_col, "Nothing", ink::kMuted);
        for (const std::string& line : write_lines) put(box, font, y++, text_col, line, k->scripted ? ink::kMuted : ink::kText);
        put(box, font, y, s.left, "Runs", ink::kMuted);
        if (run_lines.empty()) put(box, font, y, text_col, "Nothing", ink::kMuted);
        for (const std::string& line : run_lines) put(box, font, y++, text_col, line, ink::kText);
    }

    if (!foot) return;
    // What the chosen row is for, in full (or what went wrong, or what is
    // being waited for), then the keys, as many as there is room for.
    if (!s.error.empty()) {
        const bool waiting = s.pypi_loading && s.pypi_wanted;  // (PyPI's list on its way: not a problem)
        put(box, font, msg_row, s.left, fit_text(s.error, s.width), waiting ? ink::kMuted : ink::kError);
    } else if (s.form_sel < s.form.size()) {
        const StartUi::Row& r = s.form[s.form_sel];
        std::string note;
        switch (r.what) {
            case kFormKind: note = k->description; break;
            case kFormName: note = "The name of its folder and targets: letters, digits, '-', '_' and '.'"; break;
            case kFormWhere: note = "The folder it goes in; ssh://host/\xE2\x80\xA6 makes it on another machine"; break;
            case kFormGit: note = "Runs git init once the files are written, with a .gitignore"; break;
            case kFormField: note = field_note(k->fields[r.field], font); break;
            case kFormCreate: note = "Makes " + home_short(s.where + "/" + (s.name.empty() ? std::string("\xE2\x80\xA6") : s.name)) + " and opens its main file"; break;
        }
        put(box, font, msg_row, s.left, fit_text(note, s.width), ink::kMuted);
    }
    const Hint hints[] = {{"Up Down", "move"}, {"Left Right", "change"}, {"Enter", "edit"}, {create_keys, "create"}, {"Esc", "back"}};
    size_t shown = std::size(hints);
    while (shown > 0 && hints_width(std::span(hints, shown), font) > s.width) --shown;
    put_hints(box, font, hint_row, s.left, std::span(hints, shown));
}

// What the machine the project goes on was found to have (for the tests).
std::string App::project_facts_dump() {
    StartUi& s = *start_;
    if (s.kinds.empty()) project_page(true);
    project_facts(true);
    const StartUi::Facts& f = s.facts;
    return "vcpkg=" + home_short(f.vcpkg_root) + " ports=" + std::to_string(f.vcpkg_ports.size()) + " baseline=" + f.vcpkg_baseline.substr(0, 10) +
           " rocm=" + f.rocm_root + " gpus=" + f.gpus + " uv=" + f.uv_version + " pkg-config=" + std::to_string(f.pkgconfig.size());
}

// What the New project tab holds, on one line (for the tests): the fields
// shown, in their kind's order (not the sections' order the form has),
// and which row of the form is chosen.
std::string App::project_dump() {
    StartUi& s = *start_;
    project_facts_apply();
    const ProjectKind* k = s.chosen();
    if (!k) return "no kinds";
    std::string out = "kind=" + k->name + " name=" + s.name + " where=" + home_short(s.where) + " git=" + (s.git ? "on" : "off") + " |";
    const ProjectValues& v = s.values[k->name];
    for (const ProjectField& f : k->fields)
        if (f.shown(v)) {
            const auto it = v.find(f.key);
            out += " " + f.key + "=" + (it == v.end() ? std::string() : it->second);
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
        if (const int tabs = it.act == kRecent && it.recent.dir ? session_tabs(it.recent.path) : 0; tabs > 0) out += "(" + std::to_string(tabs) + ")";
    }
    return out;
}

}  // namespace blit
