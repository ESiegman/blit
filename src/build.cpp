#include "build.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string_view>

#include "json.hpp"
#include "probes.hpp"
#include "remote.hpp"
#include "simrun.hpp"

namespace blit {

namespace {

bool plain_word(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/' || c == ':'))
            return false;
    return true;
}

// For sh: as it is when it is a plain word, else in single quotes.
std::string sh_quote(std::string_view s) {
    if (plain_word(s)) return std::string(s);
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

template <class Fn>
void each_line(std::string_view text, Fn fn) {
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        fn(text.substr(0, nl));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
}

// What a system's finder is given: the folder and its entries.
struct Look {
    const std::string& root;
    const std::vector<FsEntry>& files;
    std::vector<BuildTask>& out;
    std::string system;

    const FsEntry* find(std::string_view name) const {
        for (const FsEntry& e : files)
            if (e.name == name) return &e;
        return nullptr;
    }
    bool file(std::string_view name) const {
        const FsEntry* e = find(name);
        return e && !e->dir;
    }
    bool dir(std::string_view name) const {
        const FsEntry* e = find(name);
        return e && e->dir;
    }
    std::string read(std::string_view name) const {
        std::string text;
        fs_read(root + "/" + std::string(name), &text);
        return text;
    }
    void add(std::string name, std::string cmd, bool def = false, std::string cwd = {}) {
        for (const BuildTask& t : out)
            if (t.system == system && t.name == name) return;
        BuildTask t;
        t.system = system;
        t.name = std::move(name);
        t.cmd = std::move(cmd);
        t.cwd = std::move(cwd);
        t.def = def;
        out.push_back(std::move(t));
    }
};

// ---------------------------------------------------------------- CMake

// A preset kind's lists, both files' (either file's presets may name, and
// inherit from, the other's).
using PresetLists = std::vector<json::Value>;

// A macro of CMake's still in it (${sourceParentDir}, $env{HOME},
// $penv{PATH}, $vendor{x}): what it stands for is not known here.
bool unexpanded(std::string_view dir) {
    for (size_t at = dir.find('$'); at != std::string_view::npos; at = dir.find('$', at + 1)) {
        size_t i = at + 1;
        while (i < dir.size() && std::isalpha(static_cast<unsigned char>(dir[i]))) ++i;
        if (i < dir.size() && dir[i] == '{') return true;
    }
    return false;
}

// A configure preset's binaryDir, through what it inherits, with the macros
// that can be known here filled in (${presetName} is the one asked about,
// not the one it inherits the binaryDir from); "" when one cannot be.
std::string preset_dir(const PresetLists& lists, std::string_view name, std::string_view asked = {}, int depth = 0) {
    if (depth > 8) return {};
    if (asked.empty()) asked = name;
    for (const json::Value& presets : lists)
        for (json::Value p : presets) {
            if (p["name"].str() != name) continue;
            std::string dir(p["binaryDir"].str());
            if (!dir.empty()) {
                auto put = [&](std::string_view macro, std::string_view with) {
                    for (size_t at; (at = dir.find(macro)) != std::string::npos;) dir.replace(at, macro.size(), with);
                };
                put("${sourceDir}/", "");
                put("${sourceDir}", ".");
                put("${presetName}", asked);
                return unexpanded(dir) ? std::string() : dir;
            }
            const json::Value up = p["inherits"];
            if (up.is_string()) return preset_dir(lists, up.str(), asked, depth + 1);
            for (json::Value u : up)
                if (std::string d = preset_dir(lists, u.str(), asked, depth + 1); !d.empty()) return d;
            return {};
        }
    return {};
}

// The configure preset a build preset builds, through what it inherits.
std::string build_configure(const PresetLists& lists, std::string_view name, int depth = 0) {
    if (depth > 8) return {};
    for (const json::Value& presets : lists)
        for (json::Value p : presets) {
            if (p["name"].str() != name) continue;
            if (!p["configurePreset"].str().empty()) return std::string(p["configurePreset"].str());
            const json::Value up = p["inherits"];
            if (up.is_string()) return build_configure(lists, up.str(), depth + 1);
            for (json::Value u : up)
                if (std::string conf = build_configure(lists, u.str(), depth + 1); !conf.empty()) return conf;
            return {};
        }
    return {};
}

void cmake(Look& c) {
    if (!c.file("CMakeLists.txt")) return;
    bool presets = false, built = false;
    std::vector<std::string> preset_dirs;
    // (A binaryDir may be outside the folder: an absolute one is on the
    // folder's machine, so another machine's starts with its ssh://host.)
    auto configured = [&](const std::string& dir) {
        std::string at = c.root + "/" + dir;
        if (dir.starts_with('/')) {
            at = dir;
            if (const size_t scheme = c.root.find("://"); scheme != std::string::npos) at = c.root.substr(0, c.root.find('/', scheme + 3)) + dir;
        }
        return fs_kind(at + "/CMakeCache.txt") == 1;
    };
    const char* const names[2] = {"CMakePresets.json", "CMakeUserPresets.json"};
    json::Doc docs[2];
    bool read[2] = {};
    PresetLists configures, builds;
    for (int i = 0; i < 2; ++i) {
        read[i] = c.file(names[i]) && docs[i].parse(c.read(names[i]));
        if (!read[i]) continue;
        configures.push_back(docs[i].root()["configurePresets"]);
        builds.push_back(docs[i].root()["buildPresets"]);
    }
    for (int i = 0; i < 2; ++i) {
        if (!read[i]) continue;
        const json::Value root = docs[i].root();
        auto shown = [&](const char* key, auto fn) {
            for (json::Value p : root[key])
                if (!p["hidden"].boolean() && !p["name"].str().empty()) fn(std::string(p["name"].str()));
        };
        shown("configurePresets", [&](const std::string& n) {
            presets = true;
            c.add("Configure (" + n + ")", "cmake --preset " + sh_quote(n));
            if (std::string dir = preset_dir(configures, n); !dir.empty()) preset_dirs.push_back(dir);
        });
        shown("buildPresets", [&](const std::string& n) {
            std::string cmd = "cmake --build --preset " + sh_quote(n);
            // Its folder not configured yet (a fresh clone): configured
            // first. (Not when where it is cannot be known here: a
            // $env{} one, say, would be configured again at every build.)
            if (const std::string conf = build_configure(builds, n); !conf.empty()) {
                const std::string dir = preset_dir(configures, conf);
                if (!dir.empty() && !configured(dir)) cmd = "cmake --preset " + sh_quote(conf) + " && " + cmd;
            }
            c.add("Build (" + n + ")", cmd, !built);
            built = true;
        });
        shown("testPresets", [&](const std::string& n) { c.add("Test (" + n + ")", "ctest --preset " + sh_quote(n)); });
    }
    // Folders configured already: build*, cmake-build-*, out, and those one level in (build/debug).
    std::vector<std::string> dirs;
    for (const FsEntry& e : c.files) {
        if (!e.dir || !(e.name.starts_with("build") || e.name.starts_with("cmake-build") || e.name == "out")) continue;
        if (configured(e.name)) {
            dirs.push_back(e.name);
            continue;
        }
        for (const FsEntry& in : fs_list(c.root + "/" + e.name))
            if (in.dir && configured(e.name + "/" + in.name)) dirs.push_back(e.name + "/" + in.name);
    }
    // A preset's folder is built with its preset when it has a build preset.
    for (const std::string& dir : dirs) {
        if (built && std::find(preset_dirs.begin(), preset_dirs.end(), dir) != preset_dirs.end()) continue;
        const std::string tag = dir == "build" ? "" : " (" + dir + ")", q = sh_quote(dir);
        c.add("Build" + tag, "cmake --build " + q + " --parallel", !built);
        built = true;
        c.add("Test" + tag, "ctest --test-dir " + q + " --output-on-failure");
        c.add("Clean" + tag, "cmake --build " + q + " --target clean");
        c.add("Configure" + tag, "cmake -S . -B " + q);
    }
    if (!presets && dirs.empty()) {  // never configured
        c.add("Build", "cmake -S . -B build && cmake --build build --parallel", true);
        c.add("Configure", "cmake -S . -B build");
        c.add("Test", "ctest --test-dir build --output-on-failure");
    }
}

// ---------------------------------------------------------------- make and friends

void make(Look& c) {
    const char* file = c.file("GNUmakefile") ? "GNUmakefile" : c.file("Makefile") ? "Makefile" : c.file("makefile") ? "makefile" : nullptr;
    if (!file) return;
    c.add("make", "make", true);
    std::vector<std::string> targets;
    each_line(c.read(file), [&](std::string_view line) {
        if (line.empty() || line[0] == '\t' || line[0] == ' ' || line[0] == '#' || line[0] == '.') return;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return;
        if (colon + 1 < line.size() && line[colon + 1] == '=') return;  // X := y
        const std::string_view head = line.substr(0, colon);
        if (head.find_first_of("=$%(") != std::string_view::npos) return;
        size_t at = 0;
        while (at < head.size()) {
            while (at < head.size() && head[at] == ' ') ++at;
            size_t end = at;
            while (end < head.size() && head[end] != ' ' && head[end] != '\t') ++end;
            const std::string_view w = head.substr(at, end - at);
            bool word = !w.empty();
            for (const char ch : w) word &= (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            if (word && std::find(targets.begin(), targets.end(), w) == targets.end() && targets.size() < 40) targets.emplace_back(w);
            at = end;
        }
    });
    for (const std::string& t : targets) c.add("make " + t, "make " + t);
}

void autotools(Look& c) {
    if (c.file("Makefile") || c.file("makefile") || c.file("GNUmakefile")) return;
    if (c.file("configure")) {
        c.add("Configure", "./configure", true);
        c.add("Configure and make", "./configure && make");
    } else if (c.file("configure.ac")) {
        c.add("Configure", "autoreconf -fi && ./configure", true);
        c.add("Generate configure", "autoreconf -fi");
    }
}

void ninja(Look& c) {
    if (!c.file("build.ninja") || c.file("CMakeLists.txt") || c.file("meson.build")) return;
    c.add("Build", "ninja", true);
    c.add("Clean", "ninja -t clean");
}

void meson(Look& c) {
    if (!c.file("meson.build")) return;
    std::string dir;
    if (c.dir("builddir")) dir = "builddir";
    else if (c.dir("build") && fs_kind(c.root + "/build/meson-private") == 2) dir = "build";
    if (dir.empty()) {
        c.add("Build", "meson setup build && meson compile -C build", true);
        c.add("Set up", "meson setup build");
        return;
    }
    c.add("Build", "meson compile -C " + dir, true);
    c.add("Test", "meson test -C " + dir);
    c.add("Reconfigure", "meson setup --reconfigure " + dir);
    c.add("Clean", "meson compile -C " + dir + " --clean");
}



// ---------------------------------------------------------------- Python








void python(Look& c) {
    const bool project = c.file("pyproject.toml") || c.file("setup.py") || c.file("setup.cfg");
    if (!project && !c.file("requirements.txt") && !c.file("main.py")) return;
    const std::string pyproject = c.file("pyproject.toml") ? c.read("pyproject.toml") : std::string();
    // The package, for python -m: [project]'s name.
    std::string package;
    bool in_project = false;
    each_line(pyproject, [&](std::string_view line) {
        line = trim(line);
        if (line.starts_with('[')) in_project = line == "[project]";
        else if (in_project && package.empty() && line.starts_with("name")) {
            const size_t q = line.find_first_of("\"'");
            const size_t e = q == std::string_view::npos ? q : line.find(line[q], q + 1);
            if (e != std::string_view::npos) package = std::string(line.substr(q + 1, e - q - 1));
        }
    });
    std::replace(package.begin(), package.end(), '-', '_');
    // (A name that is not a plain module path is not put in a command.)
    for (const char ch : package)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '.') package.clear();
    std::string runs, env;
    if (c.file("main.py")) runs = "main.py";
    else if (!package.empty() && fs_kind(c.root + "/src/" + package + "/__main__.py") == 1) {
        runs = "-m " + sh_quote(package);
        // Under src/ with no build system to install it: found by the path.
        if (pyproject.find("[build-system]") == std::string::npos && !c.file("setup.py") && !c.file("setup.cfg")) env = "PYTHONPATH=src ";
    } else if (!package.empty() && fs_kind(c.root + "/" + package + "/__main__.py") == 1)
        runs = "-m " + sh_quote(package);
    const bool tests = c.dir("tests") || c.dir("test") || c.file("conftest.py") || c.file("pytest.ini") || pyproject.find("pytest") != std::string::npos;
    if (c.file("uv.lock") || pyproject.find("uv_build") != std::string::npos || pyproject.find("[tool.uv") != std::string::npos) {
        if (!runs.empty()) c.add("Run", env + "uv run python " + runs, true);
        if (tests) c.add("Test", "uv run pytest", runs.empty());
        c.add("Sync", "uv sync", runs.empty() && !tests);
        return;
    }
    const std::string py = c.dir(".venv") ? ".venv/bin/python" : c.dir("venv") ? "venv/bin/python" : "python3";
    if (!runs.empty()) c.add("Run", env + py + " " + runs, true);
    if (tests) c.add("Test", py + " -m pytest", runs.empty());
    if (project) c.add("Install (editable)", py + " -m pip install -e .", runs.empty() && !tests);
    if (c.file("requirements.txt")) c.add("Install requirements", py + " -m pip install -r requirements.txt", runs.empty() && !tests && !project);
}


struct System {
    const char* name;
    void (*find)(Look&);
};
// (C, C++ and HIP's, and Python's: others are init.lua's to add, with blit.build.add.)
const System kSystems[] = {{"CMake", cmake}, {"Meson", meson}, {"Make", make}, {"Autotools", autotools}, {"Ninja", ninja}, {"Python", python}};

}  // namespace

const std::vector<std::string>& build_systems() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const System& s : kSystems) v.emplace_back(s.name);
        return v;
    }();
    return names;
}

std::string sh_word(std::string_view word) { return sh_quote(word); }

std::vector<BuildTask> read_tasks(const std::string& root, std::string* problem) {
    std::vector<BuildTask> out;
    std::string text;
    if (!fs_read(root + "/" + kTasks, &text)) return out;
    json::Doc doc;
    if (!doc.parse(text)) {
        *problem = std::string(kTasks) + " is not JSON";
        return out;
    }
    const json::Value list = doc.root().is_array() ? doc.root() : doc.root()["tasks"];
    if (!list.is_array()) {
        *problem = std::string(kTasks) + " has no \"tasks\" list";
        return out;
    }
    for (const json::Value j : list) {
        BuildTask t;
        t.system = "Tasks";
        t.name = std::string(j["label"].str());
        const std::string command(j["command"].str());
        if (t.name.empty() || command.empty()) {
            *problem = std::string(kTasks) + ": a task needs a \"label\" and a \"command\"";
            continue;
        }
        const std::string_view cwd = j["cwd"].str();
        t.cwd = cwd == "." ? std::string() : std::string(cwd);
        for (const json::Value e : j["env"]) t.env.push_back(std::string(e.key()) + "=" + std::string(e.str()));
        if (j["problems"].is_string()) t.problems.emplace_back(j["problems"].str());
        for (const json::Value m : j["problems"]) t.problems.emplace_back(m.str());
        t.lasting = j["long"].boolean();
        t.kind = std::string(j["kind"].str());
        if (!t.kind.empty() && std::ranges::find_if(kTaskKinds, [&](const char* k) { return t.kind == k; }) == std::end(kTaskKinds)) {
            std::string kinds;
            for (const char* k : kTaskKinds) kinds += (kinds.empty() ? "\"" : ", \"") + std::string(k) + "\"";
            *problem = std::string(kTasks) + ": " + t.name + "'s kind is one of " + kinds;
            t.kind.clear();
        }
        if (t.kind.empty() && !sim_tool(command).empty()) t.kind = "simulation";  // (it runs Verilator, Icarus or SymbiYosys: simrun.hpp)
        // What it runs: the command as written (a line for sh), then its
        // arguments, each one word whatever is in it.
        std::string cmd;
        if (!t.env.empty()) {
            cmd = "env";
            for (const std::string& e : t.env) cmd += " " + sh_quote(e);
            cmd += " ";
        }
        cmd += command;
        if (j["args"].is_string()) cmd += " " + std::string(j["args"].str());
        for (const json::Value a : j["args"]) cmd += " " + sh_quote(a.str());
        t.cmd = std::move(cmd);
        out.push_back(std::move(t));
    }
    return out;
}

std::string with_vars(std::string_view cmd, const TaskVars& vars) {
    if (cmd.find("${") == std::string_view::npos) return std::string(cmd);
    std::string out;
    out.reserve(cmd.size());
    char quote = 0;  // (the quotes it is in: ' or ", or none)
    for (size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];
        if (c == '\\' && quote != '\'' && i + 1 < cmd.size()) {  // (what follows is as it is)
            out += cmd.substr(i, 2);
            ++i;
            continue;
        }
        if (c == '\'' && quote != '"') quote = quote ? 0 : '\'';
        else if (c == '"' && quote != '\'') quote = quote ? 0 : '"';
        const size_t end = c == '$' && i + 1 < cmd.size() && cmd[i + 1] == '{' ? cmd.find('}', i + 2) : std::string_view::npos;
        if (end == std::string_view::npos) {
            out += c;
            continue;
        }
        const std::string_view name = cmd.substr(i + 2, end - i - 2), file = vars.file;
        const size_t slash = file.rfind('/');
        const std::string_view base = slash == std::string_view::npos ? file : file.substr(slash + 1);
        const size_t dot = base.rfind('.');
        std::string value;
        if (name == "file") value = vars.file;
        else if (name == "line") value = vars.line;
        else if (name == "word") value = vars.word;
        else if (name == "selection") value = vars.selection;
        else if (name == "workdir") value = vars.workdir;
        else if (name == "fileDirname") value = slash == std::string_view::npos ? std::string() : std::string(file.substr(0, std::max<size_t>(slash, 1)));
        else if (name == "fileBasename") value = std::string(base);
        else if (name == "fileBasenameNoExtension") value = std::string(dot == std::string_view::npos || dot == 0 ? base : base.substr(0, dot));
        else {  // (sh's own, as ${HOME})
            out += c;
            continue;
        }
        if (quote == '\'') {  // (inside '...': a quote ends them, is said, and starts them again)
            for (const char v : value) out += v == '\'' ? std::string("'\\''") : std::string(1, v);
        } else if (quote == '"') {  // (inside "...": what sh would take there, escaped)
            for (const char v : value) {
                if (v == '\\' || v == '"' || v == '$' || v == '`') out += '\\';
                out += v;
            }
        } else {
            out += sh_quote(value);
        }
        i = end;
    }
    return out;
}

std::vector<BuildTask> read_run_configs(const std::string& root, std::string* problem, RunEnvironments* environments) {
    std::vector<BuildTask> out;
    std::string text;
    if (!fs_read(root + "/" + kRunConfigs, &text)) return out;
    json::Doc doc;
    if (!doc.parse(text)) {
        *problem = std::string(kRunConfigs) + " is not JSON";
        return out;
    }
    const json::Value list = doc.root()["configurations"];
    if (!list.is_array()) {
        *problem = std::string(kRunConfigs) + " has no \"configurations\" list";
        return out;
    }
    // (In a project on another machine the command runs there, where its
    // paths have no ssh://host; Debug and Profile take them whole.)
    std::string host, there;
    const bool remote = split_remote(root, &host, &there);
    const std::string prefix = remote ? remote_prefix(host) : std::string();
    for (const json::Value c : list) {
        BuildTask t;
        t.system = "Run";
        t.config = true;
        t.name = std::string(c["name"].str());
        std::string program(c["program"].str());
        if (t.name.empty() || program.empty()) {
            *problem = std::string(kRunConfigs) + ": a configuration needs a \"name\" and a \"program\"";
            continue;
        }
        t.program = program.starts_with("ssh://") || program.starts_with("docker://") ? program
                    : program[0] != '/'                                                ? root + "/" + program
                    : remote                                                           ? prefix + program  // (a whole path in a project there is a path there)
                                                                                       : program;
        if (c["args"].is_array()) {
            for (const json::Value a : c["args"]) t.args += (t.args.empty() ? "" : " ") + sh_quote(a.str());
        } else {
            t.args = std::string(c["args"].str());
        }
        for (const json::Value e : c["env"]) t.env.push_back(std::string(e.key()) + "=" + std::string(e.str()));
        const std::string_view cwd = c["cwd"].str();
        t.cwd = cwd == "." ? std::string() : std::string(cwd);
        t.before = std::string(c["build"].str());
        if (c["problems"].is_string()) t.problems.emplace_back(c["problems"].str());
        for (const json::Value m : c["problems"]) t.problems.emplace_back(m.str());
        if (const json::Value d = c["debug"]; d.is_object()) {
            if (d["adapter"].is_array()) {  // (a command's words)
                for (const json::Value w : d["adapter"]) t.debug.adapter += (t.debug.adapter.empty() ? "" : " ") + sh_quote(w.str());
            } else {
                t.debug.adapter = std::string(d["adapter"].str());
            }
            t.debug.request = std::string(d["request"].str());
            if (d["arguments"].is_object()) json::dump(t.debug.arguments, d["arguments"]);
            if (d["after"].is_string()) t.debug.after.emplace_back(d["after"].str());
            for (const json::Value a : d["after"]) t.debug.after.emplace_back(a.str());
            t.debug.console = std::string(d["console"].str());
            if (!t.debug.request.empty() && t.debug.request != "launch" && t.debug.request != "attach")
                *problem = std::string(kRunConfigs) + ": " + t.name + "'s debug.request is \"launch\" or \"attach\"";
            if (!t.debug.console.empty() && t.debug.console != "terminal" && t.debug.console != "debug")
                *problem = std::string(kRunConfigs) + ": " + t.name + "'s debug.console is \"terminal\" or \"debug\"";
        }
        board_read(c["board"], t.name, &t.board, problem);
        t.own_env = t.env;
        run_in_environment(t, nullptr, root);
        out.push_back(std::move(t));
    }
    if (!out.empty()) out.front().def = true;
    if (environments) {
        environments->list.clear();
        const json::Value envs = doc.root()["environments"];
        if (envs.exists() && !envs.is_object()) *problem = std::string(kRunConfigs) + ": \"environments\" is an object, each a name and its variables";
        for (const json::Value e : envs) {
            RunEnvironment r;
            r.name = std::string(e.key());
            if (!e.is_object()) {
                *problem = std::string(kRunConfigs) + ": the environment " + r.name + " is an object of variables";
                continue;
            }
            for (const json::Value v : e) {
                if (v.key() == "capture") r.capture = std::string(v.str());
                else r.env.push_back(std::string(v.key()) + "=" + (v.is_string() ? std::string(v.str()) : v.exists() ? std::to_string(v.integer()) : std::string()));
            }
            if (!r.capture.empty() && r.capture != "metal" && r.capture != "renderdoc")
                *problem = std::string(kRunConfigs) + ": the environment " + r.name + "'s capture is \"metal\" or \"renderdoc\"";
            if (r.capture == "metal") r.env.emplace_back("MTL_CAPTURE_ENABLED=1");  // (Metal lets gpucapture take a frame of a program run with it)
            environments->list.push_back(std::move(r));
        }
    }
    return out;
}

void run_in_environment(BuildTask& t, const RunEnvironment* e, const std::string& root) {
    t.env = t.own_env;
    if (e) t.env.insert(t.env.end(), e->env.begin(), e->env.end());
    // What the Run button runs: the program, in its folder, with what it is
    // given; under RenderDoc, run by renderdoccmd (v1.47
    // renderdoccmd/renderdoccmd.cpp lines 1565, 1621-1625: capture, -c the
    // captures' name, frame numbers added; -w waits for the program), its
    // captures in the folder's .blit/captures, named for the configuration.
    std::string cmd;
    if (!t.env.empty()) {
        cmd = "env";
        for (const std::string& v : t.env) cmd += " " + sh_quote(v);
        cmd += " ";
    }
    std::string h, program_there, root_there;
    const std::string program = sh_quote(split_remote(t.program, &h, &program_there) ? program_there : t.program);
    if (e && e->capture == "renderdoc") {
        const std::string captures = (split_remote(root, &h, &root_there) ? root_there : root) + "/" + kCaptures;
        std::string name;
        for (const char c : t.name) name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '-';
        cmd = "mkdir -p " + sh_quote(captures) + " && " + cmd + "renderdoccmd capture -w -c " + sh_quote(captures + "/" + name) + " ";
    }
    t.cmd = cmd + program + (t.args.empty() ? "" : " " + t.args);
}

std::vector<BuildTask> detect_builds(const std::string& root, const std::vector<FsEntry>& files, const std::vector<std::string>& hidden) {
    std::vector<BuildTask> out;
    for (const System& s : kSystems) {
        if (std::find(hidden.begin(), hidden.end(), s.name) != hidden.end()) continue;
        Look c{root, files, out, s.name};
        s.find(c);
    }
    return out;
}

}  // namespace blit
