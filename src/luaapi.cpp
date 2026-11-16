// The functions scripts call: the table `blit` and what is in it. How the
// scripts are run, and why nothing here lets Lua raise an error while one
// of its variables is alive, is in src/applua.cpp.
#include "applua.hpp"
#include "fsio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "finder.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

// ---------------------------------------------------------------- the API

struct ScriptApi {
    static App& app(lua_State* L) { return *app_of(L); }
    static App::ScriptUi& ui(lua_State* L) { return *app_of(L)->script_; }

    static void hook(lua_State* L, lua_Debug*) {
        App::ScriptUi& s = ui(L);
        if (s.deadline && now_ns() > s.deadline) {
            s.deadline = now_ns() + 50'000'000;  // (what catches the error has a moment to say so)
            lua_Debug ar;
            if (lua_getstack(L, 0, &ar) && lua_getinfo(L, "Sl", &ar) && ar.currentline > 0)
                lua_pushfstring(L, "%s:%d: stopped: a script ran for longer than script.timeout_ms", ar.short_src, ar.currentline);
            else
                lua_pushliteral(L, "stopped: a script ran for longer than script.timeout_ms");
            lua_error(L);
        }
    }

    // The document a script means: a number, or nil or 0 for the one in front.
    static Doc* doc_at(lua_State* L, int i) {
        App& a = app(L);
        i64 id = 0;
        if (!lua_isnoneornil(L, i) && !int_at(L, i, &id)) return nullptr;
        if (id == 0) return a.pane_->doc;
        return a.find_doc(static_cast<u64>(id));
    }
    // A line number of a script's (from 1; negative from the end, -1 the last) as one from 0.
    static bool line_at(const Editor& ed, i64 n, bool past_end, u64* out) {
        const i64 count = static_cast<i64>(ed.buf.line_count());
        if (n < 0) n = count + 1 + n;
        if (n < 1 || n > count + (past_end ? 1 : 0)) return false;
        *out = static_cast<u64>(n - 1);
        return true;
    }

    // Before the window is up (init.lua): what needs it is done once it is.
    static int later(lua_State* L, char what) {
        ui(L).later.emplace_back(what, std::string(str_at(L, 1)));
        return 0;
    }

    // blit._opt_keys()
    static int opt_keys(lua_State* L) {
        const auto keys = config_keys();
        lua_createtable(L, static_cast<int>(keys.size()), 0);
        for (size_t i = 0; i < keys.size(); ++i) {
            lua_pushstring(L, keys[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }
    // blit._opt_get(key)
    static int opt_get(lua_State* L) {
        if (!is_str(L, 1)) return fail("a setting's name is wanted");
        push_value(L, config_get(app(L).cfg_, str_at(L, 1)));
        return 1;
    }
    // blit._opt_set(key, value, where)
    static int opt_set(lua_State* L) {
        if (!is_str(L, 1)) return fail("a setting's name is wanted");
        std::string value;
        if (!toml_of(L, 2, &value)) return fail("%s: a setting is true or false, a number, a string or a list of strings", lua_tostring(L, 1));
        const std::string wrong = app(L).script_set(std::string(str_at(L, 1)), value, is_str(L, 3) ? std::string(str_at(L, 3)) : std::string("script"));
        if (!wrong.empty()) {
            std::string said = wrong.substr(0, wrong.size() - (wrong.ends_with('\n') ? 1 : 0));
            const std::string from = is_str(L, 3) ? std::string(str_at(L, 3)) + ": " : std::string();
            if (!from.empty() && said.starts_with(from)) said.erase(0, from.size());  // (the error is raised with its place)
            return fail("%s", said.c_str());
        }
        return 0;
    }

    // blit.notify(message, level)
    static int notify(lua_State* L) {
        size_t n = 0;
        const char* s = luaL_tolstring(L, 1, &n);  // (before anything of ours is alive)
        int level = kInfo;
        if (is_str(L, 2)) {
            const std::string_view l = str_at(L, 2);
            level = l == "error" ? kError : (l == "warn" || l == "warning") ? kWarn : kInfo;
        } else if (lua_type(L, 2) == LUA_TNUMBER) {
            level = std::clamp(static_cast<int>(lua_tointeger(L, 2)), 0, 2);
        }
        app(L).notify(std::string(s, n), level);
        return 0;
    }

    // blit.cmd(line): a command line, as typed after ":".
    static int cmd(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.cmd: a command line is wanted");
        App& a = app(L);
        if (!a.font_) return later(L, 'c');
        {
            const std::string line(str_at(L, 1));
            if (a.cfg_.modal) a.vim_attach();
            a.vim_.say({});
            a.vim_.ex(a.pane_->doc->ed, line);
            a.vim_after(Vim::kHandled);
        }
        const std::string& said = a.vim_.message();
        if (said.size() > 2 && said[0] == 'E' && std::isdigit(static_cast<u8>(said[1]))) return fail("%s", said.c_str());
        return 0;
    }
    // blit.normal(keys): keys for Vim, as they are (no mappings).
    static int normal(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.normal: keys are wanted");
        App& a = app(L);
        if (!a.font_) return later(L, 'n');
        if (!a.cfg_.modal) return fail("blit.normal: Vim's modes are off (editor.modal)");
        a.vim_attach();
        a.vim_.keys(a.pane_->doc->ed, str_at(L, 1));
        a.vim_after(Vim::kHandled);
        return 0;
    }
    // blit.feed(keys, remap): keys as if typed.
    static int feed(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.feed: keys are wanted");
        const bool remap = lua_toboolean(L, 2);
        App& a = app(L);
        if (!a.font_) return later(L, remap ? 'F' : 'f');
        std::string err;
        if (!a.script_feed(str_at(L, 1), remap, &err)) return fail("blit.feed: %s", err.c_str());
        return 0;
    }
    static int mode(lua_State* L) {
        const char m = app(L).script_mode_char();
        switch (m) {
            case 'n': lua_pushliteral(L, "n"); break;
            case 'o': lua_pushliteral(L, "no"); break;
            case 'i': lua_pushliteral(L, "i"); break;
            case 'R': lua_pushliteral(L, "R"); break;
            case 'v': lua_pushliteral(L, "v"); break;
            case 'V': lua_pushliteral(L, "V"); break;
            case 'b': lua_pushliteral(L, "\x16"); break;
            default: lua_pushliteral(L, "c"); break;
        }
        return 1;
    }
    static int count(lua_State* L) {
        lua_pushinteger(L, ui(L).count);
        return 1;
    }

    // Keys: blit._map(modes, lhs, rhs, remap, desc)
    static int map(lua_State* L) {
        if (!is_str(L, 1) || !is_str(L, 2)) return fail("blit.map: a mode and keys are wanted");
        if (!is_str(L, 3) && !lua_isfunction(L, 3)) return fail("blit.map: what the keys do is keys (a string) or a function");
        const u8 modes = Keymaps::modes_of(str_at(L, 1));
        if (!modes) return fail("blit.map: unknown mode \"%s\" (n, i, v, x, o, c)", lua_tostring(L, 1));
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        int fn = LUA_NOREF;
        if (lua_isfunction(L, 3)) {
            lua_pushvalue(L, 3);
            fn = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        Keymaps::Map m;
        std::string err;
        if (!parse_keys(str_at(L, 2), a.cfg_.key_leader, &m.lhs, &err) || m.lhs.empty()) {
            s.unref(fn);
            return fail("blit.map: %s", err.empty() ? "no keys" : err.c_str());
        }
        m.modes = modes;
        m.fn = fn;
        if (is_str(L, 3)) {
            m.keys = std::string(str_at(L, 3));
            std::vector<MapKey> check;
            if (!parse_keys(m.keys, a.cfg_.key_leader, &check, &err)) return fail("blit.map: %s", err.c_str());
        }
        m.remap = lua_toboolean(L, 4);
        if (is_str(L, 5)) m.desc = std::string(str_at(L, 5));
        for (int gone : s.maps.set(std::move(m))) s.unref(gone);
        a.script_keys_ = true;
        return 0;
    }
    // blit._unmap(modes, lhs)
    static int unmap(lua_State* L) {
        if (!is_str(L, 1) || !is_str(L, 2)) return fail("blit.unmap: a mode and keys are wanted");
        const u8 modes = Keymaps::modes_of(str_at(L, 1));
        if (!modes) return fail("blit.unmap: unknown mode \"%s\"", lua_tostring(L, 1));
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        std::vector<MapKey> lhs;
        if (!parse_keys(str_at(L, 2), a.cfg_.key_leader, &lhs)) return fail("blit.unmap: unknown keys");
        for (int gone : s.maps.remove(modes, lhs)) s.unref(gone);
        a.script_keys_ = !s.maps.empty() || !s.pending.empty();
        return 0;
    }
    // blit._maps(): { {mode=, lhs=, rhs=, desc=}, ... }
    static int maps(lua_State* L) {
        const auto& all = ui(L).maps.all();
        lua_createtable(L, static_cast<int>(all.size()), 0);
        int i = 0;
        for (const Keymaps::Map& m : all) {
            lua_createtable(L, 0, 4);
            std::string modes;
            for (const auto& [bit_, letter] : {std::pair{Keymaps::kNormal, 'n'}, {Keymaps::kInsert, 'i'}, {Keymaps::kVisual, 'v'}, {Keymaps::kPending, 'o'}, {Keymaps::kCommand, 'c'}})
                if (m.modes & bit_) modes += letter;
            lua_pushlstring(L, modes.data(), modes.size());
            lua_setfield(L, -2, "mode");
            const std::string lhs = key_notation(m.lhs);
            lua_pushlstring(L, lhs.data(), lhs.size());
            lua_setfield(L, -2, "lhs");
            if (m.fn >= 0) lua_rawgeti(L, LUA_REGISTRYINDEX, m.fn);
            else lua_pushlstring(L, m.keys.data(), m.keys.size());
            lua_setfield(L, -2, "rhs");
            lua_pushlstring(L, m.desc.data(), m.desc.size());
            lua_setfield(L, -2, "desc");
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    // Commands: blit._command(name, rhs, opts)
    static int command(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.command: a name is wanted");
        const std::string_view name = str_at(L, 1);
        if (name.empty() || !std::isupper(static_cast<u8>(name[0]))) return fail("blit.command: a command's name starts with a capital letter");
        for (const char c : name)
            if (!std::isalnum(static_cast<u8>(c)) && c != '_') return fail("blit.command: letters and digits only in a command's name");
        if (!is_str(L, 2) && !lua_isfunction(L, 2)) return fail("blit.command: what it does is a command line (a string) or a function");
        App::ScriptUi& s = ui(L);
        int fn = LUA_NOREF;
        if (lua_isfunction(L, 2)) {
            lua_pushvalue(L, 2);
            fn = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        App::ScriptUi::Command c;
        c.name = std::string(name);
        c.fn = fn;
        if (is_str(L, 2)) c.ex = std::string(str_at(L, 2));
        c.desc = field_str(L, 3, "desc");
        if (field(L, 3, "nargs") == LUA_TNUMBER || lua_type(L, -1) == LUA_TSTRING) c.nargs = std::string(str_at(L, -1));
        lua_pop(L, 1);
        c.bang = field_bool(L, 3, "bang", false);
        c.range = field_bool(L, 3, "range", false);
        for (App::ScriptUi::Command& have : s.commands)
            if (have.name == c.name) {
                s.unref(have.fn);
                have = std::move(c);
                return 0;
            }
        s.commands.push_back(std::move(c));
        return 0;
    }
    static int delcommand(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.delcommand: a name is wanted");
        App::ScriptUi& s = ui(L);
        const std::string_view name = str_at(L, 1);
        for (size_t i = 0; i < s.commands.size(); ++i)
            if (s.commands[i].name == name) {
                s.unref(s.commands[i].fn);
                s.commands.erase(s.commands.begin() + static_cast<std::ptrdiff_t>(i));
                return 0;
            }
        return 0;
    }

    // Events: blit._on(events, pattern, once, group, fn) -> id
    static int on(lua_State* L) {
        if (!is_str(L, 1) || !lua_isfunction(L, 5)) return fail("blit.on: an event's name and a function are wanted");
        u32 events = 0;
        const std::string_view names = str_at(L, 1);
        for (size_t at = 0; at <= names.size();) {
            const size_t comma = std::min(names.find(',', at), names.size());
            std::string_view one = names.substr(at, comma - at);
            while (!one.empty() && one.front() == ' ') one.remove_prefix(1);
            while (!one.empty() && one.back() == ' ') one.remove_suffix(1);
            if (!one.empty()) {
                const int e = event_of(one);
                if (e < 0) return fail("blit.on: unknown event \"%.*s\"", static_cast<int>(one.size()), one.data());
                events |= bit(static_cast<u32>(e));
            }
            at = comma + 1;
        }
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        lua_pushvalue(L, 5);
        App::ScriptUi::Handler h;
        h.fn = luaL_ref(L, LUA_REGISTRYINDEX);
        h.id = s.next_handler++;
        h.events = events;
        if (is_str(L, 2)) h.pattern = std::string(str_at(L, 2));
        h.once = lua_toboolean(L, 3);
        if (is_str(L, 4)) h.group = std::string(str_at(L, 4));
        const u64 id = h.id;
        s.handlers.push_back(std::move(h));
        a.script_mask();
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    }
    // blit.off(id or group)
    static int off(lua_State* L) {
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        i64 id = 0;
        const bool by_id = lua_type(L, 1) == LUA_TNUMBER && int_at(L, 1, &id);
        if (!by_id && !is_str(L, 1)) return fail("blit.off: a handler's number or a group's name is wanted");
        const std::string group = by_id ? std::string() : std::string(str_at(L, 1));
        for (size_t i = 0; i < s.handlers.size();) {
            if (by_id ? s.handlers[i].id == static_cast<u64>(id) : s.handlers[i].group == group) {
                s.unref(s.handlers[i].fn);
                s.handlers.erase(s.handlers.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        a.script_mask();
        return 0;
    }
    // blit.emit(name, data): the User event, now.
    static int emit(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.emit: a name is wanted");
        App& a = app(L);
        const std::string name(str_at(L, 1));
        a.script_fire(kEvUser, a.pane_->doc->id, a.pane_->doc->ed.path, name, lua_isnoneornil(L, 2) ? 0 : 2);
        return 0;
    }

    // Timers: blit._timer(ms, every_ms, fn) -> id
    static int timer(lua_State* L) {
        i64 ms = 0, every = 0;
        if (!opt_int(L, 1, 0, &ms) || !opt_int(L, 2, 0, &every) || !lua_isfunction(L, 3) || ms < 0 || every < 0)
            return fail("blit.defer: milliseconds (a whole number) and a function are wanted");
        App::ScriptUi& s = ui(L);
        lua_pushvalue(L, 3);
        App::ScriptUi::Timer t;
        t.fn = luaL_ref(L, LUA_REGISTRYINDEX);
        t.id = s.next_timer++;
        t.due = now_ns() + static_cast<u64>(ms) * 1'000'000;
        t.every = static_cast<u64>(every) * 1'000'000;
        const u64 id = t.id;
        s.timers.push_back(t);
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    }
    static int stop(lua_State* L) {
        i64 id = 0;
        if (!int_at(L, 1, &id)) return fail("blit.stop: a timer's number is wanted");
        App::ScriptUi& s = ui(L);
        for (size_t i = 0; i < s.timers.size(); ++i)
            if (s.timers[i].id == static_cast<u64>(id)) {
                s.unref(s.timers[i].fn);
                s.timers.erase(s.timers.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        return 0;
    }

    // Programs: blit.system(argv, opts, on_exit). With on_exit: the job's
    // number, and on_exit({code, stdout, stderr}) when it is done. Without:
    // it is waited for, and that table returned.
    static int system(lua_State* L) {
        if (!lua_istable(L, 1) || lua_rawlen(L, 1) == 0) return fail("blit.system: a list of the program and its arguments is wanted");
        if (!lua_isnoneornil(L, 2) && !lua_istable(L, 2)) return fail("blit.system: the options are a table");
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        JobSpec spec;
        spec.argv = strings_at(L, 1);
        if (spec.argv.size() != lua_rawlen(L, 1)) return fail("blit.system: the program and its arguments are strings");
        spec.cwd = field_str(L, 2, "cwd");
        spec.input = field_str(L, 2, "stdin");
        if (field(L, 2, "env") == LUA_TTABLE) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_type(L, -2) == LUA_TSTRING && (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)) {
                    std::string var(str_at(L, -2));
                    var += '=';
                    lua_pushvalue(L, -1);  // (a copy: tolstring on a number in place would upset lua_next)
                    var += str_at(L, -1);
                    lua_pop(L, 1);
                    spec.env.push_back(std::move(var));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        const i64 timeout_ms = field_int(L, 2, "timeout", 0);
        const std::string program = spec.argv.front();
        App::ScriptUi::JobCalls calls;
        if (field(L, 2, "stdout") == LUA_TFUNCTION) calls.out = luaL_ref(L, LUA_REGISTRYINDEX), spec.stream_out = true;
        else lua_pop(L, 1);
        if (field(L, 2, "stderr") == LUA_TFUNCTION) calls.err = luaL_ref(L, LUA_REGISTRYINDEX), spec.stream_err = true;
        else lua_pop(L, 1);
        if (!s.jobs) {
            s.jobs = std::make_unique<Jobs>();
            s.jobs->set_notify([] {
                SDL_Event ev{};
                ev.type = SDL_EVENT_USER;
                ev.user.code = 11;
                SDL_PushEvent(&ev);
            });
        }
        if (lua_isfunction(L, 3)) {
            lua_pushvalue(L, 3);
            calls.exit = luaL_ref(L, LUA_REGISTRYINDEX);
            const u64 id = s.jobs->start(std::move(spec));
            s.job_calls[id] = calls;
            lua_pushinteger(L, static_cast<lua_Integer>(id));
            return 1;
        }
        // Waited for, here and now: no longer than it may, and what other
        // jobs say meanwhile is kept for later.
        s.unref(calls.out);
        s.unref(calls.err);
        spec.stream_out = spec.stream_err = false;
        const u64 id = s.jobs->start(std::move(spec));
        const u64 limit = now_ns() + static_cast<u64>(timeout_ms > 0 ? timeout_ms : static_cast<i64>(a.cfg_.script_timeout_ms)) * 1'000'000;
        JobEvent done;
        bool finished = false;
        while (!finished) {
            for (JobEvent& e : s.jobs->take()) {
                if (e.id == id && e.kind == JobEvent::kExit) done = std::move(e), finished = true;
                else s.backlog.push_back(std::move(e));
            }
            if (finished) break;
            if (now_ns() > limit || (s.deadline && now_ns() > s.deadline)) {
                s.jobs->stop(id);
                return fail("blit.system: %s did not finish in time (it was stopped)", program.c_str());
            }
            SDL_Delay(1);
        }
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, done.code);
        lua_setfield(L, -2, "code");
        lua_pushlstring(L, done.out.data(), done.out.size());
        lua_setfield(L, -2, "stdout");
        lua_pushlstring(L, done.err.data(), done.err.size());
        lua_setfield(L, -2, "stderr");
        return 1;
    }
    static int kill(lua_State* L) {
        i64 id = 0;
        if (!int_at(L, 1, &id)) return fail("blit.kill: a job's number is wanted");
        App::ScriptUi& s = ui(L);
        lua_pushboolean(L, s.jobs && s.jobs->stop(static_cast<u64>(id)));
        return 1;
    }

    // Documents.
    static int buf(lua_State* L) {
        lua_pushinteger(L, static_cast<lua_Integer>(app(L).pane_->doc->id));
        return 1;
    }
    static int bufs(lua_State* L) {
        App& a = app(L);
        lua_newtable(L);
        int i = 0;
        for (const std::unique_ptr<Pane>& p : a.panes_)
            for (const std::unique_ptr<Doc>& d : p->docs) {
                lua_pushinteger(L, static_cast<lua_Integer>(d->id));
                lua_rawseti(L, -2, ++i);
            }
        return 1;
    }
    static int info(lua_State* L) {
        Doc* d = doc_at(L, 1);
        if (!d) return fail("blit.info: no such buffer");
        App& a = app(L);
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, static_cast<lua_Integer>(d->id));
        lua_setfield(L, -2, "id");
        lua_pushlstring(L, d->ed.path.data(), d->ed.path.size());
        lua_setfield(L, -2, "path");
        const std::string name = d->title.empty() ? (d->ed.path.empty() ? std::string("untitled") : base_name(d->ed.path)) : d->title;
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, d->ed.modified());
        lua_setfield(L, -2, "modified");
        lua_pushboolean(L, d->read_only);
        lua_setfield(L, -2, "readonly");
        lua_pushinteger(L, static_cast<lua_Integer>(d->ed.buf.line_count()));
        lua_setfield(L, -2, "lines");
        lua_pushinteger(L, static_cast<lua_Integer>(d->ed.buf.size()));
        lua_setfield(L, -2, "bytes");
        const std::string lang = a.doc_language(*d);
        lua_pushlstring(L, lang.data(), lang.size());
        lua_setfield(L, -2, "language");
        return 1;
    }
    // blit.lines(buf, first, last): the lines first to last (from 1, both in; -1 the last).
    static int lines(lua_State* L) {
        Doc* d = doc_at(L, 1);
        i64 first = 1, last = -1;
        if (!d) return fail("blit.lines: no such buffer");
        if (!opt_int(L, 2, 1, &first) || !opt_int(L, 3, -1, &last)) return fail("blit.lines: line numbers are whole numbers");
        const Editor& ed = d->ed;
        u64 l0, l1;
        if (!line_at(ed, first, true, &l0) || !line_at(ed, last, false, &l1)) {
            lua_newtable(L);  // (out of the text: nothing)
            return 1;
        }
        lua_createtable(L, l1 >= l0 ? static_cast<int>(std::min<u64>(l1 - l0 + 1, 1u << 20)) : 0, 0);
        std::string text;
        for (u64 l = l0; l <= l1 && l1 >= l0; ++l) {
            const u64 start = ed.buf.line_start(l);
            text = ed.buf.text(start, ed.line_end(l) - start);
            lua_pushlstring(L, text.data(), text.size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(l - l0 + 1));
        }
        return 1;
    }
    // blit.set_lines(buf, first, last, lines): lines first to last become
    // these; last = first - 1 puts them in before first.
    static int set_lines(lua_State* L) {
        Doc* d = doc_at(L, 1);
        i64 first = 0, last = 0;
        if (!d) return fail("blit.set_lines: no such buffer");
        if (!int_at(L, 2, &first) || !int_at(L, 3, &last)) return fail("blit.set_lines: line numbers are whole numbers");
        if (!lua_istable(L, 4)) return fail("blit.set_lines: the lines are a list of strings");
        if (d->read_only) return fail("blit.set_lines: this buffer is for reading");
        App& a = app(L);
        Editor& ed = d->ed;
        const i64 count = static_cast<i64>(ed.buf.line_count());
        if (first < 0) first = count + 1 + first;
        if (last < 0) last = count + 1 + last;
        if (first < 1 || first > count + 1 || last < first - 1 || last > count) return fail("blit.set_lines: lines %lld to %lld are not in a text of %lld lines", static_cast<long long>(first), static_cast<long long>(last), static_cast<long long>(count));
        // The lines joined, straight from the table (what is not a string or a number is passed over).
        std::string text;
        bool none = true;
        for (lua_Unsigned i = 1, n = lua_rawlen(L, 4); i <= n; ++i) {
            const int type = lua_rawgeti(L, 4, static_cast<lua_Integer>(i));
            if (type == LUA_TSTRING || type == LUA_TNUMBER) {
                size_t len = 0;
                const char* p = lua_tolstring(L, -1, &len);
                if (!none) text += '\n';
                text.append(p, len);
                none = false;
            }
            lua_pop(L, 1);
        }
        const u64 f0 = static_cast<u64>(first - 1);
        u64 start, end;
        if (last >= first) {  // lines replaced
            const u64 l1 = static_cast<u64>(last - 1);
            start = ed.buf.line_start(f0);
            if (l1 + 1 < ed.buf.line_count()) {
                end = ed.buf.line_start(l1 + 1);
                if (!none) text += '\n';
            } else {
                end = ed.buf.size();
                if (none && f0 > 0) start -= 1;  // the last lines go, with the line break before them
            }
        } else if (f0 < ed.buf.line_count()) {  // put in before line `first`
            start = end = ed.buf.line_start(f0);
            if (none) return 0;
            text += '\n';
        } else {  // after the last line
            start = end = ed.buf.size();
            if (none) return 0;
            text = '\n' + text;
        }
        // Only what differs is changed: the bytes the old and the new have
        // in common at either end stay (and so do marks and the cursor in
        // them, and the change a language server is told of is small).
        std::string_view t = text;
        const u64 old_len = end - start;
        u64 same_front = 0;
        for (Buffer::Reader r = ed.buf.reader(start); same_front < old_len && same_front < t.size() && !r.at_end();) {
            const std::string_view c = r.chunk();
            const size_t k = static_cast<size_t>(std::min<u64>({c.size(), old_len - same_front, t.size() - same_front}));
            const size_t eq = static_cast<size_t>(std::mismatch(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(k), t.begin() + static_cast<std::ptrdiff_t>(same_front)).first - c.begin());
            same_front += eq;
            if (eq < k) break;
            r.advance(static_cast<u32>(k));
        }
        u64 same_back = 0;
        char tail[4096];
        while (same_front + same_back < old_len && same_front + same_back < t.size()) {
            const u64 k = std::min<u64>({sizeof tail, old_len - same_front - same_back, t.size() - same_front - same_back});
            ed.buf.read(end - same_back - k, tail, k);
            u64 eq = 0;
            while (eq < k && tail[k - 1 - eq] == t[t.size() - 1 - same_back - eq]) ++eq;
            same_back += eq;
            if (eq < k) break;
        }
        if (same_front + same_back == old_len && same_front + same_back == t.size()) return 0;  // no change at all
        a.script_replace(*d, start + same_front, end - same_back, t.substr(same_front, t.size() - same_front - same_back));
        return 0;
    }
    static int text(lua_State* L) {
        Doc* d = doc_at(L, 1);
        if (!d) return fail("blit.text: no such buffer");
        const std::string all = d->ed.buf.text(0, d->ed.buf.size());
        lua_pushlstring(L, all.data(), all.size());
        return 1;
    }
    // blit.insert(text): at the cursor, as if typed (over what is selected).
    static int insert(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.insert: text is wanted");
        App& a = app(L);
        if (a.pane_->doc->read_only) return fail("blit.insert: this buffer is for reading");
        a.script_group(*a.pane_->doc);
        a.pane_->doc->ed.insert(str_at(L, 1));
        a.scroll_to_cursor();
        a.dirty_ = true;
        return 0;
    }
    // blit.cursor(): line and column (both from 1; the column in bytes).
    static int cursor(lua_State* L) {
        const Editor& ed = app(L).pane_->doc->ed;
        const u64 at = ed.cursor(), line = ed.buf.line_of(at);
        lua_pushinteger(L, static_cast<lua_Integer>(line + 1));
        lua_pushinteger(L, static_cast<lua_Integer>(at - ed.buf.line_start(line) + 1));
        return 2;
    }
    static int set_cursor(lua_State* L) {
        i64 line = 0, col = 1;
        if (!int_at(L, 1, &line) || !opt_int(L, 2, 1, &col)) return fail("blit.set_cursor: a line and a column are wanted");
        App& a = app(L);
        Editor& ed = a.pane_->doc->ed;
        u64 l;
        if (!line_at(ed, line, false, &l)) return fail("blit.set_cursor: there is no line %lld", static_cast<long long>(line));
        const u64 start = ed.buf.line_start(l), end = ed.line_end(l);
        u64 at = start + static_cast<u64>(std::max<i64>(col, 1) - 1);
        at = std::min(at, end);
        while (at > start && at < ed.buf.size() && ed.prev_char(ed.next_char(at)) != at) --at;  // (not inside a character)
        ed.move_to(at, false);
        if (a.cfg_.modal) a.vim_mouse();
        a.pane_->cache_rev = ~0ull;
        a.scroll_to_cursor();
        a.dirty_ = true;
        return 0;
    }
    // blit.selection(): what is selected, and where (line, column, line, column; the end left out), or nil.
    static int selection(lua_State* L) {
        App& a = app(L);
        const Editor& ed = a.pane_->doc->ed;
        u64 lo = ed.sel_min(), hi = ed.sel_max();
        if (!a.vim_selection(&lo, &hi) && lo == hi) {
            lua_pushnil(L);
            return 1;
        }
        const std::string text = ed.buf.text(lo, hi - lo);
        lua_pushlstring(L, text.data(), text.size());
        const u64 l0 = ed.buf.line_of(lo), l1 = ed.buf.line_of(hi);
        lua_pushinteger(L, static_cast<lua_Integer>(l0 + 1));
        lua_pushinteger(L, static_cast<lua_Integer>(lo - ed.buf.line_start(l0) + 1));
        lua_pushinteger(L, static_cast<lua_Integer>(l1 + 1));
        lua_pushinteger(L, static_cast<lua_Integer>(hi - ed.buf.line_start(l1) + 1));
        return 5;
    }
    static int open(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.open: a path is wanted");
        App& a = app(L);
        if (!a.font_) return later(L, 'o');
        {
            std::string path(str_at(L, 1));
            if (path.starts_with("~/") && SDL_getenv("HOME")) path = std::string(SDL_getenv("HOME")) + path.substr(1);
            if (!path.empty() && path[0] != '/') {
                const std::string* root = a.panel_root();
                path = (root ? *root : a.default_workdir()) + "/" + path;
            }
            if (!a.open_path(path.c_str())) return fail("blit.open: cannot open %s", path.c_str());
        }
        lua_pushinteger(L, static_cast<lua_Integer>(a.pane_->doc->id));
        return 1;
    }
    static int save(lua_State* L) {
        Doc* d = doc_at(L, 1);
        if (!d) return fail("blit.save: no such buffer");
        App& a = app(L);
        Pane* where = nullptr;
        size_t at = 0;
        Pane* const was = a.pane_;
        const u64 front = was->doc->id;
        if (!a.find_doc(d->id, &where, &at)) return fail("blit.save: no such buffer");
        a.focus_pane(where);
        a.tab_show(at);
        a.save();
        if (Doc* back = a.find_doc(front, &where, &at)) {
            (void)back;
            a.focus_pane(where);
            a.tab_show(at);
        }
        return 0;
    }
    static int close(lua_State* L) {
        Doc* d = doc_at(L, 1);
        if (!d) return fail("blit.close: no such buffer");
        const bool force = lua_toboolean(L, 2);
        App& a = app(L);
        Pane* where = nullptr;
        size_t at = 0;
        if (!a.find_doc(d->id, &where, &at)) return 0;
        a.focus_pane(where);
        lua_pushboolean(L, a.tab_close(at, force));
        return 1;
    }
    static int workdir(lua_State* L) {
        App& a = app(L);
        const std::string* root = a.panel_root();
        const std::string dir = root ? *root : a.default_workdir();
        lua_pushlstring(L, dir.data(), dir.size());
        return 1;
    }
    static int set_workdir(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.set_workdir: a folder is wanted");
        App& a = app(L);
        const std::string dir = full_path(std::string(str_at(L, 1)));
        if (kind_of(dir) != std::string_view("dir")) return fail("blit.set_workdir: %s is not a folder", dir.c_str());
        a.panel_set_workdir(dir);
        return 0;
    }
    static int fs_list(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.fs.list: a folder is wanted");
        const std::vector<DirEntry> all = list_dir(std::string(str_at(L, 1)));
        lua_createtable(L, static_cast<int>(all.size()), 0);
        for (size_t i = 0; i < all.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushlstring(L, all[i].name.data(), all[i].name.size());
            lua_setfield(L, -2, "name");
            lua_pushstring(L, all[i].dir ? "dir" : "file");
            lua_setfield(L, -2, "type");
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }
    // blit.fs.read(path [, most]) -> text, or nil and why; blit.fs.write(path, text) and
    // blit.fs.mkdir(path) -> true, or nil and why. (Another machine's paths too: ssh://host/...)
    static int fs_read(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.fs.read: a path is wanted");
        const std::string path(str_at(L, 1));
        const u64 most = static_cast<u64>(std::max<lua_Integer>(1, luaL_optinteger(L, 2, 16 << 20)));
        std::string text;
        if (!::blit::fs_read(path, &text, most)) {
            lua_pushnil(L);
            lua_pushstring(L, ("cannot read " + path).c_str());
            return 2;
        }
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    static int fs_write(lua_State* L) {
        if (!is_str(L, 1) || !is_str(L, 2)) return fail("blit.fs.write: a path and the text are wanted");
        const std::string path(str_at(L, 1));
        std::string err;
        if (!::blit::fs_write(path, str_at(L, 2), &err)) {
            lua_pushnil(L);
            lua_pushstring(L, ("cannot write " + path + ": " + err).c_str());
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    static int fs_mkdir(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.fs.mkdir: a path is wanted");
        const std::string path(str_at(L, 1));
        std::string err;
        if (!fs_mkdirs(path, &err)) {
            lua_pushnil(L);
            lua_pushstring(L, ("cannot make " + path + ": " + err).c_str());
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    // ------------------------------------------------ blit.build

    static void push_task(lua_State* L, const BuildTask& t) {
        lua_createtable(L, 0, 5);
        lua_pushlstring(L, t.system.data(), t.system.size());
        lua_setfield(L, -2, "system");
        lua_pushlstring(L, t.name.data(), t.name.size());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, t.cmd.data(), t.cmd.size());
        lua_setfield(L, -2, "cmd");
        lua_pushlstring(L, t.cwd.data(), t.cwd.size());
        lua_setfield(L, -2, "cwd");
        lua_pushboolean(L, t.def);
        lua_setfield(L, -2, "default");
    }
    // blit.build.add(name, {detect = file(s) | function(root, files), tasks = list | function(root, files), first = bool})
    static int build_add(lua_State* L) {
        if (!is_str(L, 1) || !lua_istable(L, 2)) return fail("blit.build.add: a name and a table are wanted");
        App::ScriptUi& s = ui(L);
        App::ScriptUi::BuildHook h;
        h.name = std::string(str_at(L, 1));
        const int dt = field(L, 2, "detect");
        if (dt == LUA_TFUNCTION) {
            h.detect = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            if (dt == LUA_TSTRING) h.files.emplace_back(str_at(L, -1));
            else if (dt == LUA_TTABLE) h.files = strings_at(L, -1);
            lua_pop(L, 1);
        }
        const int tt = field(L, 2, "tasks");
        if (tt == LUA_TFUNCTION) {
            h.tasks_fn = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            h.tasks = tasks_at(L, -1, h.name);
            lua_pop(L, 1);
        }
        h.first = field_bool(L, 2, "first", false);
        const char* wrong = h.detect == LUA_NOREF && h.files.empty() ? "blit.build.add: detect is a file's name, a list of them, or a function"
                            : h.tasks_fn == LUA_NOREF && h.tasks.empty() ? "blit.build.add: tasks is a list ({name =, cmd =} each) or a function"
                                                                         : nullptr;
        if (wrong) {
            s.unref(h.detect);
            s.unref(h.tasks_fn);
            return fail("%s", wrong);
        }
        std::erase(s.build_hidden, h.name);
        bool replaced = false;
        for (App::ScriptUi::BuildHook& have : s.build_hooks)
            if (have.name == h.name) {
                s.unref(have.detect);
                s.unref(have.tasks_fn);
                have = std::move(h);
                replaced = true;
                break;
            }
        if (!replaced) s.build_hooks.push_back(std::move(h));
        app(L).build_changed();
        return 0;
    }
    // blit.build.remove(name): one of init.lua's, or a built-in system, no longer looked for.
    static int build_remove(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.build.remove: a name is wanted");
        App::ScriptUi& s = ui(L);
        const std::string name(str_at(L, 1));
        for (size_t i = 0; i < s.build_hooks.size(); ++i)
            if (s.build_hooks[i].name == name) {
                s.unref(s.build_hooks[i].detect);
                s.unref(s.build_hooks[i].tasks_fn);
                s.build_hooks.erase(s.build_hooks.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        if (std::find(s.build_hidden.begin(), s.build_hidden.end(), name) == s.build_hidden.end()) s.build_hidden.push_back(name);
        app(L).build_changed();
        return 0;
    }
    // blit.build.systems() -> the names of the systems looked for.
    static int build_list_systems(lua_State* L) {
        App::ScriptUi& s = ui(L);
        lua_newtable(L);
        lua_Integer i = 0;
        for (const App::ScriptUi::BuildHook& h : s.build_hooks)
            if (h.first) lua_pushlstring(L, h.name.data(), h.name.size()), lua_rawseti(L, -2, ++i);
        for (const std::string& n : build_systems())
            if (std::find(s.build_hidden.begin(), s.build_hidden.end(), n) == s.build_hidden.end()) lua_pushlstring(L, n.data(), n.size()), lua_rawseti(L, -2, ++i);
        for (const App::ScriptUi::BuildHook& h : s.build_hooks)
            if (!h.first) lua_pushlstring(L, h.name.data(), h.name.size()), lua_rawseti(L, -2, ++i);
        return 1;
    }
    // blit.build.tasks() -> the Build tab's tasks ({system, name, cmd, cwd, default} each).
    static int build_list_tasks(lua_State* L) {
        const std::vector<BuildTask> tasks = app(L).build_tasks();
        lua_createtable(L, static_cast<int>(tasks.size()), 0);
        for (size_t i = 0; i < tasks.size(); ++i) {
            push_task(L, tasks[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }
    // blit.build.run([name]): the task of that name ("Build", or "CMake: Build"), or the one the Run button runs.
    static int build_run(lua_State* L) {
        const std::string name = is_str(L, 1) ? std::string(str_at(L, 1)) : std::string();
        lua_pushboolean(L, app(L).build_run_named(name));
        return 1;
    }

    // ------------------------------------------------ blit.project

    // blit.project.add(name, {description =, fields = {...}, create = function(dir, values)})
    static int project_add(lua_State* L) {
        if (!is_str(L, 1) || !lua_istable(L, 2)) return fail("blit.project.add: a name and a table are wanted");
        App::ScriptUi& s = ui(L);
        App::ScriptUi::ProjectHook h;
        h.name = std::string(str_at(L, 1));
        h.description = field_str(L, 2, "description");
        if (field(L, 2, "fields") == LUA_TTABLE) {
            const int t = lua_gettop(L);
            const lua_Unsigned n = lua_rawlen(L, t);
            for (lua_Unsigned i = 1; i <= n; ++i) {
                if (lua_rawgeti(L, t, static_cast<lua_Integer>(i)) == LUA_TTABLE) {
                    ProjectField f;
                    f.key = field_str(L, -1, "key");
                    f.label = field_str(L, -1, "label", f.key);
                    const std::string kind = field_str(L, -1, "kind", "text");
                    f.kind = kind == "switch" || kind == "bool" ? ProjectField::kSwitch : kind == "choice" ? ProjectField::kChoice : ProjectField::kText;
                    if (field(L, -1, "choices") == LUA_TTABLE) f.choices = strings_at(L, -1);
                    lua_pop(L, 1);
                    const int dt = field(L, -1, "default");
                    if (dt == LUA_TBOOLEAN) f.def = lua_toboolean(L, -1) ? "true" : "false";
                    else if (dt == LUA_TSTRING || dt == LUA_TNUMBER) f.def = std::string(str_at(L, -1));
                    lua_pop(L, 1);
                    if (f.kind == ProjectField::kSwitch && f.def != "true") f.def = "false";
                    if (f.kind == ProjectField::kChoice && f.def.empty() && !f.choices.empty()) f.def = f.choices[0];
                    if (field(L, -1, "when") == LUA_TTABLE) {
                        lua_pushnil(L);
                        if (lua_next(L, -2)) {  // (its first pair: key = value, or key = {values})
                            if (lua_type(L, -2) == LUA_TSTRING) f.when_key = std::string(str_at(L, -2));
                            if (lua_type(L, -1) == LUA_TTABLE) f.when_values = strings_at(L, -1);
                            else if (lua_type(L, -1) == LUA_TBOOLEAN) f.when_values = {lua_toboolean(L, -1) ? "true" : "false"};
                            else if (lua_type(L, -1) == LUA_TSTRING) f.when_values = {std::string(str_at(L, -1))};
                            lua_pop(L, 2);
                        }
                    }
                    lua_pop(L, 1);
                    if (!f.key.empty() && (f.kind != ProjectField::kChoice || !f.choices.empty())) h.fields.push_back(std::move(f));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        if (field(L, 2, "create") != LUA_TFUNCTION) {
            lua_pop(L, 1);
            return fail("blit.project.add: create = function(dir, values) is wanted");
        }
        h.create = luaL_ref(L, LUA_REGISTRYINDEX);
        std::erase(s.project_hidden, h.name);
        for (App::ScriptUi::ProjectHook& have : s.project_hooks)
            if (have.name == h.name) {
                s.unref(have.create);
                have = std::move(h);
                return 0;
            }
        s.project_hooks.push_back(std::move(h));
        return 0;
    }
    // blit.project.remove(name): one of init.lua's, or a built-in kind, no longer offered.
    static int project_remove(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.project.remove: a name is wanted");
        App::ScriptUi& s = ui(L);
        const std::string name(str_at(L, 1));
        for (size_t i = 0; i < s.project_hooks.size(); ++i)
            if (s.project_hooks[i].name == name) {
                s.unref(s.project_hooks[i].create);
                s.project_hooks.erase(s.project_hooks.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        if (std::find(s.project_hidden.begin(), s.project_hidden.end(), name) == s.project_hidden.end()) s.project_hidden.push_back(name);
        return 0;
    }
    static int fs_exists(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.fs.exists: a path is wanted");
        const char* k = kind_of(std::string(str_at(L, 1)));
        if (k) lua_pushstring(L, k);
        else lua_pushnil(L);
        return 1;
    }

    // Marks: blit.ns(name) -> number; blit.mark(buf, ns, line, col_start, col_end, opts); blit.clear_marks(buf, ns)
    static int ns(lua_State* L) {
        if (!is_str(L, 1)) return fail("blit.ns: a name is wanted");
        App::ScriptUi& s = ui(L);
        const std::string_view name = str_at(L, 1);
        size_t i = 0;
        while (i < s.namespaces.size() && s.namespaces[i] != name) ++i;
        if (i == s.namespaces.size()) s.namespaces.emplace_back(name);
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        return 1;
    }
    static int mark(lua_State* L) {
        Doc* d = doc_at(L, 1);
        i64 ns = 0, line = 0, c0 = 1, c1 = 0;
        if (!d) return fail("blit.mark: no such buffer");
        if (!int_at(L, 2, &ns) || !int_at(L, 3, &line) || !opt_int(L, 4, 1, &c0) || !opt_int(L, 5, 0, &c1)) return fail("blit.mark: buffer, namespace, line, first column, last column");
        if (!lua_isnoneornil(L, 6) && !lua_istable(L, 6)) return fail("blit.mark: the options are a table");
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        u64 l;
        if (!line_at(d->ed, line, false, &l)) return fail("blit.mark: there is no line %lld", static_cast<long long>(line));
        App::ScriptUi::Mark m;
        m.ns = static_cast<u32>(std::max<i64>(ns, 0));
        m.line = l;
        m.col0 = static_cast<u32>(std::clamp<i64>(c0, 1, 1 << 30) - 1);
        m.col1 = static_cast<u32>(std::clamp<i64>(c1, 0, 1 << 30));  // (the last column, from 1, is where the mark ends, left out from 0)
        const std::string kind = field_str(L, 6, "kind", c1 > 0 ? "match" : "");
        m.kind = kind == "error" ? 1 : kind == "warning" || kind == "warn" ? 2 : kind == "info" ? 3 : kind == "hint" ? 4 : kind == "match" ? kDecoMatch : kind == "select" ? kDecoSelect : 0;
        m.note = field_str(L, 6, "note");
        m.note_ink = ink_named(field_str(L, 6, "note_ink", "muted"));
        if (!m.note_ink) m.note_ink = kInkMuted;
        if (!kind.empty() && !m.kind) return fail("blit.mark: unknown kind \"%s\" (match, select, error, warning, info, hint)", kind.c_str());
        std::vector<App::ScriptUi::Mark>& all = s.marks[d->id];
        if (!all.empty() && all.back().line > m.line) s.marks_unsorted.insert(d->id);  // (put in order when they are next drawn)
        all.push_back(std::move(m));
        a.dirty_ = true;
        return 0;
    }
    static int clear_marks(lua_State* L) {
        Doc* d = doc_at(L, 1);
        i64 ns = 0;
        if (!d) return fail("blit.clear_marks: no such buffer");
        if (!opt_int(L, 2, 0, &ns)) return fail("blit.clear_marks: a namespace's number is wanted");
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        auto it = s.marks.find(d->id);
        if (it == s.marks.end()) return 0;
        if (ns == 0) s.marks.erase(it);
        else std::erase_if(it->second, [&](const App::ScriptUi::Mark& m) { return m.ns == static_cast<u32>(ns); });
        a.dirty_ = true;
        return 0;
    }

    // Floating boxes: blit.ui.float(content, opts) -> id
    static bool content_at(lua_State* L, int i, std::vector<std::string>* out) {
        if (is_str(L, i)) *out = split_lines(str_at(L, i));
        else if (lua_istable(L, i)) *out = strings_at(L, i);
        else return false;
        return true;
    }
    static int float_open(lua_State* L) {
        std::vector<std::string> lines;
        if (!content_at(L, 1, &lines)) return fail("blit.ui.float: text, or a list of lines, is wanted");
        if (!lua_isnoneornil(L, 2) && !lua_istable(L, 2)) return fail("blit.ui.float: the options are a table");
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        App::ScriptUi::Float f;
        f.id = s.next_float++;
        f.lines = std::move(lines);
        f.title = field_str(L, 2, "title");
        const std::string at = field_str(L, 2, "at", "cursor");
        f.at = at == "center" ? kAtCenter : at == "editor" ? kAtEditor : at == "top-right" ? kAtTopRight : at == "bottom-right" ? kAtBottomRight : kAtCursor;
        f.row = static_cast<int>(field_int(L, 2, "row", 0));
        f.col = static_cast<int>(field_int(L, 2, "col", 0));
        f.width = static_cast<u32>(std::clamp<i64>(field_int(L, 2, "width", 0), 0, 400));
        f.height = static_cast<u32>(std::clamp<i64>(field_int(L, 2, "height", 0), 0, 200));
        const std::string close = field_str(L, 2, "close", "esc");
        f.close = close == "move" ? kCloseMove : close == "never" ? kCloseNever : kCloseEsc;
        f.ink = ink_named(field_str(L, 2, "ink"));
        f.timeout_ns = static_cast<u64>(std::max<i64>(field_int(L, 2, "timeout", 0), 0)) * 1'000'000;
        f.doc = a.pane_->doc->id;
        f.cursor = a.pane_->doc->ed.cursor();
        const int id = f.id;
        s.floats.push_back(std::move(f));
        a.dirty_ = true;
        lua_pushinteger(L, id);
        return 1;
    }
    static int float_set(lua_State* L) {
        i64 id = 0;
        std::vector<std::string> lines;
        if (!int_at(L, 1, &id) || !content_at(L, 2, &lines)) return fail("blit.ui.set: a box's number and text are wanted");
        App& a = app(L);
        for (App::ScriptUi::Float& f : ui(L).floats)
            if (f.id == id) {
                f.lines = std::move(lines);
                f.dirty = true;
                a.dirty_ = true;
                lua_pushboolean(L, 1);
                return 1;
            }
        lua_pushboolean(L, 0);
        return 1;
    }
    static int float_close(lua_State* L) {
        i64 id = 0;
        if (!int_at(L, 1, &id)) return fail("blit.ui.close: a box's number is wanted");
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        const size_t had = s.floats.size();
        std::erase_if(s.floats, [&](const App::ScriptUi::Float& f) { return f.id == id; });
        a.dirty_ = true;
        lua_pushboolean(L, s.floats.size() != had);
        return 1;
    }
    // blit._select(items, shown, prompt, fn)
    static int select(lua_State* L) {
        if (!lua_istable(L, 1) || !lua_istable(L, 2) || !lua_isfunction(L, 4)) return fail("blit.ui.select: a list of items and a function are wanted");
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        std::vector<std::string> shown = strings_at(L, 2);
        const std::string prompt = is_str(L, 3) ? std::string(str_at(L, 3)) : std::string("Pick");
        a.find_pick(std::move(shown), prompt, false, {});  // (a pick that was open is answered "nothing" first)
        s.unref(s.pick_fn);
        s.unref(s.pick_items);
        lua_pushvalue(L, 4);
        s.pick_fn = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, 1);
        s.pick_items = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }
    // blit._input(prompt, default, fn)
    static int input(lua_State* L) {
        if (!lua_isfunction(L, 3)) return fail("blit.ui.input: a function is wanted");
        App& a = app(L);
        App::ScriptUi& s = ui(L);
        const std::string prompt = is_str(L, 1) ? std::string(str_at(L, 1)) : std::string("Input");
        const std::string typed = is_str(L, 2) ? std::string(str_at(L, 2)) : std::string();
        a.find_pick({}, prompt, true, typed);
        s.unref(s.pick_fn);
        s.unref(s.pick_items);
        lua_pushvalue(L, 3);
        s.pick_fn = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }
    static int status(lua_State* L) {
        App& a = app(L);
        if (lua_isnoneornil(L, 1)) a.script_status_.clear();
        else if (is_str(L, 1)) a.script_status_ = std::string(str_at(L, 1));
        else return fail("blit.ui.status: text is wanted");
        a.dirty_ = true;
        return 0;
    }
    static int messages(lua_State* L) {
        const std::vector<std::string>& all = ui(L).messages;
        lua_createtable(L, static_cast<int>(all.size()), 0);
        for (size_t i = 0; i < all.size(); ++i) {
            lua_pushlstring(L, all[i].data(), all[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }
    static int reload(lua_State* L) {
        ui(L).reload = true;
        return 0;
    }
    // blit._load(path): runs a file now (for plugins), errors shown and not raised.
    static int load(lua_State* L) {
        if (!is_str(L, 1)) return fail("a file is wanted");
        App& a = app(L);
        lua_pushboolean(L, a.script_file(std::string(str_at(L, 1))));
        return 1;
    }
};

namespace {


// Calls F, and raises the error it found (once F's variables are gone),
// with the place in the script that called: the first one on the stack that
// is not blit's own Lua.
template <int (*F)(lua_State*)>
int api(lua_State* L) {
    const int n = F(L);
    if (n >= 0) return n;
    lua_Debug ar;
    for (int level = 1; lua_getstack(L, level, &ar); ++level) {
        lua_getinfo(L, "Sl", &ar);
        if (ar.currentline > 0 && std::strcmp(ar.source, "=blit") != 0) {
            lua_pushfstring(L, "%s:%d: %s", ar.short_src, ar.currentline, g_err);
            return lua_error(L);
        }
    }
    lua_pushstring(L, g_err);
    return lua_error(L);
}

}  // namespace

namespace script {

const luaL_Reg kTop[] = {
    {"_opt_keys", api<ScriptApi::opt_keys>}, {"_opt_get", api<ScriptApi::opt_get>}, {"_opt_set", api<ScriptApi::opt_set>},
    {"notify", api<ScriptApi::notify>},      {"cmd", api<ScriptApi::cmd>},          {"normal", api<ScriptApi::normal>},
    {"feed", api<ScriptApi::feed>},          {"mode", api<ScriptApi::mode>},        {"count", api<ScriptApi::count>},
    {"_map", api<ScriptApi::map>},           {"_unmap", api<ScriptApi::unmap>},     {"_maps", api<ScriptApi::maps>},
    {"_command", api<ScriptApi::command>},   {"delcommand", api<ScriptApi::delcommand>}, {"_on", api<ScriptApi::on>},
    {"off", api<ScriptApi::off>},            {"emit", api<ScriptApi::emit>},        {"_timer", api<ScriptApi::timer>},
    {"stop", api<ScriptApi::stop>},          {"system", api<ScriptApi::system>},    {"kill", api<ScriptApi::kill>},
    {"buf", api<ScriptApi::buf>},            {"bufs", api<ScriptApi::bufs>},        {"info", api<ScriptApi::info>},
    {"lines", api<ScriptApi::lines>},        {"set_lines", api<ScriptApi::set_lines>}, {"text", api<ScriptApi::text>},
    {"insert", api<ScriptApi::insert>},      {"cursor", api<ScriptApi::cursor>},    {"set_cursor", api<ScriptApi::set_cursor>},
    {"selection", api<ScriptApi::selection>}, {"open", api<ScriptApi::open>},       {"save", api<ScriptApi::save>},
    {"close", api<ScriptApi::close>},        {"workdir", api<ScriptApi::workdir>},  {"set_workdir", api<ScriptApi::set_workdir>},
    {"ns", api<ScriptApi::ns>},              {"mark", api<ScriptApi::mark>},        {"clear_marks", api<ScriptApi::clear_marks>},
    {"messages", api<ScriptApi::messages>},  {"reload", api<ScriptApi::reload>},    {"_load", api<ScriptApi::load>},
    {"_select", api<ScriptApi::select>},     {"_input", api<ScriptApi::input>},     {nullptr, nullptr}};
const luaL_Reg kUi[] = {{"float", api<ScriptApi::float_open>}, {"set", api<ScriptApi::float_set>}, {"close", api<ScriptApi::float_close>},
                        {"status", api<ScriptApi::status>},    {nullptr, nullptr}};
const luaL_Reg kFs[] = {{"list", api<ScriptApi::fs_list>},   {"exists", api<ScriptApi::fs_exists>}, {"read", api<ScriptApi::fs_read>},
                        {"write", api<ScriptApi::fs_write>}, {"mkdir", api<ScriptApi::fs_mkdir>},   {nullptr, nullptr}};
const luaL_Reg kBuild[] = {{"add", api<ScriptApi::build_add>},           {"remove", api<ScriptApi::build_remove>}, {"systems", api<ScriptApi::build_list_systems>},
                           {"tasks", api<ScriptApi::build_list_tasks>},  {"run", api<ScriptApi::build_run>},       {nullptr, nullptr}};
const luaL_Reg kProject[] = {{"add", api<ScriptApi::project_add>}, {"remove", api<ScriptApi::project_remove>}, {nullptr, nullptr}};

// The part of the API written in Lua. It is run with the table `blit` as
// its argument, before anything of the user's.
const char kBoot[] = R"LUA(
local blit = ...
local getinfo, traceback = debug.getinfo, debug.traceback

-- Where a setting was set: the first place on the stack that is not here.
local function where()
  for level = 3, 40 do
    local info = getinfo(level, "Sl")
    if not info then break end
    if info.source ~= "=blit" and info.what ~= "C" then
      return info.short_src .. ":" .. (info.currentline or 0)
    end
  end
  return "script"
end

-- blit.opt: every setting of the settings file, by the same names.
local keys, sections = {}, {}
for _, k in ipairs(blit._opt_keys()) do
  keys[k] = true
  local s = k:match("^(.*)%.[^.]+$")
  while s do sections[s] = true; s = s:match("^(.*)%.[^.]+$") end
end
local open_ended = { themes = true, ["lsp.servers"] = true, ["syntax.grammars"] = true, ["syntax.styles"] = true }
local function is_section(key)
  if sections[key] or open_ended[key] then return true end
  for prefix in pairs(open_ended) do
    if key:sub(1, #prefix + 1) == prefix .. "." then return true end
  end
  return false
end
local function set_value(key, value, at)
  if type(value) == "table" and value[1] == nil and next(value) ~= nil then
    for k, v in pairs(value) do set_value(key .. "." .. k, v, at) end
    return
  end
  blit._opt_set(key, value, at)
end
local made = {}
local function section(prefix)
  if prefix and made[prefix] then return made[prefix] end
  local t = setmetatable({}, {
    __index = function(_, k)
      local key = prefix and (prefix .. "." .. k) or k
      if keys[key] then return blit._opt_get(key) end
      if is_section(key) then return section(key) end
      return nil
    end,
    __newindex = function(_, k, v)
      set_value(prefix and (prefix .. "." .. k) or k, v, where())
    end,
  })
  if prefix then made[prefix] = t end
  return t
end
blit.opt = section(nil)
blit.g = {}

-- Text, for showing.
function blit.inspect(value, depth)
  local seen = {}
  local function show(v, indent, level)
    local t = type(v)
    if t == "string" then return string.format("%q", v) end
    if t ~= "table" then return tostring(v) end
    if seen[v] then return "<cycle>" end
    if level > (depth or 6) then return "{...}" end
    seen[v] = true
    local keys, n = {}, #v
    for k in pairs(v) do
      if not (math.type(k) == "integer" and k >= 1 and k <= n) then keys[#keys + 1] = k end
    end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    local parts = {}
    for i = 1, n do parts[#parts + 1] = show(v[i], indent .. "  ", level + 1) end
    for _, k in ipairs(keys) do
      local name = type(k) == "string" and k:match("^[%a_][%w_]*$") and k or "[" .. show(k, indent, level + 1) .. "]"
      parts[#parts + 1] = name .. " = " .. show(v[k], indent .. "  ", level + 1)
    end
    seen[v] = nil
    if #parts == 0 then return "{}" end
    local one = "{ " .. table.concat(parts, ", ") .. " }"
    if #one <= 72 and not one:find("\n") then return one end
    return "{\n" .. indent .. "  " .. table.concat(parts, ",\n" .. indent .. "  ") .. "\n" .. indent .. "}"
  end
  return show(value, "", 1)
end

print = function(...)
  local parts = {}
  for i = 1, select("#", ...) do parts[i] = tostring((select(i, ...))) end
  blit.notify(table.concat(parts, "\t"))
end

os.exit = function() blit.cmd("qa") end

function blit.split(s, sep, plain)
  sep = sep or "%s+"
  local out, at = {}, 1
  while true do
    local i, j = s:find(sep, at, plain)
    if not i then out[#out + 1] = s:sub(at); break end
    if j < i then j = i end
    out[#out + 1] = s:sub(at, i - 1)
    at = j + 1
  end
  return out
end

function blit.trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

-- :lua and :=
function blit._eval(code)
  local f = load("return " .. code, "=(:lua)")
  if f then
    local r = table.pack(f())
    if r.n > 0 then
      local out = {}
      for i = 1, r.n do out[i] = blit.inspect(r[i]) end
      print(table.concat(out, "\t"))
    end
    return
  end
  local g, err = load(code, "=(:lua)")
  if not g then error(err, 0) end
  g()
end

-- Keys, commands, events, timers.
local function modes_of(mode)
  if type(mode) == "table" then return table.concat(mode) end
  return mode or ""
end
function blit.map(mode, lhs, rhs, opts)
  opts = opts or {}
  local remap = opts.remap
  if remap == nil and opts.noremap ~= nil then remap = not opts.noremap end
  blit._map(modes_of(mode), lhs, rhs, remap or false, opts.desc)
end
function blit.unmap(mode, lhs) blit._unmap(modes_of(mode), lhs) end
function blit.maps() return blit._maps() end
function blit.command(name, rhs, opts) blit._command(name, rhs, opts or {}) end
function blit.on(events, opts, fn)
  if type(opts) == "function" then opts, fn = {}, opts end
  opts = opts or {}
  if type(events) == "table" then events = table.concat(events, ",") end
  local pattern = opts.pattern
  if type(pattern) == "table" then pattern = table.concat(pattern, ",") end
  return blit._on(events, pattern, opts.once or false, opts.group, fn)
end
function blit.group(name)
  blit.off(name)
  return name
end
function blit.defer(ms, fn) return blit._timer(ms, 0, fn) end
function blit.every(ms, fn) return blit._timer(ms, ms, fn) end
function blit.schedule(fn) return blit._timer(0, 0, fn) end

blit.ui = blit.ui or {}
function blit.ui.select(items, opts, on_choice)
  if type(opts) == "function" then opts, on_choice = {}, opts end
  opts = opts or {}
  local shown = {}
  for i, item in ipairs(items) do
    shown[i] = opts.format and tostring(opts.format(item)) or tostring(item)
  end
  blit._select(items, shown, opts.prompt or "Pick", on_choice or function() end)
end
function blit.ui.input(opts, on_confirm)
  if type(opts) == "function" then opts, on_confirm = {}, opts end
  opts = opts or {}
  blit._input(opts.prompt or "Input", opts.default or "", on_confirm or function() end)
end

-- Where modules are found: lua/ beside init.lua, then each plugin's lua/.
local plugdir = blit.data_dir .. "/plugins"
local paths = { blit.config_dir .. "/lua/?.lua", blit.config_dir .. "/lua/?/init.lua" }
for _, e in ipairs(blit.fs.list(plugdir)) do
  if e.type == "dir" then
    paths[#paths + 1] = plugdir .. "/" .. e.name .. "/lua/?.lua"
    paths[#paths + 1] = plugdir .. "/" .. e.name .. "/lua/?/init.lua"
  end
end
package.path = table.concat(paths, ";") .. ";" .. package.path

-- Plugins: folders under the data folder, each with plugin/*.lua run once
-- init.lua has been, and lua/ to require from. blit.plug() names the ones
-- :PlugInstall fetches (with git) and :PlugUpdate brings up to date.
local declared, loaded = {}, {}
local function run_dir(dir)
  for _, e in ipairs(blit.fs.list(dir)) do
    if e.type == "file" and e.name:match("%.lua$") then blit._load(dir .. "/" .. e.name) end
  end
end
local function load_plugin(name)
  if loaded[name] then return end
  loaded[name] = true
  local dir = plugdir .. "/" .. name
  local lua = dir .. "/lua"
  if blit.fs.exists(lua) == "dir" and not package.path:find(lua, 1, true) then
    package.path = lua .. "/?.lua;" .. lua .. "/?/init.lua;" .. package.path
  end
  run_dir(dir .. "/plugin")
end
function blit._plugins()
  run_dir(blit.config_dir .. "/plugin")
  for _, e in ipairs(blit.fs.list(plugdir)) do
    if e.type == "dir" then load_plugin(e.name) end
  end
end
function blit.plug(spec)
  if type(spec) == "string" then spec = { spec } end
  local src = spec[1] or spec.url
  if type(src) ~= "string" or src == "" then error("blit.plug: a plugin is \"owner/name\" (on GitHub) or a git URL", 2) end
  local url = src
  if not (src:match("^%a[%w+.-]*://") or src:match("^git@") or src:match("^/") or src:match("^~")) then
    url = "https://github.com/" .. src .. ".git"
  end
  local name = spec.name or src:gsub("/+$", ""):match("([^/:]+)$"):gsub("%.git$", "")
  declared[#declared + 1] = { name = name, url = url, rev = spec.rev }
  return name
end
local function git(args, cwd, done)
  blit.system(args, { cwd = cwd }, function(r) done(r.code == 0, blit.trim(r.stderr ~= "" and r.stderr or r.stdout)) end)
end
blit.command("PlugInstall", function()
  local wanted = 0
  for _, p in ipairs(declared) do
    local dir = plugdir .. "/" .. p.name
    if not blit.fs.exists(dir) then
      wanted = wanted + 1
      git({ "git", "clone", "--quiet", p.url, dir }, nil, function(ok, said)
        if ok and p.rev then
          git({ "git", "checkout", "--quiet", p.rev }, dir, function(ok2, said2)
            if ok2 then load_plugin(p.name); blit.notify("Installed " .. p.name .. " at " .. p.rev)
            else blit.notify(p.name .. ": " .. said2, "error") end
          end)
        elseif ok then
          load_plugin(p.name)
          blit.notify("Installed " .. p.name)
        else
          blit.notify(p.name .. ": " .. said, "error")
        end
      end)
    end
  end
  if wanted == 0 then blit.notify("Every plugin named with blit.plug() is installed") end
end, { desc = "Fetch the plugins named with blit.plug() that are not there yet" })
blit.command("PlugUpdate", function()
  local any = false
  for _, e in ipairs(blit.fs.list(plugdir)) do
    local dir = plugdir .. "/" .. e.name
    if e.type == "dir" and blit.fs.exists(dir .. "/.git") then
      any = true
      git({ "git", "pull", "--ff-only", "--quiet" }, dir, function(ok, said)
        blit.notify(e.name .. (ok and ": up to date" or (": " .. said)), ok and "info" or "error")
      end)
    end
  end
  if not any then blit.notify("No plugins to update") end
end, { desc = "Bring every plugin up to date (git pull); :Reload then uses them" })
blit.command("PlugList", function()
  local lines, seen = {}, {}
  for _, p in ipairs(declared) do
    seen[p.name] = true
    lines[#lines + 1] = (blit.fs.exists(plugdir .. "/" .. p.name) and "  " or "- ") .. p.name .. "  " .. p.url
  end
  for _, e in ipairs(blit.fs.list(plugdir)) do
    if e.type == "dir" and not seen[e.name] then lines[#lines + 1] = "  " .. e.name end
  end
  if #lines == 0 then lines[1] = "No plugins (blit.plug() names them; they go in " .. plugdir .. ")" end
  blit.ui.float(lines, { title = "Plugins (- not installed: :PlugInstall)", at = "center" })
end, { desc = "The plugins, and which are installed" })
blit.command("Reload", function() blit.reload() end, { desc = "Run init.lua and the plugins again from the start" })
)LUA";


const size_t kBootSize = sizeof kBoot - 1;

void count_hook(lua_State* L, lua_Debug* ar) { ScriptApi::hook(L, ar); }

}  // namespace script

}  // namespace blit
