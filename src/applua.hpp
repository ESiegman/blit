#pragma once
// What the two halves of the scripting share (src/applua.cpp runs the
// scripts, src/luaapi.cpp is what they can call): small helpers, the
// events, and the state kept for the scripts. Only those two include it.
#include <dirent.h>
#include <sys/stat.h>

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "remote.hpp"
#include "app.hpp"
#include "build.hpp"
#include "finder.hpp"
#include "job.hpp"
#include "keymap.hpp"
#include "project.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

// (Helpers of these two files alone: in a namespace of their own, so that
// each file may use some of them and not others.)
namespace script {

// The events handlers can be for, by Neovim's names for them.
enum Event : u32 {
    kEvBufReadPost, kEvBufNewFile, kEvBufEnter, kEvBufLeave, kEvBufWritePre, kEvBufWritePost, kEvBufDelete, kEvFileType,
    kEvModeChanged, kEvInsertEnter, kEvInsertLeave, kEvTextChanged, kEvTextChangedI, kEvCursorMoved, kEvCursorMovedI,
    kEvVimEnter, kEvVimLeavePre, kEvFocusGained, kEvFocusLost, kEvDirChanged, kEvUser, kEvCount
};
constexpr const char* kEventNames[kEvCount] = {
    "BufReadPost", "BufNewFile", "BufEnter", "BufLeave", "BufWritePre", "BufWritePost", "BufDelete", "FileType",
    "ModeChanged", "InsertEnter", "InsertLeave", "TextChanged", "TextChangedI", "CursorMoved", "CursorMovedI",
    "VimEnter", "VimLeavePre", "FocusGained", "FocusLost", "DirChanged", "User"};
constexpr u32 bit(u32 e) { return 1u << e; }
// The ones that are noticed by looking (after each batch of input) rather than told.
constexpr u32 kWatched = bit(kEvBufEnter) | bit(kEvBufLeave) | bit(kEvModeChanged) | bit(kEvInsertEnter) | bit(kEvInsertLeave) |
                         bit(kEvTextChanged) | bit(kEvTextChangedI) | bit(kEvCursorMoved) | bit(kEvCursorMovedI);

inline bool same_word(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<u8>(a[i])) != std::tolower(static_cast<u8>(b[i]))) return false;
    return true;
}

inline int event_of(std::string_view name) {
    if (same_word(name, "BufRead")) return kEvBufReadPost;
    if (same_word(name, "BufWrite")) return kEvBufWritePre;
    if (same_word(name, "BufNew")) return kEvBufNewFile;
    for (u32 e = 0; e < kEvCount; ++e)
        if (same_word(name, kEventNames[e])) return static_cast<int>(e);
    return -1;
}

// Where floating boxes go, and what shuts them.
enum : int { kAtCursor, kAtCenter, kAtEditor, kAtTopRight, kAtBottomRight };
enum : int { kCloseEsc, kCloseMove, kCloseNever };
enum : int { kInfo, kWarn, kError };

inline i64 stamp_of(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<i64>(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
    return static_cast<i64>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
}

// "file", "dir", or nothing.
// (A path on another machine is asked of it, when it is connected; scripts run on the main thread, which does not wait for that.)
inline const char* kind_of(const std::string& path) {
    if (is_remote(path)) {
        std::string there;
        RemoteStat rs;
        const std::shared_ptr<Remote> r = remote_for(path, &there);
        if (!r || !r->stat(there, &rs) || rs.kind == 0) return nullptr;
        return rs.kind == 2 ? "dir" : "file";
    }
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return nullptr;
    return S_ISDIR(st.st_mode) ? "dir" : "file";
}

struct DirEntry {
    std::string name;
    bool dir = false;
};
inline std::vector<DirEntry> list_dir(const std::string& path) {
    std::vector<DirEntry> out;
    if (is_remote(path)) {
        std::string there;
        std::vector<RemoteEntry> list;
        if (const std::shared_ptr<Remote> r = remote_for(path, &there); r && r->list(there, &list))
            for (RemoteEntry& e : list) out.push_back({std::move(e.name), e.dir});
        return out;
    }
    if (DIR* d = ::opendir(path.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const char* name = e->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            bool dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) dir = kind_of(path + "/" + name) == std::string_view("dir");
            out.push_back({name, dir});
        }
        ::closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return out;
}

inline std::string base_name(std::string_view path) {
    const size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// A pattern of an event's: globs, separated by commas; one without a slash
// is matched against the file's name, one with a slash against its path.
inline bool pattern_matches(std::string_view pattern, std::string_view text) {
    if (pattern.empty() || pattern == "*") return true;
    for (size_t at = 0; at <= pattern.size();) {
        const size_t comma = std::min(pattern.find(',', at), pattern.size());
        const std::string_view one = pattern.substr(at, comma - at);
        if (!one.empty()) {
            const bool whole = one.find('/') != std::string_view::npos;
            const std::string name = whole ? std::string(text) : base_name(text);
            if (Finder::glob(one.starts_with("**/") || !whole || one.starts_with("/") ? one : std::string_view(), name) ||
                (whole && Finder::glob(one, text)) || Finder::glob(std::string("**/") + std::string(one), text))
                return true;
        }
        at = comma + 1;
    }
    return false;
}

// What went wrong in the function Lua called, for api<>() to raise once
// that function has returned.
inline char g_err[1024];
inline int fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

inline bool is_str(lua_State* L, int i) { return lua_type(L, i) == LUA_TSTRING; }
inline std::string_view str_at(lua_State* L, int i) {
    size_t n = 0;
    const char* s = lua_tolstring(L, i, &n);
    return s ? std::string_view(s, n) : std::string_view();
}
inline bool int_at(lua_State* L, int i, i64* out) {
    int ok = 0;
    const lua_Integer v = lua_tointegerx(L, i, &ok);
    if (ok) *out = static_cast<i64>(v);
    return ok;
}
inline bool opt_int(lua_State* L, int i, i64 def, i64* out) {
    if (lua_isnoneornil(L, i)) {
        *out = def;
        return true;
    }
    return int_at(L, i, out);
}
// A field of a table, without its metamethods (which could raise errors):
// pushes it and returns its type.
inline int field(lua_State* L, int t, const char* name) {
    t = lua_absindex(L, t);
    if (!lua_istable(L, t)) {
        lua_pushnil(L);
        return LUA_TNIL;
    }
    lua_pushstring(L, name);
    return lua_rawget(L, t);
}
inline std::string field_str(lua_State* L, int t, const char* name, std::string def = {}) {
    if (field(L, t, name) == LUA_TSTRING) def = std::string(str_at(L, -1));
    lua_pop(L, 1);
    return def;
}
inline i64 field_int(lua_State* L, int t, const char* name, i64 def) {
    if (field(L, t, name) == LUA_TNUMBER) int_at(L, -1, &def);
    lua_pop(L, 1);
    return def;
}
inline bool field_bool(lua_State* L, int t, const char* name, bool def) {
    if (field(L, t, name) == LUA_TBOOLEAN) def = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return def;
}
// The strings of an array (other values are turned into strings as tostring would not: they are left out).
inline std::vector<std::string> strings_at(lua_State* L, int t) {
    std::vector<std::string> out;
    if (!lua_istable(L, t)) return out;
    t = lua_absindex(L, t);
    const lua_Unsigned n = lua_rawlen(L, t);
    for (lua_Unsigned i = 1; i <= n; ++i) {
        const int type = lua_rawgeti(L, t, static_cast<lua_Integer>(i));
        if (type == LUA_TSTRING || type == LUA_TNUMBER) out.emplace_back(str_at(L, -1));
        lua_pop(L, 1);
    }
    return out;
}
// Tasks from a list: {name =, cmd =, cwd =, default =} each, or a command alone.
inline std::vector<BuildTask> tasks_at(lua_State* L, int t, const std::string& system) {
    std::vector<BuildTask> out;
    if (!lua_istable(L, t)) return out;
    t = lua_absindex(L, t);
    const lua_Unsigned n = lua_rawlen(L, t);
    for (lua_Unsigned i = 1; i <= n; ++i) {
        const int type = lua_rawgeti(L, t, static_cast<lua_Integer>(i));
        BuildTask task;
        task.system = system;
        if (type == LUA_TSTRING) {
            task.name = task.cmd = std::string(str_at(L, -1));
        } else if (type == LUA_TTABLE) {
            task.cmd = field_str(L, -1, "cmd");
            task.name = field_str(L, -1, "name", task.cmd);
            task.cwd = field_str(L, -1, "cwd");
            task.def = field_bool(L, -1, "default", false);
        }
        lua_pop(L, 1);
        if (!task.cmd.empty()) out.push_back(std::move(task));
    }
    return out;
}
inline std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    for (size_t at = 0;;) {
        const size_t nl = s.find('\n', at);
        out.emplace_back(s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        if (nl == std::string_view::npos) break;
        at = nl + 1;
    }
    return out;
}

inline u32 ink_named(std::string_view name) {
    if (name == "muted") return kInkMuted;
    if (name == "accent") return kInkAccent;
    if (name == "error") return kInkDiagnostic;
    if (name == "warning" || name == "warn") return kInkDiagnostic + 1;
    if (name == "info") return kInkDiagnostic + 2;
    if (name == "hint") return kInkDiagnostic + 3;
    if (name == "added") return kInkAdded;
    if (name == "changed") return kInkChanged;
    return 0;
}

// A Lua value as TOML writes it, for a setting: true, 14, "text", ["a", "b"].
inline bool toml_of(lua_State* L, int i, std::string* out) {
    switch (lua_type(L, i)) {
        case LUA_TBOOLEAN: *out = lua_toboolean(L, i) ? "true" : "false"; return true;
        case LUA_TNUMBER: {
            char buf[64];
            if (lua_isinteger(L, i)) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(lua_tointeger(L, i)));
            else std::snprintf(buf, sizeof buf, "%.17g", lua_tonumber(L, i));
            *out = buf;
            return true;
        }
        case LUA_TSTRING: *out = toml_quote(str_at(L, i)); return true;
        case LUA_TTABLE: {
            *out = "[";
            const lua_Unsigned n = lua_rawlen(L, i);
            for (lua_Unsigned k = 1; k <= n; ++k) {
                const int t = lua_rawgeti(L, i, static_cast<lua_Integer>(k));
                const bool ok = t == LUA_TSTRING;
                if (ok) *out += (k > 1 ? ", " : "") + toml_quote(str_at(L, -1));
                lua_pop(L, 1);
                if (!ok) return false;
            }
            *out += "]";
            return true;
        }
        default: return false;
    }
}

inline void push_value(lua_State* L, const ConfigValue& v) {
    switch (v.kind) {
        case ConfigValue::kBool: lua_pushboolean(L, v.flag); break;
        case ConfigValue::kNumber:
            if (v.number == std::floor(v.number) && std::fabs(v.number) < 9e15) lua_pushinteger(L, static_cast<lua_Integer>(v.number));
            else lua_pushnumber(L, v.number);
            break;
        case ConfigValue::kText: lua_pushlstring(L, v.text.data(), v.text.size()); break;
        case ConfigValue::kList:
            lua_createtable(L, static_cast<int>(v.list.size()), 0);
            for (size_t k = 0; k < v.list.size(); ++k) {
                lua_pushlstring(L, v.list[k].data(), v.list[k].size());
                lua_rawseti(L, -2, static_cast<lua_Integer>(k + 1));
            }
            break;
        default: lua_pushnil(L); break;
    }
}

// Vim's short names for a few settings, for :set.
const struct {
    const char* vim;
    const char* key;
} kVimNames[] = {{"ts", "editor.tab_width"},      {"tabstop", "editor.tab_width"},     {"sw", "editor.indent_width"},
                 {"shiftwidth", "editor.indent_width"}, {"nu", "editor.line_numbers"}, {"number", "editor.line_numbers"},
                 {"rnu", "editor.relative_numbers"}, {"relativenumber", "editor.relative_numbers"},
                 {"tm", "keys.timeout_ms"},       {"timeoutlen", "keys.timeout_ms"},   {"guifont", "font.family"},
                 {"mapleader", "keys.leader"},    {"modal", "editor.modal"}};


}  // namespace script
using namespace script;

// ---------------------------------------------------------------- state

// A key held back while it may be the start of a mapping.
struct ScriptKey {
    MapKey key;
    bool down = false;       // a key press (else typed text)
    SDL_KeyboardEvent ev{};  // down: the press
    std::string text;        // the text: typed, or what the press typed too (Option and a letter)
};

struct App::ScriptUi {
    lua_State* L = nullptr;
    int api = LUA_NOREF;   // the table `blit`, in the registry
    int depth = 0;         // calls into Lua under way, one inside another
    u64 deadline = 0;      // when the outermost of them is stopped (0: none is running)
    bool starting = false; // init.lua is running before the window is there: settings go straight in
    bool reload = false;   // a fresh start is wanted, once nothing is running
    bool announce = false; // and it is for init.lua saved: the bottom line says how it went
    u32 errors = 0;        // errors shown so far (one while init.lua ran again: it said which line)
    std::string config_dir, data_dir, init_path;
    i64 init_stamp = 0;

    // Keys: the mappings, the keys typed so far that may be the start of
    // one, and the mapping they already are (when they could also be the
    // start of a longer one).
    Keymaps maps;
    using Pending = ScriptKey;
    std::vector<Pending> pending;
    Keymaps::Map exact;
    bool has_exact = false;
    size_t exact_len = 0;
    u64 pending_until = 0;
    int unmapped = 0;        // keys being put through as they are, not looked at again
    int feeding = 0;         // how deep mappings that type mappings have gone
    int expanding = 0;       // how deep commands that run commands have gone
    u32 count = 0;           // the count typed before the mapping being run

    // Commands, events, timers, programs.
    struct Command {
        std::string name, ex, desc, nargs = "*";
        int fn = LUA_NOREF;
        bool bang = false, range = false;
        std::function<void(const std::string& args)> native;  // blit's own (:PlugInstall ...)
    };
    std::vector<Command> commands;
    struct Handler {
        u64 id = 0;
        u32 events = 0;
        std::string pattern, group;
        int fn = LUA_NOREF;
        bool once = false;
    };
    std::vector<Handler> handlers;
    u64 next_handler = 1;
    u32 mask = 0;  // the events some handler is for
    struct Queued {
        u32 event = 0;
        u64 doc = 0;
        std::string file, match;
    };
    std::vector<Queued> queue;
    bool seen = false;  // what follows was looked at (it is noticed when it changes)
    u64 seen_doc = 0, seen_rev = 0, seen_rev_i = 0, seen_cursor = 0;
    char seen_mode = 'n';
    struct Timer {
        u64 id = 0, due = 0, every = 0;
        int fn = LUA_NOREF;
    };
    std::vector<Timer> timers;
    u64 next_timer = 1;
    std::unique_ptr<Jobs> jobs;
    struct JobCalls {
        int out = LUA_NOREF, err = LUA_NOREF, exit = LUA_NOREF;
    };
    std::unordered_map<u64, JobCalls> job_calls;
    // Programs blit itself started (git, for the plugins): what to do when each ends.
    std::unordered_map<u64, std::function<void(int code, const std::string& out, const std::string& err)>> native_jobs;
    // blit.opt's tables, one for each section, by its name ("" for blit.opt itself).
    int opt_sections = LUA_NOREF;
    // Plugins: those blit.plug() names, and those loaded.
    struct Plugin {
        std::string name, url, rev;
    };
    std::vector<Plugin> declared;
    std::vector<std::string> loaded;
    std::vector<JobEvent> backlog;  // what other jobs said while one was being waited for

    // What scripts show.
    struct Float {
        int id = 0;
        std::vector<std::string> lines;
        std::string title;
        int at = kAtCursor;
        int row = 0, col = 0;       // kAtEditor: cells from the top left of the window
        u32 width = 0, height = 0;  // 0: what the text needs
        int close = kCloseEsc;
        u32 ink = 0;
        u64 doc = 0, cursor = 0;  // where the cursor was when it opened
        u64 timeout_ns = 0, until = 0;
        TextBox box;
        u32 font = ~0u, made_cols = 0, made_rows = 0;
        bool dirty = true;
    };
    std::vector<Float> floats;
    int next_float = 1;
    struct Toast {
        std::string text;
        int level = kInfo;
        u64 until = 0;  // set when it is first drawn
        TextBox box;
        u32 font = ~0u;
    };
    std::vector<Toast> toasts;
    std::vector<std::string> messages;
    struct Mark {
        u32 ns = 0;
        u64 line = 0;
        u32 col0 = 0, col1 = 0;  // bytes in the line, [col0, col1)
        u32 kind = 0;            // a GridDeco kind, 0: none (a note alone)
        std::string note;
        u32 note_ink = kInkMuted;
        std::vector<u32> cells;
        u32 font = ~0u;
    };
    std::unordered_map<u64, std::vector<Mark>> marks;  // by line, once drawn (see marks_unsorted)
    std::unordered_set<u64> marks_unsorted;             // documents whose marks were added out of order since
    std::vector<std::string> namespaces;
    std::vector<GridDeco> view_decos[kMaxViews];
    std::vector<GridNote> view_notes[kMaxViews];

    // Tab on a ":set" line (set_complete): the line it made, what it chose
    // among, which of them, and what came before the word completed.
    std::string set_made, set_head;
    std::vector<std::string> set_options;
    size_t set_at = 0;

    // The finder box, asked for by blit.ui.select or blit.ui.input.
    int pick_fn = LUA_NOREF, pick_items = LUA_NOREF;
    u64 writing = 0;  // the document whose write handlers are running (a save they make fires none)
    // Documents a script call has changed: its changes there are one undo step, closed when it returns.
    std::vector<u64> grouped;
    // What init.lua asked for that needs the window (commands, keys, files to open): done once blit has started.
    std::vector<std::pair<char, std::string>> later;

    // What init.lua adds to the Build tab (blit.build.add) and to the start
    // screen's New project tab (blit.project.add), and the built-in ones it
    // took away.
    struct BuildHook {
        std::string name;
        std::vector<std::string> files;  // found when any of these is in the folder,
        int detect = LUA_NOREF;          // or when detect(root, files) says so
        std::vector<BuildTask> tasks;    // these, or what tasks(root, files) gives
        int tasks_fn = LUA_NOREF;
        bool first = false;              // before the built-in systems
    };
    std::vector<BuildHook> build_hooks;
    std::vector<std::string> build_hidden;
    struct ProjectHook {
        std::string name, description;
        std::vector<ProjectField> fields;
        int create = LUA_NOREF;  // create(dir, values) -> the file to open, a command to run there
    };
    std::vector<ProjectHook> project_hooks;
    std::vector<std::string> project_hidden;

    void unref(int& r) {
        if (L && r != LUA_NOREF && r != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, r);
        r = LUA_NOREF;
    }
    ~ScriptUi() {
        jobs.reset();
        if (L) {
            lua_sethook(L, nullptr, 0, 0);
            lua_close(L);
        }
    }
};

// The functions Lua calls (a friend of App's; src/luaapi.cpp).
struct ScriptApi;

namespace script {


inline App* app_of(lua_State* L) { return *static_cast<App**>(lua_getextraspace(L)); }

inline int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) msg = lua_type(L, 1) == LUA_TNIL ? "error (nil)" : "error (not a string)";
    luaL_traceback(L, L, msg, 1);
    return 1;
}


// The API, put into a Lua state: the table `blit` (and blit.opt, blit.ui,
// blit.fs, blit.build, blit.project in it) left on the stack, print and
// os.exit made blit's; and the hook that stops a script that runs too long
// (src/luaapi.cpp).
extern const luaL_Reg kTop[], kUi[], kFs[], kBuild[], kProject[];
void open_api(lua_State* L);
void count_hook(lua_State* L, lua_Debug* ar);

}  // namespace script

}  // namespace blit
