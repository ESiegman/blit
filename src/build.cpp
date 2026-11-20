#include "build.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string_view>

#include "json.hpp"
#include "remote.hpp"

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

// A configure preset's binaryDir, through what it inherits, with the macros
// that can be known here filled in (${presetName} is the one asked about,
// not the one it inherits the binaryDir from).
std::string preset_dir(const json::Value& presets, std::string_view name, std::string_view asked = {}, int depth = 0) {
    if (depth > 8) return {};
    if (asked.empty()) asked = name;
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
            return dir.find("${") == std::string::npos ? dir : std::string();
        }
        const json::Value up = p["inherits"];
        if (up.is_string()) return preset_dir(presets, up.str(), asked, depth + 1);
        for (json::Value u : up)
            if (std::string d = preset_dir(presets, u.str(), asked, depth + 1); !d.empty()) return d;
        return {};
    }
    return {};
}

void cmake(Look& c) {
    if (!c.file("CMakeLists.txt")) return;
    bool presets = false, built = false;
    std::vector<std::string> preset_dirs;
    for (const char* name : {"CMakePresets.json", "CMakeUserPresets.json"}) {
        if (!c.file(name)) continue;
        json::Doc d;
        if (!d.parse(c.read(name))) continue;
        const json::Value root = d.root();
        auto shown = [&](const char* key, auto fn) {
            for (json::Value p : root[key])
                if (!p["hidden"].boolean() && !p["name"].str().empty()) fn(std::string(p["name"].str()));
        };
        shown("configurePresets", [&](const std::string& n) {
            presets = true;
            c.add("Configure (" + n + ")", "cmake --preset " + sh_quote(n));
            if (std::string dir = preset_dir(root["configurePresets"], n); !dir.empty()) preset_dirs.push_back(dir);
        });
        shown("buildPresets", [&](const std::string& n) {
            c.add("Build (" + n + ")", "cmake --build --preset " + sh_quote(n), !built);
            built = true;
        });
        shown("testPresets", [&](const std::string& n) { c.add("Test (" + n + ")", "ctest --preset " + sh_quote(n)); });
    }
    // Folders configured already: build*, cmake-build-*, out, and those one level in (build/debug).
    std::vector<std::string> dirs;
    auto configured = [&](const std::string& dir) { return fs_kind(c.root + "/" + dir + "/CMakeCache.txt") == 1; };
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

std::vector<BuildTask> read_run_configs(const std::string& root, std::string* problem) {
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
        // What the Run button runs: the program, in its folder, with what it is given.
        std::string cmd;
        if (!t.env.empty()) {
            cmd = "env";
            for (const std::string& e : t.env) cmd += " " + sh_quote(e);
            cmd += " ";
        }
        std::string h, program_there;
        t.cmd = cmd + sh_quote(split_remote(t.program, &h, &program_there) ? program_there : t.program) + (t.args.empty() ? "" : " " + t.args);
        out.push_back(std::move(t));
    }
    if (!out.empty()) out.front().def = true;
    return out;
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
