// Scripts, in Lua 5.5: init.lua beside the settings file, the modules it
// requires from lua/ there, the files in plugin/ there, and plugins (each a
// folder with lua/ and plugin/ in it) under blit's data folder. What they
// can reach is the table `blit`: settings (blit.opt), keys (blit.map),
// commands (blit.command), events (blit.on), the open files (blit.lines and
// the like), floating boxes, pickers and messages (blit.ui, blit.notify),
// timers (blit.defer) and programs run in the background (blit.system).
// README.md lists it all. All of it is C++ (src/luaapi.cpp): the Lua is the
// user's alone.
//
// None of this exists until there is a script to run: without init.lua and
// plugins there is no Lua at all, and a key costs what it did.
//
// Lua reports an error by jumping (longjmp) out of whatever C was running,
// and jumping out of a C++ function skips the destructors of its
// variables. So the functions Lua calls here never let it raise an error
// while one of theirs is alive: arguments are read with calls that cannot
// fail, and an error is raised by api<>() once the function has returned
// (see fail()). Lua is called from C++ only through script_call(), which is
//
// The functions scripts call are in src/luaapi.cpp.
#include "applua.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "finder.hpp"
#include "look.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

// ---------------------------------------------------------------- running scripts

// A Lua state, with the API in it; made the first time something needs one.
bool App::script_ensure() {
    if (script_ && script_->L) return true;
    if (!script_) script_ = std::make_shared<ScriptUi>();
    ScriptUi& s = *script_;
    if (s.config_dir.empty()) {
        s.init_path = init_path_.empty() ? default_init_path() : init_path_;
        s.config_dir = parent_of(s.init_path);
        s.data_dir = grammar_dir();
    }
    lua_State* L = luaL_newstate();
    if (!L) return false;
    s.L = L;
    *static_cast<App**>(lua_getextraspace(L)) = this;
    luaL_openlibs(L);
    lua_sethook(L, count_hook, LUA_MASKCOUNT, 10000);
    open_api(L);
    lua_pushlstring(L, s.config_dir.data(), s.config_dir.size());
    lua_setfield(L, -2, "config_dir");
    lua_pushlstring(L, s.data_dir.data(), s.data_dir.size());
    lua_setfield(L, -2, "data_dir");
    lua_pushstring(L, LUA_RELEASE);
    lua_setfield(L, -2, "lua_version");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "blit");
    s.api = luaL_ref(L, LUA_REGISTRYINDEX);
    vim_.has_user_command = [this](std::string_view name) {
        if (!script_) return false;
        for (const ScriptUi::Command& c : script_->commands)
            if (c.name == name) return true;
        return false;
    };
    // Where modules are found: lua/ beside init.lua, then each plugin's lua/.
    std::string paths = s.config_dir + "/lua/?.lua;" + s.config_dir + "/lua/?/init.lua;";
    const std::string plugdir = s.data_dir + "/plugins";
    for (const DirEntry& e : list_dir(plugdir))
        if (e.dir) paths += plugdir + "/" + e.name + "/lua/?.lua;" + plugdir + "/" + e.name + "/lua/?/init.lua;";
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    paths += str_at(L, -1);
    lua_pop(L, 1);
    lua_pushlstring(L, paths.data(), paths.size());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
    // blit's own commands.
    auto own = [&](const char* name, const char* desc, std::function<void(const std::string&)> run) {
        ScriptUi::Command c;
        c.name = name;
        c.desc = desc;
        c.nargs = "0";
        c.native = std::move(run);
        s.commands.push_back(std::move(c));
    };
    own("PlugInstall", "Fetch the plugins named with blit.plug() that are not there yet", [this](const std::string&) { plug_install(); });
    own("PlugUpdate", "Bring every plugin up to date (git pull); :Reload then uses them", [this](const std::string&) { plug_update(); });
    own("PlugList", "The plugins, and which are installed", [this](const std::string&) { plug_list(); });
    own("Reload", "Run init.lua and the plugins again from the start", [this](const std::string&) {
        if (script_) script_->reload = true;
    });
    return true;
}

// ---------------------------------------------------------------- plugins

// A program of blit's own run in the background (git, for the plugins);
// `done` hears how it ended, on the main thread.
void App::script_run(std::vector<std::string> argv, std::string cwd, std::function<void(int, const std::string&, const std::string&)> done) {
    if (!script_ensure()) return;
    ScriptUi& s = *script_;
    if (!s.jobs) {
        s.jobs = std::make_unique<Jobs>();
        s.jobs->set_notify([] {
            SDL_Event ev{};
            ev.type = SDL_EVENT_USER;
            ev.user.code = 11;
            SDL_PushEvent(&ev);
        });
    }
    JobSpec spec;
    spec.argv = std::move(argv);
    spec.cwd = std::move(cwd);
    s.native_jobs[s.jobs->start(std::move(spec))] = std::move(done);
}

// Each .lua file in a folder, run now (in order of name).
void App::script_run_dir(const std::string& dir) {
    for (const DirEntry& e : list_dir(dir))
        if (!e.dir && e.name.ends_with(".lua")) script_file(dir + "/" + e.name);
}

// A plugin (a folder under blit's data folder): its lua/ to require from,
// and its plugin/*.lua run. Once.
void App::plugin_load(const std::string& name) {
    if (!script_ || !script_->L) return;
    ScriptUi& s = *script_;
    if (std::find(s.loaded.begin(), s.loaded.end(), name) != s.loaded.end()) return;
    s.loaded.push_back(name);
    const std::string dir = s.data_dir + "/plugins/" + name;
    const std::string lua = dir + "/lua";
    if (const char* kind = kind_of(lua); kind && std::string_view(kind) == "dir") {
        lua_State* L = s.L;
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "path");
        std::string path(str_at(L, -1));
        lua_pop(L, 1);
        if (path.find(lua) == std::string::npos) {
            path = lua + "/?.lua;" + lua + "/?/init.lua;" + path;
            lua_pushlstring(L, path.data(), path.size());
            lua_setfield(L, -2, "path");
        }
        lua_pop(L, 1);
    }
    script_run_dir(dir + "/plugin");
}

// After init.lua: plugin/ beside it, then every plugin.
void App::script_plugins() {
    if (!script_ || !script_->L) return;
    script_run_dir(script_->config_dir + "/plugin");
    for (const DirEntry& e : list_dir(script_->data_dir + "/plugins"))
        if (e.dir) plugin_load(e.name);
}

namespace {

// What a program that failed said, or else how it ended.
std::string said_of(const std::string& out, const std::string& err, int code) {
    std::string t = err.empty() ? out : err;
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ' || t.back() == '\r')) t.pop_back();
    while (!t.empty() && (t.front() == '\n' || t.front() == ' ')) t.erase(0, 1);
    return t.empty() ? "exited with code " + std::to_string(code) : t;
}

}  // namespace

// :PlugInstall: each plugin blit.plug() named that is not there yet, fetched with git (at its rev, if it has one).
void App::plug_install() {
    if (!script_) return;
    const std::string plugdir = script_->data_dir + "/plugins";
    int wanted = 0;
    for (const ScriptUi::Plugin& p : script_->declared) {
        const std::string dir = plugdir + "/" + p.name;
        if (kind_of(dir)) continue;
        ++wanted;
        script_run({"git", "clone", "--quiet", p.url, dir}, {}, [this, p, dir](int code, const std::string& out, const std::string& err) {
            if (code != 0) return notify(p.name + ": " + said_of(out, err, code), kError);
            if (p.rev.empty()) {
                plugin_load(p.name);
                return notify("Installed " + p.name);
            }
            script_run({"git", "checkout", "--quiet", p.rev}, dir, [this, p](int code2, const std::string& out2, const std::string& err2) {
                if (code2 != 0) return notify(p.name + ": " + said_of(out2, err2, code2), kError);
                plugin_load(p.name);
                notify("Installed " + p.name + " at " + p.rev);
            });
        });
    }
    if (wanted == 0) notify("Every plugin named with blit.plug() is installed");
}

// :PlugUpdate: git pull in each plugin's folder.
void App::plug_update() {
    if (!script_) return;
    const std::string plugdir = script_->data_dir + "/plugins";
    bool any = false;
    for (const DirEntry& e : list_dir(plugdir)) {
        const std::string dir = plugdir + "/" + e.name;
        if (!e.dir || !kind_of(dir + "/.git")) continue;
        any = true;
        const std::string name = e.name;
        script_run({"git", "pull", "--ff-only", "--quiet"}, dir, [this, name](int code, const std::string& out, const std::string& err) {
            notify(name + (code == 0 ? ": up to date" : ": " + said_of(out, err, code)), code == 0 ? kInfo : kError);
        });
    }
    if (!any) notify("No plugins to update");
}

// :PlugList: those named, and whether each is there (- if not), and the others there.
void App::plug_list() {
    if (!script_ensure()) return;
    ScriptUi& s = *script_;
    const std::string plugdir = s.data_dir + "/plugins";
    ScriptUi::Float f;
    f.id = s.next_float++;
    std::vector<std::string> seen;
    for (const ScriptUi::Plugin& p : s.declared) {
        seen.push_back(p.name);
        f.lines.push_back(std::string(kind_of(plugdir + "/" + p.name) ? "  " : "- ") + p.name + "  " + p.url);
    }
    for (const DirEntry& e : list_dir(plugdir))
        if (e.dir && std::find(seen.begin(), seen.end(), e.name) == seen.end()) f.lines.push_back("  " + e.name);
    if (f.lines.empty()) f.lines.push_back("No plugins (blit.plug() names them; they go in " + plugdir + ")");
    f.title = "Plugins (- not installed: :PlugInstall)";
    f.at = kAtCenter;
    f.doc = pane_->doc->id;
    f.cursor = pane_->doc->ed.cursor();
    s.floats.push_back(std::move(f));
    dirty_ = true;
}

// Calls the function under nargs arguments on the stack, protected and
// against the clock. Its results (nres) are left on the stack if it went
// well; if not, nothing is, the error is shown, and the answer is false.
bool App::script_call(int nargs, int nres, const char* what) {
    ScriptUi& s = *script_;
    lua_State* L = s.L;
    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    if (s.depth++ == 0) s.deadline = now_ns() + static_cast<u64>(std::max(100.0f, cfg_.script_timeout_ms)) * 1'000'000;
    const int rc = lua_pcall(L, nargs, nres, base);
    if (--s.depth == 0) {
        if (s.deadline && now_ns() > s.deadline) lua_sethook(L, count_hook, LUA_MASKCOUNT, 10000);  // (the timeout's hook at every instruction, back to its usual pace)
        s.deadline = 0;
        for (const u64 id : s.grouped)
            if (Doc* d = find_doc(id)) d->ed.end_group();
        s.grouped.clear();
    }
    lua_remove(L, base);
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        const std::string said = (what && *what ? std::string(what) + ": " : std::string()) + (msg ? msg : "error");
        lua_pop(L, 1);
        // The box says what went wrong and where; the traceback is in :messages.
        const size_t tb = said.find("\nstack traceback:");
        notify(said.substr(0, tb), kError, tb == std::string::npos ? std::string() : said);
        return false;
    }
    return true;
}

// A file of Lua, run now. It is known by its path from the settings'
// folder or the data folder ("init.lua", "plugins/x/plugin/y.lua"), which
// is what errors in it say.
bool App::script_file(const std::string& path) {
    if (!script_ensure()) return false;
    ScriptUi& s = *script_;
    lua_State* L = s.L;
    std::string text;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[16384];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
    } else {
        notify("cannot read " + path, kError);
        return false;
    }
    std::string name = path;
    const char* home = SDL_getenv("HOME");
    if (name.starts_with(s.config_dir + "/")) name.erase(0, s.config_dir.size() + 1);
    else if (name.starts_with(s.data_dir + "/")) name.erase(0, s.data_dir.size() + 1);
    else if (home && *home && name.starts_with(std::string(home) + "/")) name = "~" + name.substr(std::strlen(home));
    if (luaL_loadbufferx(L, text.data(), text.size(), ("@" + name).c_str(), "t") != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        notify(msg ? msg : ("cannot load " + path), kError);
        lua_pop(L, 1);
        return false;
    }
    return script_call(0, 0, "");
}

bool App::script_string(std::string_view code, const char* name) {
    if (!script_ensure()) return false;
    lua_State* L = script_->L;
    if (luaL_loadbufferx(L, code.data(), code.size(), name, "t") != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        notify(msg ? msg : "cannot load", kError);
        lua_pop(L, 1);
        return false;
    }
    return script_call(0, 0, "");
}

// Pushes a function of the API's by its name (one written in Lua, mostly).
bool App::script_api(const char* name) {
    lua_State* L = script_->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, script_->api);
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (lua_isfunction(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

// ---------------------------------------------------------------- blit.build and blit.project

std::vector<std::string> App::script_hidden_builds() const { return script_ ? script_->build_hidden : std::vector<std::string>(); }

// The systems init.lua added (blit.build.add) that the folder has, and their tasks.
void App::script_build(const std::string& root, const std::vector<FsEntry>& files, std::vector<BuildTask>* tasks) {
    if (!script_ || !script_->L || script_->build_hooks.empty()) return;
    lua_State* const L = script_->L;
    auto push_args = [&] {
        lua_pushlstring(L, root.data(), root.size());
        lua_createtable(L, static_cast<int>(files.size()), 0);
        for (size_t i = 0; i < files.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushlstring(L, files[i].name.data(), files[i].name.size());
            lua_setfield(L, -2, "name");
            lua_pushstring(L, files[i].dir ? "dir" : "file");
            lua_setfield(L, -2, "type");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    };
    std::vector<BuildTask> first;
    const size_t n = script_->build_hooks.size();
    for (size_t i = 0; i < n; ++i) {
        if (!script_ || script_->L != L || i >= script_->build_hooks.size()) break;  // (a call started everything afresh, or took systems away)
        const ScriptUi::BuildHook h = script_->build_hooks[i];
        bool found = false;
        for (const std::string& f : h.files)
            for (const FsEntry& e : files) found |= e.name == f;
        const std::string what = "blit.build.add(\"" + h.name + "\")";
        if (!found && h.detect != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, h.detect);
            push_args();
            if (script_call(2, 1, what.c_str())) {
                found = lua_toboolean(L, -1);
                lua_pop(L, 1);
            }
        }
        if (!found || !script_ || script_->L != L) continue;
        std::vector<BuildTask> got = h.tasks;
        if (h.tasks_fn != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, h.tasks_fn);
            push_args();
            if (script_call(2, 1, what.c_str())) {
                got = script::tasks_at(L, -1, h.name);
                lua_pop(L, 1);
            }
        }
        for (BuildTask& t : got) t.system = h.name;
        (h.first ? first : *tasks).insert((h.first ? first : *tasks).end(), got.begin(), got.end());
    }
    tasks->insert(tasks->begin(), first.begin(), first.end());
}

std::vector<ProjectKind> App::script_project_kinds() {
    std::vector<ProjectKind> out;
    const ScriptUi* s = script_ && script_->L ? script_.get() : nullptr;
    for (const ProjectKind& k : project_kinds())
        if (!s || std::find(s->project_hidden.begin(), s->project_hidden.end(), k.name) == s->project_hidden.end()) out.push_back(k);
    if (s)
        for (const ScriptUi::ProjectHook& h : s->project_hooks) {
            ProjectKind k;
            k.name = h.name;
            k.description = h.description;
            k.fields = h.fields;
            k.scripted = true;
            std::erase_if(out, [&](const ProjectKind& o) { return o.name == k.name; });
            out.push_back(std::move(k));
        }
    return out;
}

// init.lua's kind of project made in dir (there already): create(dir, values) -> the file to open, a command to run.
bool App::script_project_create(const std::string& kind, const std::string& dir, const ProjectValues& values, std::string* open, std::string* then, std::string* err) {
    if (!script_ || !script_->L) return *err = "init.lua is not running", false;
    lua_State* const L = script_->L;
    int fn = LUA_NOREF;
    for (const ScriptUi::ProjectHook& h : script_->project_hooks)
        if (h.name == kind) fn = h.create;
    if (fn == LUA_NOREF) return *err = "no kind of project called " + kind, false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
    lua_pushlstring(L, dir.data(), dir.size());
    lua_createtable(L, 0, static_cast<int>(values.size()));
    for (const auto& [k, v] : values) {
        if (v == "true" || v == "false") lua_pushboolean(L, v == "true");
        else lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, k.c_str());
    }
    if (!script_call(2, 2, ("blit.project.add(\"" + kind + "\")").c_str())) return *err = "its create function failed (:messages says why)", false;
    if (lua_type(L, -2) == LUA_TSTRING) *open = std::string(script::str_at(L, -2));
    if (lua_type(L, -1) == LUA_TSTRING) *then = std::string(script::str_at(L, -1));
    lua_pop(L, 2);
    return true;
}

// blit[table][name] on the stack, if it is a function (as script_api).
bool App::script_lib(const char* table, const char* name) {
    lua_State* L = script_->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, script_->api);
    lua_getfield(L, -1, table);
    lua_remove(L, -2);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, name);
        lua_remove(L, -2);
        if (lua_isfunction(L, -1)) return true;
    }
    lua_pop(L, 1);
    return false;
}

// At startup: init.lua, then plugin/ beside it and the plugins'. Nothing
// at all when there are none of these.
void App::script_start() {
    const std::string init_path = init_path_.empty() ? default_init_path() : init_path_;
    const std::string dir = parent_of(init_path);
    const std::string plugins = grammar_dir() + "/plugins";
    if (!kind_of(init_path) && !kind_of(dir + "/plugin") && list_dir(plugins).empty()) return;
    if (!script_ensure()) return;
    ScriptUi& s = *script_;
    s.starting = true;
    s.init_stamp = stamp_of(s.init_path);
    if (kind_of(s.init_path)) script_file(s.init_path);
    script_plugins();
    s.starting = false;
    if (!script_settings_.empty()) cfg_ = make_config(script_settings_);  // (all of them as one: see make_config())
}

// Once the window is up: the first file was read, and blit has started.
void App::script_started(bool fresh) {
    if (!script_ || !script_->L) return;
    const std::vector<std::pair<char, std::string>> later = std::move(script_->later);
    script_->later.clear();
    for (const auto& [what, arg] : later) {
        if (!script_ || !script_->L) return;
        std::string err;
        switch (what) {
            case 'c':
                if (cfg_.modal) vim_attach();
                vim_.ex(pane_->doc->ed, arg);
                vim_after(Vim::kHandled);
                break;
            case 'n':
                if (!cfg_.modal) break;
                vim_attach();
                vim_.keys(pane_->doc->ed, arg);
                vim_after(Vim::kHandled);
                break;
            case 'o': open_path(full_path(arg).c_str()); break;
            default:
                if (!script_feed(arg, what == 'F', &err)) notify("blit.feed: " + err, kError);
                break;
        }
    }
    if (!pane_->doc->ed.path.empty()) script_opened(pane_->doc->id, fresh);
    script_event(kEvBufEnter, pane_->doc->id);
    script_event(kEvVimEnter, 0);
}

void App::script_dir(const std::string& dir) { script_event(kEvDirChanged, pane_->doc->id, {}, dir); }

// Everything scripts set up goes (keys, commands, handlers, timers, jobs,
// boxes), and the Lua state with it. Messages stay.
void App::script_shutdown() {
    if (!script_) return;
    ScriptUi& s = *script_;
    s.pick_fn = s.pick_items = LUA_NOREF;  // (a picker of a script's that is open shuts, without calling anything)
    find_drop_script();
    if (s.jobs) s.jobs->stop_all();
    s.jobs.reset();
    s.job_calls.clear();
    s.native_jobs.clear();
    s.backlog.clear();
    s.maps.clear();
    s.pending.clear();
    s.has_exact = false;
    s.commands.clear();
    s.handlers.clear();
    s.mask = 0;
    s.queue.clear();
    s.timers.clear();
    s.floats.clear();
    s.marks.clear();
    s.marks_unsorted.clear();
    s.namespaces.clear();
    s.pick_fn = s.pick_items = LUA_NOREF;
    s.seen = false;
    script_keys_ = false;
    script_modes_ = false;
    script_status_.clear();
    vim_.has_user_command = nullptr;
    if (s.L) lua_close(s.L);
    s.L = nullptr;
    s.api = LUA_NOREF;
    dirty_ = true;
}

// init.lua and the plugins again, from the start, with the settings they
// make on top of the settings file's.
void App::script_restart() {
    if (script_) {
        if (script_->depth > 0) {  // (not from inside a script: when it has finished)
            script_->reload = true;
            return;
        }
        script_->reload = false;
    }
    script_shutdown();
    script_settings_.clear();
    // init.lua starts again from the defaults (what it no longer sets goes
    // back to them); what it ends with is then put into effect at once.
    Config now = cfg_;
    cfg_ = make_config();
    script_start();  // (its settings go straight into cfg_ while it starts)
    Config next = std::move(cfg_);
    cfg_ = std::move(now);
    apply_config(std::move(next));
    if (script_ && script_->L) {
        script_event(kEvBufEnter, pane_->doc->id);
        script_event(kEvVimEnter, 0);
    }
}

// A setting from a script: checked, kept (it is made again on top of the
// settings file whenever that is read), and put into effect.
std::string App::script_set(const std::string& key, const std::string& value, const std::string& from) {
    const bool starting = script_ && script_->starting;
    if (starting) {
        // While init.lua runs: straight in (a wrong one changes nothing),
        // so that what it reads back is what it set. They are all made
        // again as one when it is done (script_start()).
        const std::string wrong = config_set(cfg_, {key, value, from});
        if (!wrong.empty()) return wrong;
    } else {
        Config probe = cfg_;
        const std::string wrong = config_set(probe, {key, value, from});
        if (!wrong.empty()) return wrong;
    }
    std::erase_if(script_settings_, [&](const ConfigSetting& c) { return c.key == key; });
    script_settings_.push_back({key, value, from});
    if (!starting) apply_config(make_config(script_settings_));
    return {};
}

// Messages: in the bottom line for a short one with Vim's modes on, in a
// box at the bottom right otherwise; and on the standard error, and in
// the list :messages shows.
void App::notify(const std::string& text, int level, const std::string& detail) {
    const std::string& full = detail.empty() ? text : detail;
    std::fprintf(stderr, "blit: %s\n", full.c_str());
    if (!script_) script_ = std::make_shared<ScriptUi>();
    ScriptUi& s = *script_;
    s.messages.push_back(full);
    if (s.messages.size() > 200) s.messages.erase(s.messages.begin());
    if (level == kInfo && cfg_.modal && text.find('\n') == std::string::npos && width_of(text) < 120 && win_) {
        vim_.say(text);
    } else {
        ScriptUi::Toast t;
        t.text = text;
        t.level = level;
        s.toasts.push_back(std::move(t));
        if (s.toasts.size() > 4) s.toasts.erase(s.toasts.begin());
    }
    dirty_ = true;
}

// ---------------------------------------------------------------- keys

char App::script_mode_char() const {
    if (!cfg_.modal) return 'i';
    switch (vim_.mode()) {
        case Vim::kInsert: return 'i';
        case Vim::kReplace: return 'R';
        case Vim::kVisual: return 'v';
        case Vim::kVisualLine: return 'V';
        case Vim::kVisualBlock: return 'b';
        case Vim::kCommand: return 'c';
        default: return vim_.pending() ? 'o' : 'n';
    }
}

namespace {

u8 map_mode_of(char m) {
    switch (m) {
        case 'i':
        case 'R': return Keymaps::kInsert;
        case 'v':
        case 'V':
        case 'b': return Keymaps::kVisual;
        case 'c': return Keymaps::kCommand;
        case 'o': return Keymaps::kPending;
        default: return Keymaps::kNormal;
    }
}

// A key press as a mapping's key. False for a key that is not one (Shift
// alone), and for a character with no Ctrl, Option or Cmd: that comes as
// typed text, and is looked at then.
bool map_key_of(const SDL_KeyboardEvent& k, MapKey* out) {
    MapKey m;
    if (k.mod & SDL_KMOD_CTRL) m.mods |= MapKey::kCtrl;
    if (k.mod & SDL_KMOD_ALT) m.mods |= MapKey::kAlt;
    if (k.mod & SDL_KMOD_GUI) m.mods |= MapKey::kCmd;
    if (k.mod & SDL_KMOD_SHIFT) m.mods |= MapKey::kShift;
    switch (k.key) {
        case SDLK_ESCAPE: m.kind = MapKey::kEsc; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: m.kind = MapKey::kEnter; break;
        case SDLK_BACKSPACE: m.kind = MapKey::kBackspace; break;
        case SDLK_TAB: m.kind = MapKey::kTab; break;
        case SDLK_LEFT: m.kind = MapKey::kLeft; break;
        case SDLK_RIGHT: m.kind = MapKey::kRight; break;
        case SDLK_UP: m.kind = MapKey::kUp; break;
        case SDLK_DOWN: m.kind = MapKey::kDown; break;
        case SDLK_HOME: m.kind = MapKey::kHome; break;
        case SDLK_END: m.kind = MapKey::kEnd; break;
        case SDLK_PAGEUP: m.kind = MapKey::kPageUp; break;
        case SDLK_PAGEDOWN: m.kind = MapKey::kPageDown; break;
        case SDLK_DELETE: m.kind = MapKey::kDelete; break;
        default:
            if (k.key >= SDLK_F1 && k.key <= SDLK_F12) {
                m.kind = MapKey::kFn;
                m.ch = k.key - SDLK_F1 + 1;
                break;
            }
            if (k.key >= SDLK_F13 && k.key <= SDLK_F24) {
                m.kind = MapKey::kFn;
                m.ch = k.key - SDLK_F13 + 13;
                break;
            }
            if (k.key < 0x20 || k.key >= 0x40000000) return false;
            if (!(m.mods & (MapKey::kCtrl | MapKey::kAlt | MapKey::kCmd))) return false;
            m.kind = MapKey::kChar;
            m.ch = k.key;
            break;
    }
    *out = m;
    return true;
}

// And back: the press a mapping's key stands for.
SDL_KeyboardEvent press_of(const MapKey& m) {
    SDL_KeyboardEvent e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.down = true;
    e.mod = (m.mods & MapKey::kCtrl ? SDL_KMOD_LCTRL : 0) | (m.mods & MapKey::kAlt ? SDL_KMOD_LALT : 0) | (m.mods & MapKey::kCmd ? SDL_KMOD_LGUI : 0) |
            (m.mods & MapKey::kShift ? SDL_KMOD_LSHIFT : 0);
    static const SDL_Keycode named[] = {0, SDLK_ESCAPE, SDLK_RETURN, SDLK_BACKSPACE, SDLK_TAB, SDLK_LEFT, SDLK_RIGHT, SDLK_UP, SDLK_DOWN, SDLK_HOME, SDLK_END, SDLK_PAGEUP, SDLK_PAGEDOWN, SDLK_DELETE};
    if (m.kind == MapKey::kFn) e.key = m.ch <= 12 ? SDLK_F1 + m.ch - 1 : SDLK_F13 + m.ch - 13;
    else if (m.kind != MapKey::kChar) e.key = named[m.kind];
    else e.key = m.ch >= 'A' && m.ch <= 'Z' ? m.ch + 32 : m.ch;
    return e;
}

}  // namespace

// A key press, before anything else has it. True if a mapping took it (or
// may: it is waiting for the keys after it).
bool App::script_key(const SDL_KeyboardEvent& k) {
    ScriptUi& s = *script_;
    if (s.unmapped || find_open_ || panel_has_keyboard() || (cfg_.modal && vim_.awaiting_char())) return false;
    MapKey key;
    if (!map_key_of(k, &key)) return false;
    if (s.pending.empty() && !s.maps.starts(map_mode_of(script_mode_char()), key)) return false;
    ScriptUi::Pending p;
    p.key = key;
    p.down = true;
    p.ev = k;
    if (!script_take(std::move(p))) return false;
    // Option and a letter type a character as well: it belongs to this
    // press. (Nothing else does: with Cmd or Ctrl, or a key that is not a
    // character's, the text that comes next is the next key's.)
    script_drop_text_ = key.kind == MapKey::kChar && (key.mods & MapKey::kAlt) && !(key.mods & (MapKey::kCmd | MapKey::kCtrl));
    return true;
}

// The text a press that a mapping took typed as well.
void App::script_dropped(const char* text) {
    if (script_ && !script_->pending.empty() && script_->pending.back().down) script_->pending.back().text += text;
}

// Typed text, before it goes anywhere. True if the mappings had it.
bool App::script_text(const char* text) {
    ScriptUi& s = *script_;
    if (s.unmapped || find_open_ || panel_has_keyboard()) return false;
    const size_t n = std::strlen(text);
    if (n == 0) return false;
    const Decoded d = decode_utf8(reinterpret_cast<const u8*>(text), n);
    if (d.len < n) {
        // Several characters at once (an input method, a paste): one at a
        // time, each in the mode the ones before it left.
        for (size_t i = 0; i < n;) {
            const u32 len = decode_utf8(reinterpret_cast<const u8*>(text) + i, n - i).len;
            const std::string one(text + i, len);
            i += len;
            if (!script_ || !script_->L) {
                on_text(one.c_str());
                continue;
            }
            if (!script_text(one.c_str())) {
                ++script_->unmapped;
                typed_text(one.c_str());
                if (script_) --script_->unmapped;
            }
        }
        return true;
    }
    if (cfg_.modal && vim_.awaiting_char()) return false;
    ScriptUi::Pending p;
    p.key.ch = d.cp;
    if (s.pending.empty() && !s.maps.starts(map_mode_of(script_mode_char()), p.key)) return false;
    p.text = text;
    return script_take(std::move(p));
}

// One more key for the mappings. False: no mapping starts with the keys
// waiting and this one, which was the only one (it goes on as it is).
bool App::script_take(ScriptUi::Pending p) {
    ScriptUi& s = *script_;
    s.pending.push_back(std::move(p));
    script_keys_ = true;
    return script_match(false);
}

// What the keys waiting add up to. `late`: no more are coming in time.
bool App::script_match(bool late) {
    ScriptUi& s = *script_;
    std::vector<MapKey> keys;
    for (const ScriptUi::Pending& p : s.pending) keys.push_back(p.key);
    const u8 mode = map_mode_of(script_mode_char());
    const Keymaps::Map* m = nullptr;
    const Keymaps::Match r = s.maps.match(mode, keys, &m);
    if (r == Keymaps::kExact || (r == Keymaps::kBoth && late)) {
        Keymaps::Map run = *m;
        s.pending.clear();
        s.has_exact = false;
        script_keys_ = !s.maps.empty();
        script_run_map(run);
        return true;
    }
    if ((r == Keymaps::kPrefix || r == Keymaps::kBoth) && !late) {
        if (r == Keymaps::kBoth) {
            s.exact = *m;
            s.has_exact = true;
            s.exact_len = keys.size();
        }
        s.pending_until = now_ns() + static_cast<u64>(std::max(0.0f, cfg_.key_timeout_ms)) * 1'000'000;
        return true;
    }
    // Not a mapping, and not the start of one.
    if (s.pending.size() == 1 && !s.has_exact && !late) {
        s.pending.clear();
        script_keys_ = !s.maps.empty();
        return false;
    }
    std::vector<ScriptUi::Pending> rest;
    if (s.has_exact) {  // the longest mapping they started with is what they were
        Keymaps::Map run = s.exact;
        rest.assign(s.pending.begin() + static_cast<std::ptrdiff_t>(s.exact_len), s.pending.end());
        s.pending.clear();
        s.has_exact = false;
        script_run_map(run);
    } else {  // the first key is itself, and the others are looked at afresh
        ScriptUi::Pending first = std::move(s.pending.front());
        rest.assign(std::make_move_iterator(s.pending.begin() + 1), std::make_move_iterator(s.pending.end()));
        s.pending.clear();
        script_replay(first);
    }
    for (ScriptUi::Pending& p : rest) {
        if (!script_ || !script_->L) break;
        const ScriptUi::Pending copy = p;
        if (!script_take(std::move(p))) script_replay(copy);
    }
    if (script_) script_keys_ = !script_->maps.empty() || !script_->pending.empty();
    return true;
}

// A key that was held back goes on as it would have.
void App::script_replay(const ScriptUi::Pending& p) {
    ++script_->unmapped;
    if (p.down) {
        on_key(p.ev);
        if (!p.text.empty()) typed_text(p.text.c_str());
    } else {
        typed_text(p.text.c_str());
    }
    if (script_) --script_->unmapped;
}

void App::script_run_map(const Keymaps::Map& m) {
    ScriptUi& s = *script_;
    if (m.fn >= 0) {
        s.count = cfg_.modal ? vim_.count() : 0;
        if (cfg_.modal && s.count && !vim_.pending()) vim_.cancel();
        lua_rawgeti(s.L, LUA_REGISTRYINDEX, m.fn);
        script_call(0, 0, ("mapping " + key_notation(m.lhs)).c_str());
        if (script_) script_->count = 0;
        pane_->cache_rev = ~0ull;
        dirty_ = true;
        return;
    }
    std::string err;
    if (!script_feed(m.keys, m.remap, &err)) notify("mapping " + key_notation(m.lhs) + ": " + err, kError);
}

// Keys in Vim's notation, as if typed: through the mappings or not.
bool App::script_feed(std::string_view notation, bool remap, std::string* err) {
    if (!script_) return false;
    std::vector<MapKey> keys;
    if (!parse_keys(notation, cfg_.key_leader, &keys, err)) return false;
    ScriptUi& s = *script_;
    if (s.feeding > 20) {
        *err = "mappings that type mappings, more than 20 deep";
        return false;
    }
    ++s.feeding;
    if (!remap) ++s.unmapped;
    for (const MapKey& k : keys) {
        if (k.kind == MapKey::kChar && !(k.mods & (MapKey::kCtrl | MapKey::kAlt | MapKey::kCmd))) {
            std::string t;
            append_utf8(&t, k.ch);
            typed_text(t.c_str());
        } else {
            on_key(press_of(k));
        }
        if (!script_) return true;  // (a key that shut everything)
    }
    if (!remap) --s.unmapped;
    --s.feeding;
    return true;
}

// Typed text, wherever it goes: the finder box, the side panel, the mappings, the text.
void App::typed_text(const char* text) {
    if (script_drop_text_) {
        script_drop_text_ = false;
        return script_dropped(text);
    }
    if (find_open_ && find_typed(text)) return;
    if (panel_text(text)) return;
    if (start_text(text)) return;  // the start screen's keys (appstart.cpp)
    if (script_keys_ && script_text(text)) return;
    on_text(text);
}

// ---------------------------------------------------------------- events

void App::script_mask() {
    ScriptUi& s = *script_;
    const u32 had = s.mask;
    s.mask = 0;
    for (const ScriptUi::Handler& h : s.handlers) s.mask |= h.events;
    if ((s.mask & kWatched) && !(had & kWatched)) s.seen = false;  // looked at from now on
    script_modes_ = s.mask & (bit(kEvModeChanged) | bit(kEvInsertEnter) | bit(kEvInsertLeave));
    if (script_modes_ && !(had & (bit(kEvModeChanged) | bit(kEvInsertEnter) | bit(kEvInsertLeave)))) s.seen_mode = script_mode_char();
}

// Vim may have changed mode: every change is an event (in the order they
// came, at the next tick), however many keys one batch of input holds.
void App::script_mode_seen() {
    ScriptUi& s = *script_;
    const char mode = script_mode_char(), old = s.seen_mode;
    if (mode == old) return;
    s.seen_mode = mode;
    auto code = [](char m) { return m == 'b' ? std::string("\x16") : m == 'o' ? std::string("no") : std::string(1, m); };
    const u64 id = pane_->doc->id;
    script_event(kEvModeChanged, id, {}, code(old) + ":" + code(mode));
    const bool was = old == 'i' || old == 'R', is = mode == 'i' || mode == 'R';
    if (!was && is) script_event(kEvInsertEnter, id);
    if (was && !is) script_event(kEvInsertLeave, id);
}

std::string App::doc_language(const Doc& d) const {
    if (d.ed.path.empty()) return {};
    char head[128];
    const u64 n = d.ed.buf.read(0, head, sizeof head);
    const std::string_view first(head, static_cast<size_t>(std::min<u64>(n, std::string_view(head, n).find('\n'))));
    return Syntax::language_for(d.ed.path, first, cfg_.grammars);
}

// An event for the next tick (it happened in the middle of something).
void App::script_event(u32 event, u64 doc, std::string file, std::string match) {
    if (!script_ || !(script_->mask & bit(event))) return;
    if (file.empty() && doc)
        if (Doc* d = find_doc(doc)) file = d->ed.path;
    script_->queue.push_back({event, doc, std::move(file), std::move(match)});
}

// The handlers of an event, now. `data`: a stack slot with something for them, or 0.
void App::script_fire(u32 event, u64 doc, const std::string& file, const std::string& match, int data) {
    if (!script_ || !script_->L || !(script_->mask & bit(event))) return;
    ScriptUi& s = *script_;
    const std::string& against = event == kEvFileType || event == kEvUser || event == kEvModeChanged || event == kEvDirChanged ? match : file;
    std::vector<u64> ids;
    for (const ScriptUi::Handler& h : s.handlers)
        if ((h.events & bit(event)) && pattern_matches(h.pattern, against)) ids.push_back(h.id);
    lua_State* L = s.L;
    if (data) data = lua_absindex(L, data);
    for (const u64 id : ids) {
        if (!script_ || script_->L != L) return;  // (a handler started everything afresh)
        auto it = std::find_if(s.handlers.begin(), s.handlers.end(), [&](const ScriptUi::Handler& h) { return h.id == id; });
        if (it == s.handlers.end()) continue;  // (an earlier one took it away)
        const int fn = it->fn;
        lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
        if (it->once) {
            s.handlers.erase(it);
            script_mask();
            luaL_unref(L, LUA_REGISTRYINDEX, fn);  // (the function is on the stack: it lives while it is called)
        }
        lua_createtable(L, 0, 6);
        lua_pushstring(L, kEventNames[event]);
        lua_setfield(L, -2, "event");
        lua_pushinteger(L, static_cast<lua_Integer>(doc));
        lua_setfield(L, -2, "buf");
        lua_pushlstring(L, file.data(), file.size());
        lua_setfield(L, -2, "file");
        lua_pushlstring(L, match.data(), match.size());
        lua_setfield(L, -2, "match");
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        lua_setfield(L, -2, "id");
        if (data) {
            lua_pushvalue(L, data);
            lua_setfield(L, -2, "data");
        }
        script_call(1, 0, kEventNames[event]);
    }
}

// Before and after a document is written (now: a handler may change it
// first). False if it has gone meanwhile.
bool App::script_write(bool after, u64 doc) {
    if (!script_ || !script_->L) return true;
    const u32 ev = after ? kEvBufWritePost : kEvBufWritePre;
    if (!(script_->mask & bit(ev)) || script_->writing == doc) return true;
    Doc* d = find_doc(doc);
    if (!d) return false;
    const std::string path = d->ed.path;
    const u64 was = script_->writing;
    script_->writing = doc;
    script_fire(ev, doc, path, path);
    if (script_) script_->writing = was;
    return find_doc(doc) != nullptr;
}

void App::script_opened(u64 doc, bool fresh) {
    if (!script_) return;
    Doc* d = find_doc(doc);
    if (!d) return;
    script_event(fresh ? kEvBufNewFile : kEvBufReadPost, doc, d->ed.path, d->ed.path);
    script_event(kEvFileType, doc, d->ed.path, doc_language(*d));
}

void App::script_closed(const Doc& d) {
    if (!script_) return;
    script_->marks.erase(d.id);
    script_event(kEvBufDelete, d.id, d.ed.path, d.ed.path);
}

void App::script_focus(bool gained) {
    if (!script_ || !script_->L) {
        if (gained && kind_of(init_path_)) script_restart();  // (an init.lua made since blit started)
        return;
    }
    if (gained && script_->L && stamp_of(script_->init_path) != script_->init_stamp) {
        script_restart();
        return;
    }
    script_event(gained ? kEvFocusGained : kEvFocusLost, pane_->doc->id);
}

void App::script_saved(const std::string& path) {
    // A script of the settings' was saved: all of them run again.
    const std::string dir = parent_of(init_path_) + "/";
    const bool ours = path.ends_with(".lua") && (path.starts_with(dir) || (script_ && path == script_->init_path));
    if (ours) script_restart();
}

// What the commands outside Vim's own do: :lua, :=, :set, :map, a script's.
void App::script_ex(const VimRequest& q) {
    const std::string& name = q.name;
    auto is = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            if (name == n) return true;
        return false;
    };
    if (name == "=" || is({"lua"})) {
        std::string code = q.arg;
        if (code.empty() && q.ranged) {  // :'<,'>lua runs the lines
            const Editor& ed = pane_->doc->ed;
            const u64 a = ed.buf.line_start(q.line0), b = q.line1 + 1 < ed.buf.line_count() ? ed.buf.line_start(q.line1 + 1) : ed.buf.size();
            code = ed.buf.text(a, b - a);
            if (!script_ensure()) return;
            script_string(code, "=(:lua)");
            return;
        }
        if (name == "lua" && !code.starts_with("=")) {
            if (!script_ensure()) return;
            script_string(code, "=(:lua)");
            return;
        }
        if (code.starts_with("=")) code.erase(0, 1);
        if (!script_ensure() || !script_api("_eval")) return;
        lua_pushlstring(script_->L, code.data(), code.size());
        script_call(1, 0, ":lua");
        return;
    }
    if (is({"luafile", "so", "source"})) {
        std::string path = q.arg.empty() || q.arg == "%" ? pane_->doc->ed.path : q.arg;
        if (path.starts_with("~/") && SDL_getenv("HOME")) path = std::string(SDL_getenv("HOME")) + path.substr(1);
        if (!path.empty() && path[0] != '/') {
            const std::string* root = panel_root();
            path = (root ? *root : default_workdir()) + "/" + path;
        }
        if (script_ && path == script_->init_path) return script_restart();
        if (!path.ends_with(".lua")) {
            vim_.say("E484: Only Lua files can be sourced: " + path);
            return;
        }
        script_file(path);
        return;
    }
    if (is({"mes", "messages"})) {
        if (!script_ || script_->messages.empty()) {
            vim_.say("No messages");
            return;
        }
        if (!script_ensure()) return;
        ScriptUi& s = *script_;
        ScriptUi::Float f;
        f.id = s.next_float++;
        const size_t from = s.messages.size() > 40 ? s.messages.size() - 40 : 0;
        for (size_t i = from; i < s.messages.size(); ++i)
            for (std::string& l : split_lines(s.messages[i])) f.lines.push_back(std::move(l));
        f.title = "Messages";
        f.at = kAtCenter;
        f.doc = pane_->doc->id;
        f.cursor = pane_->doc->ed.cursor();
        s.floats.push_back(std::move(f));
        dirty_ = true;
        return;
    }
    if (is({"se", "set", "setl", "setlocal"})) return script_set_command(q.arg);
    const bool is_map = [&] {
        std::string_view rest = name;
        if (rest != "map" && rest != "noremap" && rest != "unmap" && !rest.empty() && std::strchr("nivxoc", rest[0])) rest.remove_prefix(1);
        return rest == "map" || rest == "noremap" || rest == "unmap";
    }();
    if (is_map) {
        // :map, :nnoremap and the like: lhs rhs (and with nothing, the mappings there are).
        if (!script_ensure()) return;
        ScriptUi& s = *script_;
        const bool un = name.find("unmap") != std::string::npos;
        const bool noremap = name.find("noremap") != std::string::npos;
        const char first = name[0];
        const std::string modes = std::strchr("nivxoc", first) && name.size() > 1 && name[1] != 'o' ? std::string(1, first) : std::string();
        std::string_view arg = q.arg;
        const size_t space = arg.find(' ');
        const std::string lhs(arg.substr(0, space));
        std::string rhs = space == std::string_view::npos ? std::string() : std::string(arg.substr(space + 1));
        while (!rhs.empty() && rhs.front() == ' ') rhs.erase(0, 1);
        if (lhs.empty()) {
            ScriptUi::Float f;
            f.id = s.next_float++;
            for (const Keymaps::Map& m : s.maps.all()) {
                std::string line;
                for (const auto& [b, letter] : {std::pair{Keymaps::kNormal, 'n'}, {Keymaps::kInsert, 'i'}, {Keymaps::kVisual, 'v'}, {Keymaps::kPending, 'o'}, {Keymaps::kCommand, 'c'}})
                    line += m.modes & b ? letter : ' ';
                line += "  " + key_notation(m.lhs);
                line.resize(std::max<size_t>(line.size() + 2, 22), ' ');
                line += m.fn >= 0 ? (m.desc.empty() ? std::string("<function>") : m.desc) : m.keys + (m.desc.empty() ? "" : "  " + m.desc);
                f.lines.push_back(std::move(line));
            }
            if (f.lines.empty()) f.lines.push_back("No mappings");
            f.title = "Mappings";
            f.at = kAtCenter;
            f.doc = pane_->doc->id;
            s.floats.push_back(std::move(f));
            dirty_ = true;
            return;
        }
        const u8 mode_bits = Keymaps::modes_of(modes);
        std::string err;
        std::vector<MapKey> keys;
        if (!parse_keys(lhs, cfg_.key_leader, &keys, &err)) {
            vim_.say("E474: " + err);
            return;
        }
        if (un) {
            for (int gone : s.maps.remove(mode_bits, keys)) s.unref(gone);
        } else {
            if (rhs.empty()) {
                vim_.say("E474: what the keys do is missing");
                return;
            }
            Keymaps::Map m;
            m.lhs = std::move(keys);
            m.modes = mode_bits;
            m.keys = rhs;
            m.remap = !noremap;
            for (int gone : s.maps.set(std::move(m))) s.unref(gone);
        }
        script_keys_ = !s.maps.empty() || !s.pending.empty();
        return;
    }
    if (is({"command", "com"})) {
        // :command Name what-it-does (a command line; <args> is what follows the name).
        std::string_view arg = q.arg;
        const size_t space = arg.find(' ');
        const std::string cname(arg.substr(0, space));
        const std::string ex = space == std::string_view::npos ? std::string() : std::string(arg.substr(space + 1));
        if (cname.empty() || !std::isupper(static_cast<u8>(cname[0])) || ex.empty()) {
            vim_.say("E183: :command Name what-it-does (the name starts with a capital)");
            return;
        }
        if (!script_ensure()) return;
        ScriptUi& s = *script_;
        for (ScriptUi::Command& c : s.commands)
            if (c.name == cname) {
                s.unref(c.fn);
                c = {cname, ex, {}, "*", LUA_NOREF, false, false, {}};
                return;
            }
        s.commands.push_back({cname, ex, {}, "*", LUA_NOREF, false, false, {}});
        return;
    }
    if (is({"delcommand", "delc"})) {
        if (!script_) return;
        ScriptUi& s = *script_;
        for (size_t i = 0; i < s.commands.size(); ++i)
            if (s.commands[i].name == q.arg) {
                s.unref(s.commands[i].fn);
                s.commands.erase(s.commands.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        vim_.say("E184: No such user-defined command: " + q.arg);
        return;
    }
    // A script's command.
    if (!script_ || !script_->L) {
        vim_.say("E492: Not an editor command: " + name);
        return;
    }
    ScriptUi& s = *script_;
    const ScriptUi::Command* c = nullptr;
    for (const ScriptUi::Command& have : s.commands)
        if (have.name == name) c = &have;
    if (!c) {
        vim_.say("E492: Not an editor command: " + name);
        return;
    }
    if (q.force && !c->bang) {
        vim_.say("E477: No ! allowed");
        return;
    }
    if (q.ranged && !c->range) {
        vim_.say("E481: No range allowed");
        return;
    }
    const std::vector<std::string> fargs = [&] {
        std::vector<std::string> out;
        std::string word;
        for (const char ch : q.arg) {
            if (ch == ' ') {
                if (!word.empty()) out.push_back(std::move(word)), word.clear();
            } else {
                word += ch;
            }
        }
        if (!word.empty()) out.push_back(std::move(word));
        return out;
    }();
    const std::string& n = c->nargs;
    if ((n == "0" && !fargs.empty()) || (n == "1" && q.arg.empty()) || (n == "+" && fargs.empty()) || (n == "?" && fargs.size() > 1)) {
        vim_.say("E471: " + name + (n == "0" ? " takes no arguments" : n == "1" || n == "+" ? " needs an argument" : " takes one argument at most"));
        return;
    }
    if (c->native) {
        const auto run = c->native;
        run(q.arg);
        vim_after(Vim::kHandled);
        return;
    }
    if (c->fn == LUA_NOREF) {
        std::string ex = c->ex;
        for (size_t at = 0; (at = ex.find("<args>", at)) != std::string::npos; at += q.arg.size()) ex.replace(at, 6, q.arg);
        if (s.expanding > 20) {
            vim_.say("E169: Command too recursive: " + name);
            return;
        }
        ++s.expanding;  // (what the command runs is carried out in vim_after: the count covers both)
        vim_.ex(pane_->doc->ed, ex);
        vim_after(Vim::kHandled);
        if (script_) --script_->expanding;
        return;
    }
    lua_State* L = s.L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
    lua_createtable(L, 0, 7);
    lua_pushlstring(L, q.arg.data(), q.arg.size());
    lua_setfield(L, -2, "args");
    lua_createtable(L, static_cast<int>(fargs.size()), 0);
    for (size_t i = 0; i < fargs.size(); ++i) {
        lua_pushlstring(L, fargs[i].data(), fargs[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "fargs");
    lua_pushboolean(L, q.force);
    lua_setfield(L, -2, "bang");
    lua_pushinteger(L, q.ranged ? 2 : 0);
    lua_setfield(L, -2, "range");
    lua_pushinteger(L, static_cast<lua_Integer>(q.line0 + 1));
    lua_setfield(L, -2, "line1");
    lua_pushinteger(L, static_cast<lua_Integer>(q.line1 + 1));
    lua_setfield(L, -2, "line2");
    lua_pushlstring(L, name.data(), name.size());
    lua_setfield(L, -2, "name");
    script_call(1, 0, (":" + name).c_str());
}

// :set name=value, :set name, :set noname, :set name? (the names are the
// settings file's, the part after the last dot alone where that is
// enough, or a few of Vim's).
void App::script_set_command(const std::string& arg) {
    if (arg.empty()) {
        vim_.say("Settings: :set name=value (font.size=14, tab_width=2, ligatures, noligatures)");
        return;
    }
    std::string said;
    size_t at = 0;
    while (at < arg.size()) {
        while (at < arg.size() && arg[at] == ' ') ++at;
        size_t end = at;
        bool quoted = false;
        while (end < arg.size() && (quoted || arg[end] != ' ')) {
            if (arg[end] == '"') quoted = !quoted;
            ++end;
        }
        std::string word = arg.substr(at, end - at);
        at = end;
        if (word.empty()) continue;
        const size_t eq = word.find_first_of("=:");
        std::string name = word.substr(0, eq);
        std::string value = eq == std::string::npos ? std::string() : word.substr(eq + 1);
        const bool ask = name.ends_with("?");
        if (ask) name.pop_back();
        bool flag = true;
        auto resolve = [&](const std::string& n) -> std::string {
            for (const auto& v : kVimNames)
                if (n == v.vim) return v.key;
            std::string found;
            for (const char* k : config_keys()) {
                const std::string_view key = k;
                if (key == n) return std::string(key);
                const size_t dot = key.rfind('.');
                if (key.substr(dot + 1) == n) {
                    if (!found.empty()) return "?" + found + " " + std::string(key);
                    found = key;
                }
            }
            return found;
        };
        std::string key = resolve(name);
        if (key.empty() && eq == std::string::npos && name.starts_with("no")) {
            key = resolve(name.substr(2));
            flag = false;
        }
        if (key.empty()) {
            vim_.say("E518: Unknown option: " + name);
            return;
        }
        if (key[0] == '?') {
            vim_.say("E518: Which one: " + key.substr(1));
            return;
        }
        const ConfigValue now = config_get(cfg_, key);
        if (ask || (eq == std::string::npos && now.kind != ConfigValue::kBool)) {
            std::string shown;
            switch (now.kind) {
                case ConfigValue::kBool: shown = now.flag ? "true" : "false"; break;
                case ConfigValue::kNumber: {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%g", now.number);
                    shown = buf;
                    break;
                }
                case ConfigValue::kText: shown = toml_quote(now.text); break;
                default:
                    for (const std::string& s : now.list) shown += (shown.empty() ? "" : ",") + s;
                    break;
            }
            said += (said.empty() ? "" : "  ") + key + "=" + shown;
            continue;
        }
        std::string toml;
        if (now.kind == ConfigValue::kBool) {
            toml = eq == std::string::npos ? (flag ? "true" : "false") : value;
        } else if (now.kind == ConfigValue::kNumber) {
            toml = value;
        } else if (now.kind == ConfigValue::kText) {
            toml = value.size() >= 2 && value.front() == '"' && value.back() == '"' ? value : toml_quote(value);
        } else {
            toml = "[";
            for (size_t from = 0; from <= value.size() && !value.empty();) {
                const size_t comma = std::min(value.find(',', from), value.size());
                toml += (toml.size() > 1 ? ", " : "") + toml_quote(value.substr(from, comma - from));
                from = comma + 1;
            }
            toml += "]";
        }
        const std::string wrong = script_set(key, toml, ":set");
        if (!wrong.empty()) {
            vim_.say("E474: " + wrong.substr(0, wrong.find('\n')));
            return;
        }
    }
    if (!said.empty()) vim_.say(said);
}

// The finder box a script asked for has an answer: an item (by its
// place, from 0), what was typed (blit.ui.input), or nothing.
void App::script_chose(bool chosen, int index, const std::string& typed) {
    if (!script_ || !script_->L || script_->pick_fn == LUA_NOREF) return;
    ScriptUi& s = *script_;
    lua_State* L = s.L;
    const int fn = s.pick_fn, items = s.pick_items;
    s.pick_fn = s.pick_items = LUA_NOREF;
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
    luaL_unref(L, LUA_REGISTRYINDEX, fn);
    if (!chosen) {
        lua_pushnil(L);
        lua_pushnil(L);
    } else if (items == LUA_NOREF) {
        lua_pushlstring(L, typed.data(), typed.size());
        lua_pushnil(L);
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, items);
        lua_rawgeti(L, -1, index + 1);
        lua_remove(L, -2);
        lua_pushinteger(L, index + 1);
    }
    if (items != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, items);
    script_call(2, 0, items == LUA_NOREF ? "blit.ui.input" : "blit.ui.select");
}

// ---------------------------------------------------------------- the loop

// Timers that are due, mapping keys that waited too long, events, boxes
// that are to go, a fresh start that was asked for.
void App::script_tick() {
    ScriptUi& s = *script_;
    if (s.reload && s.depth == 0) {
        script_restart();
        return;
    }
    if (!s.L) {
        // (Only messages: their boxes go when their time is up.)
        script_expire();
        return;
    }
    // (The clock is read only when something here keeps time: this runs after every key.)
    if (!s.pending.empty() && now_ns() >= s.pending_until) script_match(true);
    if (!script_ || !script_->L) return;
    if (!s.backlog.empty()) script_jobs();
    // Timers: each due one once, in the order they are due.
    if (!s.timers.empty()) {
        const u64 now = now_ns();
        std::vector<std::pair<u64, u64>> when;
        for (const ScriptUi::Timer& t : s.timers)
            if (t.due <= now) when.push_back({t.due, t.id});
        std::sort(when.begin(), when.end());
        for (const auto& [at, id] : when) {
            if (!script_ || !script_->L) return;
            auto it = std::find_if(s.timers.begin(), s.timers.end(), [&](const ScriptUi::Timer& t) { return t.id == id; });
            if (it == s.timers.end()) continue;
            const int fn = it->fn;
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, fn);
            if (it->every) {
                it->due += it->every;
                if (it->due <= now) it->due = now + it->every;  // (late: on from now, not all the missed ones at once)
            } else {
                s.timers.erase(it);
                luaL_unref(s.L, LUA_REGISTRYINDEX, fn);
            }
            script_call(0, 0, "timer");
        }
    }
    if (!script_ || !script_->L) return;
    script_tick_queue();
    if (!script_ || !script_->L) return;
    script_watch();
    script_expire();
}

// Events that came in the middle of something.
void App::script_tick_queue() {
    while (script_ && script_->L && !script_->queue.empty()) {
        std::vector<ScriptUi::Queued> q;
        q.swap(script_->queue);
        for (const ScriptUi::Queued& e : q) {
            if (!script_ || !script_->L) return;
            script_fire(e.event, e.doc, e.file, e.match);
        }
    }
}

// Events noticed by looking: another document in front, another mode, the
// text or the cursor changed. And boxes that go when the cursor moves.
void App::script_watch() {
    ScriptUi& s = *script_;
    const Doc& d = *pane_->doc;
    const char mode = script_mode_char();
    if (!s.floats.empty()) {
        const size_t had = s.floats.size();
        std::erase_if(s.floats, [&](const ScriptUi::Float& f) {
            if (f.close == kCloseNever) return false;
            if (f.doc != d.id) return true;
            return f.close == kCloseMove && f.cursor != d.ed.cursor();
        });
        if (s.floats.size() != had) dirty_ = true;
    }
    if (!(s.mask & kWatched)) return;
    if (!s.seen) {
        s.seen = true;
        s.seen_doc = d.id;
        s.seen_rev = s.seen_rev_i = d.ed.revision();
        s.seen_cursor = d.ed.cursor();
        if (!script_modes_) s.seen_mode = mode;
        return;
    }
    const u64 id = d.id, rev = d.ed.revision(), cur = d.ed.cursor();
    const std::string path = d.ed.path;
    if (id != s.seen_doc) {
        const u64 old = s.seen_doc;
        s.seen_doc = id, s.seen_rev = s.seen_rev_i = rev, s.seen_cursor = cur;
        if (s.mask & bit(kEvBufLeave))
            if (Doc* o = find_doc(old)) script_fire(kEvBufLeave, old, o->ed.path, o->ed.path);
        if (!script_ || !script_->L) return;
        script_fire(kEvBufEnter, id, path, path);
        return;
    }
    if (script_modes_ && mode != s.seen_mode) {  // (a change Vim was not asked for by a key: the mouse)
        script_mode_seen();
        script_tick_queue();
        if (!script_ || !script_->L) return;
    }
    // (As Vim keeps them: TextChangedI against the last time it was said in
    // Insert mode, TextChanged against the last time in any other, so that
    // what was typed is a change once Insert mode is left.)
    const bool typing = mode == 'i' || mode == 'R';
    u64& last = typing ? s.seen_rev_i : s.seen_rev;
    if (rev != last) {
        last = rev;
        script_fire(typing ? kEvTextChangedI : kEvTextChanged, id, path, path);
        if (!script_ || !script_->L) return;
    }
    if (cur != s.seen_cursor) {
        s.seen_cursor = cur;
        script_fire(typing ? kEvCursorMovedI : kEvCursorMoved, id, path, path);
    }
}

void App::script_expire() {
    ScriptUi& s = *script_;
    if (s.toasts.empty() && s.floats.empty()) return;
    const u64 now = now_ns();
    const size_t toasts = s.toasts.size(), floats = s.floats.size();
    std::erase_if(s.toasts, [&](const ScriptUi::Toast& t) { return t.until && now >= t.until; });
    std::erase_if(s.floats, [&](const ScriptUi::Float& f) { return f.until && now >= f.until; });
    if (s.toasts.size() != toasts || s.floats.size() != floats) dirty_ = true;
}

// How long the loop may sleep before something here is due (-1: as long as it likes).
int App::script_wait_ms() const {
    const ScriptUi& s = *script_;
    if (s.reload || !s.queue.empty() || !s.backlog.empty()) return 0;
    u64 next = ~0ull;
    if (!s.pending.empty()) next = std::min(next, s.pending_until);
    for (const ScriptUi::Timer& t : s.timers) next = std::min(next, t.due);
    for (const ScriptUi::Toast& t : s.toasts)
        if (t.until) next = std::min(next, t.until);
    for (const ScriptUi::Float& f : s.floats)
        if (f.until) next = std::min(next, f.until);
    if (next == ~0ull) return -1;
    const u64 now = now_ns();
    return next <= now ? 0 : static_cast<int>(std::min<u64>((next - now + 999'999) / 1'000'000, 1'000'000));
}

// What background programs have said.
void App::script_jobs() {
    if (!script_ || !script_->L) return;
    ScriptUi& s = *script_;
    std::vector<JobEvent> events = std::move(s.backlog);
    s.backlog.clear();
    if (s.jobs)
        for (JobEvent& e : s.jobs->take()) events.push_back(std::move(e));
    for (JobEvent& e : events) {
        if (!script_ || !script_->L) return;
        if (auto mine = s.native_jobs.find(e.id); mine != s.native_jobs.end()) {  // (blit's own: only how it ended)
            if (e.kind != JobEvent::kExit) continue;
            auto done = std::move(mine->second);
            s.native_jobs.erase(mine);
            done(e.code, e.out, e.err);
            continue;
        }
        auto it = s.job_calls.find(e.id);
        if (it == s.job_calls.end()) continue;
        ScriptUi::JobCalls calls = it->second;
        lua_State* L = s.L;
        if (e.kind == JobEvent::kOut || e.kind == JobEvent::kErr) {
            const int fn = e.kind == JobEvent::kOut ? calls.out : calls.err;
            if (fn == LUA_NOREF) continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
            lua_pushlstring(L, e.out.data(), e.out.size());
            script_call(1, 0, e.kind == JobEvent::kOut ? "stdout" : "stderr");
            continue;
        }
        s.job_calls.erase(it);
        s.unref(calls.out);
        s.unref(calls.err);
        if (calls.exit == LUA_NOREF) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, calls.exit);
        luaL_unref(L, LUA_REGISTRYINDEX, calls.exit);
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, e.code);
        lua_setfield(L, -2, "code");
        lua_pushlstring(L, e.out.data(), e.out.size());
        lua_setfield(L, -2, "stdout");
        lua_pushlstring(L, e.err.data(), e.err.size());
        lua_setfield(L, -2, "stderr");
        script_call(1, 0, "blit.system");
    }
}

// At the end: VimLeavePre, then everything goes.
void App::script_stop() {
    if (!script_) return;
    if (script_->L && (script_->mask & bit(kEvVimLeavePre))) script_fire(kEvVimLeavePre, pane_->doc->id, pane_->doc->ed.path, {});
    script_shutdown();
}

void App::script_close_floats() {
    if (!script_ || script_->floats.empty()) return;
    std::erase_if(script_->floats, [](const ScriptUi::Float& f) { return f.close == kCloseEsc || f.close == kCloseMove; });
    dirty_ = true;
}

// All a script call changes in a document is one undo step.
void App::script_group(Doc& d) {
    ScriptUi& s = *script_;
    if (s.depth == 0 || std::find(s.grouped.begin(), s.grouped.end(), d.id) != s.grouped.end()) return;
    d.ed.begin_group();
    s.grouped.push_back(d.id);
}

// Replaces bytes of a document for a script, the cursor kept where it was
// in the text around.
void App::script_replace(Doc& d, u64 start, u64 end, std::string_view text) {
    script_group(d);
    Editor& ed = d.ed;
    const u64 cur = ed.cursor(), anchor = ed.anchor();
    auto moved = [&](u64 at) {
        if (at < start) return at;
        if (at >= end) return at - (end - start) + text.size();
        return start;
    };
    ed.replace(start, end, text);
    const u64 c = std::min(moved(cur), ed.buf.size()), a = std::min(moved(anchor), ed.buf.size());
    ed.move_to(a, false);
    if (c != a) ed.move_to(c, true);
    for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
    dirty_ = true;
}

// ---------------------------------------------------------------- drawing

void App::script_layout(int w, int h, FrameDesc& fd) {
    ScriptUi& s = *script_;
    Font& font = *font_;
    const Look lk = look();
    const int cw = font.cell_w(), ch = font.cell_h(), pad_x = lk.edge, pad_y = lk.pad_y, gap = lk.gap;
    const float border = static_cast<float>(lk.hair);

    // Marks in each view of a document that has them.
    if (!s.marks.empty())
        for (u32 v = 0; v < fd.view_count && v < kMaxViews; ++v) {
            GridSpec& g = fd.views[v].grid;
            auto it = s.marks.find(g.doc);
            if (it == s.marks.end() || it->second.empty()) continue;
            Doc* d = find_doc(g.doc);
            if (!d) continue;
            const Editor& ed = d->ed;
            std::vector<GridDeco>& decos = s.view_decos[v];
            std::vector<GridNote>& notes = s.view_notes[v];
            decos.assign(g.decos.begin(), g.decos.end());
            notes.assign(g.notes.begin(), g.notes.end());
            const size_t had_notes = notes.size();
            const u64 lines = ed.buf.line_count();
            std::vector<ScriptUi::Mark>& all = it->second;
            if (s.marks_unsorted.erase(g.doc))
                std::stable_sort(all.begin(), all.end(), [](const ScriptUi::Mark& x, const ScriptUi::Mark& y) { return x.line < y.line; });
            // Only those on the lines in view are looked at.
            auto first = std::lower_bound(all.begin(), all.end(), g.top_line, [](const ScriptUi::Mark& x, u64 l) { return x.line < l; });
            for (auto mi = first; mi != all.end() && mi->line < g.top_line + g.rows; ++mi) {
                ScriptUi::Mark& m = *mi;
                if (m.line >= lines) break;
                const u64 start = ed.buf.line_start(m.line), end = ed.line_end(m.line);
                if (m.kind) {
                    const u64 a = std::min<u64>(start + m.col0, end), b = std::min<u64>(start + std::max(m.col1, m.col0 + 1), end);
                    const u32 c0 = ed.col_between(start, a), c1 = std::max(ed.col_between(start, b), c0 + 1);
                    decos.push_back({m.line, c0, c1, m.kind});
                }
                if (!m.note.empty()) {
                    // One note to a line: the first there, a language server's before a script's.
                    bool taken = notes.size() > had_notes && notes.back().line == m.line;  // (the marks come by line)
                    for (size_t k = 0; k < had_notes && !taken; ++k) taken = notes[k].line == m.line;
                    if (taken) continue;
                    if (m.font != font.generation()) {
                        m.font = font.generation();
                        // (Upright: an italic face may be a script hand.)
                        const std::string text = fit_text(m.note, 200);
                        TextBox box;
                        box.resize(std::max<u32>(width_of(text), 1), 1);
                        box.put(font, 0, 0, text, m.note_ink);
                        m.cells = std::move(box.cells);
                    }
                    notes.push_back({m.line, ed.col_between(start, end) + 3, m.cells.data(), static_cast<u32>(m.cells.size())});
                }
            }
            g.decos = decos;
            g.notes = notes;
        }

    // Floating boxes.
    for (ScriptUi::Float& f : s.floats) {
        if (fd.popup_count >= kMaxPopups) break;
        if (!f.until && f.timeout_ns) f.until = now_ns() + f.timeout_ns;
        u32 widest = width_of(f.title);
        for (const std::string& l : f.lines) widest = std::max(widest, width_of(l));
        const u32 room_cols = static_cast<u32>(std::max(10, (w * 9 / 10 - 2 * pad_x) / cw));
        const u32 room_rows = static_cast<u32>(std::max(2, (h * 7 / 10 - 2 * pad_y) / ch));
        const u32 cols = std::clamp<u32>(f.width ? f.width : widest, 1, room_cols);
        const u32 title_rows = f.title.empty() ? 0 : 1;
        const u32 rows = std::clamp<u32>(f.height ? f.height : static_cast<u32>(f.lines.size()) + title_rows, 1, room_rows);
        if (f.dirty || f.font != font.generation() || f.made_cols != cols || f.made_rows != rows) {
            f.dirty = false;
            f.font = font.generation();
            f.made_cols = cols;
            f.made_rows = rows;
            f.box.resize(cols, rows);
            // (What is wider than the box ends in an ellipsis.)
            auto fit = [cols](const std::string& t) { return width_of(t) > cols ? fit_text(t, cols) : t; };
            if (title_rows) put(f.box, font, 0, 0, fit(f.title), ink::kHeading);
            for (u32 r = title_rows; r < rows && r - title_rows < f.lines.size(); ++r) f.box.put(font, r, 0, fit(f.lines[r - title_rows]), f.ink);
        }
        Popup& p = fd.popups[fd.popup_count++];
        p.cols = cols;
        p.rows = rows;
        p.cells = f.box.cells.data();
        p.pad_x = pad_x;
        p.pad_y = pad_y;
        p.w = static_cast<int>(cols) * cw + 2 * pad_x;
        p.h = static_cast<int>(rows) * ch + 2 * pad_y;
        p.border_px = border;
        const int bottom = h - vim_px() - status_px();
        switch (f.at) {
            case kAtCenter:
                p.x = (w - p.w) / 2;
                p.y = std::max(0, (h - p.h) / 3);
                break;
            case kAtEditor:
                p.x = f.col >= 0 ? f.col * cw : w + f.col * cw - p.w;
                p.y = f.row >= 0 ? f.row * ch : h + f.row * ch - p.h;
                break;
            case kAtTopRight:
                p.x = w - p.w - gap;
                p.y = tabs_px() + gap;
                break;
            case kAtBottomRight:
                p.x = w - p.w - gap;
                p.y = bottom - p.h - gap;
                break;
            default: {  // under the cursor (over it if there is no room below)
                const Pane& pn = *pane_;
                const int cx = pn.text_x + pad_x_ + static_cast<int>(pn.cur_col) * cw - static_cast<int>(std::lround(pn.scroll_x));
                const int cy = pn.text_y + static_cast<int>(pn.cur_line) * ch - static_cast<int>(std::lround(pn.scroll_y));
                p.x = cx - pad_x;
                p.y = cy + ch + gap;
                if (p.y + p.h > bottom && cy - p.h - gap >= 0) p.y = cy - p.h - gap;
                break;
            }
        }
        p.x = std::clamp(p.x, 0, std::max(0, w - p.w));
        p.y = std::clamp(p.y, 0, std::max(0, h - p.h));
        fx_float(p, kFxScript);
    }

    // Messages, from the bottom right upward.
    int y = h - vim_px() - status_px() - gap;
    const u64 now = now_ns();
    for (size_t i = s.toasts.size(); i-- > 0;) {
        if (fd.popup_count >= kMaxPopups) break;
        ScriptUi::Toast& t = s.toasts[i];
        if (!t.until) t.until = now + (t.level == kError ? 10'000'000'000ull : t.level == kWarn ? 6'000'000'000ull : 4'000'000'000ull);
        if (t.font != font.generation()) {
            t.font = font.generation();
            // Wrapped at 60 columns; at most 12 lines, the last ending in an
            // ellipsis if there was more.
            std::vector<std::string> lines;
            bool more = false;
            for (const std::string& line : split_lines(t.text)) {
                if (lines.size() >= 12) {
                    more = more || !line.empty();
                    continue;
                }
                std::string_view rest = line;
                while (width_of(rest) > 60 && lines.size() < 12) {
                    size_t cut = 0;
                    u32 width = 0;
                    const u8* p = reinterpret_cast<const u8*>(rest.data());
                    while (cut < rest.size() && width < 60) {
                        cut += decode_utf8(p + cut, rest.size() - cut).len;
                        ++width;
                    }
                    const size_t space = rest.substr(0, cut).rfind(' ');
                    if (space != std::string_view::npos && space > 20) cut = space + 1;
                    lines.emplace_back(rest.substr(0, cut));
                    rest.remove_prefix(cut);
                }
                if (lines.size() < 12) lines.emplace_back(rest);
                else more = more || !rest.empty();
            }
            while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
            if (more) lines.back() += "\xE2\x80\xA6";
            u32 widest = 1;
            for (const std::string& l : lines) widest = std::max(widest, width_of(l));
            t.box.resize(widest, static_cast<u32>(lines.size()));
            const u32 ink = t.level == kError ? kInkDiagnostic : t.level == kWarn ? kInkDiagnostic + 1 : 0;
            for (u32 r = 0; r < lines.size(); ++r) t.box.put(font, r, 0, lines[r], ink);
        }
        Popup& p = fd.popups[fd.popup_count++];
        p.cols = t.box.cols;
        p.rows = t.box.rows;
        p.cells = t.box.cells.data();
        p.pad_x = pad_x;
        p.pad_y = pad_y;
        p.w = static_cast<int>(p.cols) * cw + 2 * pad_x;
        p.h = static_cast<int>(p.rows) * ch + 2 * pad_y;
        p.x = std::max(0, w - p.w - gap);
        y -= p.h;
        p.y = std::max(0, y);
        y -= gap;
        p.border_px = border;
        fx_float(p, kFxToast);
    }
}

bool App::script_busy() const {
    if (!script_ || !script_->L) return false;
    const ScriptUi& s = *script_;
    if (!s.pending.empty() || !s.queue.empty() || !s.backlog.empty() || (s.jobs && s.jobs->running() > 0) || !s.job_calls.empty() || !s.native_jobs.empty()) return true;
    for (const ScriptUi::Timer& t : s.timers)
        if (!t.every) return true;
    return false;
}

// Where things stand, for the tests: the mode, the bottom line, the
// mappings, commands and handlers there are, and the boxes on show.
std::string App::script_dump() {
    if (!script_) return "no scripts";
    ScriptUi& s = *script_;
    std::string out = std::string(s.L ? "lua" : "no lua") + " | maps " + std::to_string(s.maps.all().size()) + " | commands " + std::to_string(s.commands.size()) +
                      " | handlers " + std::to_string(s.handlers.size()) + " | timers " + std::to_string(s.timers.size()) + " | floats " + std::to_string(s.floats.size());
    for (const ScriptUi::Float& f : s.floats) out += " [" + (f.lines.empty() ? std::string() : f.lines[0]) + "]";
    out += " | toasts " + std::to_string(s.toasts.size());
    return out;
}

}  // namespace blit
