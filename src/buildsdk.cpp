// Firmware's build systems, found natively (answer 27): ESP-IDF and
// PlatformIO. Each finder reads the project's own files (an ESP-IDF
// project's CMakeLists.txt, sdkconfig and build/project_description.json;
// PlatformIO's platformio.ini and the board manifests of its installed
// platforms) and looks for its SDK on the folder's machine: ESP-IDF where
// install.sh or Espressif's installation manager put it, its Python
// environment and so its tools put on the PATH for each task by
// idf_tools.py (as export.sh does, without the user sourcing it);
// PlatformIO's pio on the PATH or in its own ~/.platformio/penv. What is
// found is kept per machine (one not found is looked for again a few
// seconds on). Flashing and the serial monitor are blit's own (appflash.cpp,
// appfirmware.cpp): their tasks carry the firmware's board.
#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

#include "build.hpp"
#include "common.hpp"
#include "fsio.hpp"
#include "json.hpp"
#include "missing.hpp"
#include "remote.hpp"
#include "engine.hpp"

namespace blit {

namespace {

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

// The folder's machine: its host ("" here), what its paths start with
// ("ssh://host"), and its home folder there.
struct Machine {
    std::string host, at, home;
};

Machine machine_of(const std::string& root) {
    Machine m;
    std::string there;
    if (split_remote(root, &m.host, &there)) {
        m.at = remote_prefix(m.host);
        const std::shared_ptr<Remote> c = Remote::get(m.host);
        if (c && c->wait_ready(kRemoteWaitMs)) m.home = c->home();
    } else if (const char* h = std::getenv("HOME")) {
        m.home = h;
    }
    return m;
}

// A path of the machine's as the user writes it: ~ for its home.
std::string tilde(const Machine& m, const std::string& path) {
    if (!m.home.empty() && path.starts_with(m.home + "/")) return "~" + path.substr(m.home.size());
    return path;
}

bool is_file(const Machine& m, const std::string& path) { return fs_kind(m.at + path) == 1; }

// What is found on each machine (by host), and when: kept, though one not
// found is looked for again after kAgainNs (installed since).
constexpr u64 kAgainNs = 5'000'000'000ull;
template <class T>
struct Kept {
    std::mutex mu;
    std::map<std::string, std::pair<T, u64>> by_host;
    template <class Find>
    T get(const std::string& host, Find find) {
        {
            std::lock_guard lock(mu);
            const auto it = by_host.find(host);
            if (it != by_host.end() && (it->second.first.missing.empty() || now_ns() - it->second.second < kAgainNs)) return it->second.first;
        }
        T found = find();
        std::lock_guard lock(mu);
        by_host[host] = {found, now_ns()};
        return found;
    }
};

// A quoted list's words, as Python writes one: ['esp32', 'esp32s2'] (after `name =`).
std::vector<std::string> python_list(std::string_view text, std::string_view name) {
    std::vector<std::string> out;
    size_t at = text.find(name);
    while (at != std::string_view::npos && at > 0 && text[at - 1] != '\n') at = text.find(name, at + 1);
    if (at == std::string_view::npos) return out;
    const size_t open = text.find('[', at), close = open == std::string_view::npos ? open : text.find(']', open);
    if (close == std::string_view::npos) return out;
    std::string_view list = text.substr(open + 1, close - open - 1);
    while (true) {
        const size_t q = list.find_first_of("'\"");
        if (q == std::string_view::npos) break;
        const size_t e = list.find(list[q], q + 1);
        if (e == std::string_view::npos) break;
        out.emplace_back(list.substr(q + 1, e - q - 1));
        list.remove_prefix(e + 1);
    }
    return out;
}

// "3.14" and "3.9" as numbers: which is the newer.
bool newer_version(std::string_view a, std::string_view b) {
    while (!a.empty() || !b.empty()) {
        const long x = std::strtol(std::string(a).c_str(), nullptr, 10), y = std::strtol(std::string(b).c_str(), nullptr, 10);
        if (x != y) return x > y;
        const size_t da = a.find('.'), db = b.find('.');
        a = da == std::string_view::npos ? std::string_view{} : a.substr(da + 1);
        b = db == std::string_view::npos ? std::string_view{} : b.substr(db + 1);
    }
    return false;
}

// ---------------------------------------------------------------- ESP-IDF

// Where ESP-IDF is on a machine, its Python environment (whose idf_tools.py
// puts its tools on the PATH), its version, and the targets idf.py
// set-target takes (its own list; the preview ones with --preview).
struct IdfPlace {
    std::string path, python, tools, version;
    std::vector<std::pair<std::string, std::string>> targets;  // (the name shown, what idf.py is given)
    std::string missing;
};

IdfPlace idf_look(const Machine& m, const std::string& root) {
    IdfPlace p;
    std::vector<std::string> places;
    std::map<std::string, std::string> pythons, tools_of;  // (by place: what Espressif's installation manager says)
    // IDF_PATH: blit's own environment here; there, the login shell's.
    if (m.host.empty()) {
        if (const char* e = std::getenv("IDF_PATH"); e && *e) places.emplace_back(e);
    } else {
        std::string out;
        if (fs_run(root, "printf '%s' \"$IDF_PATH\"", &out, 10'000) && !trim(out).empty()) places.emplace_back(trim(out));
    }
    std::string tools = m.home + "/.espressif";
    if (const char* e = std::getenv("IDF_TOOLS_PATH"); m.host.empty() && e && *e) tools = e;
    // The installation manager's (EIM, ESP-IDF 5.4 on): eim_idf.json, the
    // one chosen first.
    if (std::string text; fs_read(m.at + tools + "/eim_idf.json", &text)) {
        json::Doc doc;
        if (doc.parse(text)) {
            const std::string chosen(doc.root()["idfSelectedId"].str());
            for (int pass = 0; pass < 2; ++pass)
                for (const json::Value v : doc.root()["idfInstalled"]) {
                    if ((std::string(v["id"].str()) == chosen) != (pass == 0)) continue;
                    const std::string path(v["path"].str());
                    if (path.empty()) continue;
                    places.push_back(path);
                    if (!v["python"].str().empty()) pythons[path] = std::string(v["python"].str());
                    if (!v["idfToolsPath"].str().empty()) tools_of[path] = std::string(v["idfToolsPath"].str());
                }
        }
    }
    // install.sh's: ~/esp/esp-idf, then ~/esp/esp-idf-v6.1 and ~/esp/v6.1/esp-idf, newest first.
    places.push_back(m.home + "/esp/esp-idf");
    std::vector<std::string> more;
    for (const FsEntry& e : fs_list(m.at + m.home + "/esp"))
        if (e.dir && e.name != "esp-idf" && (e.name.starts_with("esp-idf") || e.name.starts_with("v")))
            more.push_back(e.name.starts_with("v") ? e.name + "/esp-idf" : e.name);
    auto number = [](const std::string& name) { return std::string_view(name).substr(std::min(name.find_first_of("0123456789"), name.size())); };
    std::ranges::sort(more, [&](const std::string& a, const std::string& b) { return newer_version(number(a), number(b)); });
    for (const std::string& d : more) places.push_back(m.home + "/esp/" + d);
    for (const std::string& place : places)
        if (is_file(m, place + "/tools/idf.py")) {
            p.path = place;
            break;
        }
    if (p.path.empty()) {
        p.missing = "ESP-IDF is not installed on " + machine_name(m.host) + " (not in IDF_PATH, ~/esp or " + tilde(m, tools) + ")";
        return p;
    }
    if (const auto it = tools_of.find(p.path); it != tools_of.end()) tools = it->second;
    p.tools = tools;
    // Its version (tools/cmake/version.cmake: set(IDF_VERSION_MAJOR 6) ...).
    std::string major, minor;
    if (std::string text; fs_read(m.at + p.path + "/tools/cmake/version.cmake", &text, 1u << 16))
        each_line(text, [&](std::string_view line) {
            line = trim(line);
            for (auto [key, into] : {std::pair<std::string_view, std::string*>{"set(IDF_VERSION_MAJOR ", &major}, {"set(IDF_VERSION_MINOR ", &minor}})
                if (line.starts_with(key)) *into = std::string(trim(line.substr(key.size(), line.find(')') - key.size())));
        });
    p.version = major.empty() ? std::string() : major + "." + minor;
    // Its Python environment: the manager's, else install.sh's
    // (TOOLS/python_env/idf6.1_py3.14_env), the newest Python's.
    if (const auto it = pythons.find(p.path); it != pythons.end() && is_file(m, it->second)) {
        p.python = it->second;
    } else if (!p.version.empty()) {
        const std::string prefix = "idf" + p.version + "_py";
        std::string best;
        for (const FsEntry& e : fs_list(m.at + tools + "/python_env"))
            if (e.dir && e.name.starts_with(prefix) && e.name.ends_with("_env") && is_file(m, tools + "/python_env/" + e.name + "/bin/python")) {
                const std::string py = e.name.substr(prefix.size(), e.name.size() - prefix.size() - 4);
                if (best.empty() || newer_version(py, best.substr(prefix.size(), best.size() - prefix.size() - 4))) best = e.name;
            }
        if (!best.empty()) p.python = tools + "/python_env/" + best + "/bin/python";
    }
    if (p.python.empty()) {
        p.missing = "ESP-IDF's Python environment is not installed on " + machine_name(m.host) + ": " + tilde(m, p.path) + "/install.sh sets it up";
        return p;
    }
    // The targets, as idf.py lists them (tools/idf_py_actions/constants.py).
    if (std::string text; fs_read(m.at + p.path + "/tools/idf_py_actions/constants.py", &text, 1u << 18)) {
        for (const std::string& t : python_list(text, "SUPPORTED_TARGETS")) p.targets.emplace_back(t, "idf.py set-target " + t);
        for (const std::string& t : python_list(text, "PREVIEW_TARGETS")) p.targets.emplace_back(t + " (preview)", "idf.py --preview set-target " + t);
    }
    return p;
}

Kept<IdfPlace>& idf_kept() {
    static Kept<IdfPlace> k;
    return k;
}

// A line of the project's sdkconfig: CONFIG_IDF_TARGET="esp32s3".
std::string sdk_value(std::string_view text, std::string_view key) {
    std::string out;
    each_line(text, [&](std::string_view line) {
        if (!out.empty() || !line.starts_with(key) || line.size() <= key.size() || line[key.size()] != '=') return;
        std::string_view v = trim(line.substr(key.size() + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        out = std::string(v);
    });
    return out;
}

// What a Monitor's row says it opens: the port, where it is known, at its speed.
std::string serial_words(const Board& b) { return (b.port.empty() ? std::string("serial port") : b.port) + " at " + std::to_string(b.baud) + " baud"; }

// A task of a system's (its setup and what it needs, from where its SDK was found).
struct Adder {
    std::vector<BuildTask>& out;
    std::string system, setup, missing;
    BuildTask& add(std::string name, std::string cmd, bool def = false) {
        BuildTask t;
        t.system = system;
        t.name = std::move(name);
        t.cmd = std::move(cmd);
        t.def = def;
        t.setup = setup;
        t.missing = missing;
        out.push_back(std::move(t));
        return out.back();
    }
    // One of blit's own (flash, monitor): it runs no command of the SDK's.
    BuildTask& own(std::string name, std::string what, std::string action, const Board& board, const std::string& program, std::string firmware, std::string before) {
        BuildTask& t = add(std::move(name), std::move(what));
        t.setup.clear();
        t.missing.clear();
        t.action = std::move(action);
        t.board = board;
        t.program = program;
        t.firmware = std::move(firmware);
        t.before = std::move(before);
        if (t.action == "flash") t.kind = "flash";
        return t;
    }
};

}  // namespace

bool find_esp_idf(const std::string& root, const std::vector<FsEntry>& files, std::vector<BuildTask>& out) {
    auto file = [&](std::string_view name) { return std::ranges::any_of(files, [&](const FsEntry& e) { return !e.dir && e.name == name; }); };
    auto dir = [&](std::string_view name) { return std::ranges::any_of(files, [&](const FsEntry& e) { return e.dir && e.name == name; }); };
    if (!file("CMakeLists.txt")) return false;
    std::string cml;
    if (!fs_read(root + "/CMakeLists.txt", &cml, 1u << 20)) return false;
    // A project is ESP-IDF's when a line of its CMakeLists.txt, spaces left
    // out, begins with the include of its project.cmake (or of v2's
    // idf.cmake): idf.py v6.1.1's own test (idf_py_actions/mcp_ext.py,
    // _is_valid_project_dir, with idf_component_tools' CMAKE_PROJECT_LINE).
    bool idf = false;
    std::string project;
    each_line(cml, [&](std::string_view line) {
        std::string packed;
        for (const char c : line)
            if (!std::isspace(static_cast<unsigned char>(c))) packed += c;
        if (packed.starts_with("include($ENV{IDF_PATH}/tools/cmake/project.cmake)") || packed.starts_with("include($ENV{IDF_PATH}/tools/cmakev2/idf.cmake)")) idf = true;
        // (project(hello_world): its name, which its ELF is named for.)
        line = trim(line);
        if (project.empty() && line.starts_with("project")) {
            std::string_view rest = trim(line.substr(7));
            if (rest.starts_with('(')) {
                rest = trim(rest.substr(1));
                const size_t end = rest.find_first_of(" \t)");
                project = std::string(rest.substr(0, end));
            }
        }
    });
    if (!idf) return false;
    // What the last build says of itself (build/project_description.json),
    // else the project's sdkconfig, else ESP-IDF's defaults.
    std::string elf = project.empty() ? std::string() : project + ".elf", target, baud;
    if (dir("build"))
        if (std::string text; fs_read(root + "/build/project_description.json", &text, 1u << 20)) {
            json::Doc doc;
            if (doc.parse(text)) {
                if (!doc.root()["app_elf"].str().empty()) elf = std::string(doc.root()["app_elf"].str());
                target = std::string(doc.root()["target"].str());
                if (doc.root()["monitor_baud"].exists()) baud = doc.root()["monitor_baud"].is_string() ? std::string(doc.root()["monitor_baud"].str()) : std::to_string(doc.root()["monitor_baud"].integer());
            }
        }
    std::string sdkconfig;
    if (file("sdkconfig")) fs_read(root + "/sdkconfig", &sdkconfig, 1u << 20);
    else if (file("sdkconfig.defaults")) fs_read(root + "/sdkconfig.defaults", &sdkconfig, 1u << 20);
    if (target.empty()) target = sdk_value(sdkconfig, "CONFIG_IDF_TARGET");
    if (target.empty()) target = "esp32";
    if (baud.empty() || baud == "0") baud = sdk_value(sdkconfig, "CONFIG_ESPTOOLPY_MONITOR_BAUD");
    if (baud.empty()) baud = sdk_value(sdkconfig, "CONFIG_ESP_CONSOLE_UART_BAUDRATE");

    const Machine m = machine_of(root);
    const IdfPlace place = idf_kept().get(m.host, [&] { return idf_look(m, root); });
    Adder a{out, "ESP-IDF", {}, place.missing};
    // Each command runs with ESP-IDF's environment, as export.sh gives it:
    // idf_tools.py's export (its tools and Python environment on the PATH,
    // its variables), with IDF_PATH (and a tools folder not the usual one).
    if (place.missing.empty()) {
        a.setup = "export IDF_PATH=" + sh_word(place.path);
        if (place.tools != m.home + "/.espressif") a.setup += " IDF_TOOLS_PATH=" + sh_word(place.tools);
        a.setup += " && idf_env=$(" + sh_word(place.python) + " \"$IDF_PATH/tools/idf_tools.py\" export) && eval \"$idf_env\"";
    }
    a.add("Build", "idf.py build", true);
    // The board: no probe, its chip the target, so esptool, with the files
    // and addresses the build wrote in build/flasher_args.json; its console
    // at the monitor's speed.
    Board board;
    board.set = true;
    board.chip = target;
    board.baud = std::atoi(baud.c_str());
    if (board.baud <= 0) board.baud = 115200;
    if (!elf.empty()) board.elf = "build/" + elf;
    if (!place.python.empty()) board.tool_dirs.push_back(place.python.substr(0, place.python.rfind('/')));  // (its esptool)
    const std::string firmware = !project.empty() ? project : elf.substr(0, elf.rfind('.'));
    a.own("Flash", "esptool, build/flasher_args.json", "flash", board, board.elf.empty() ? std::string() : root + "/" + board.elf, firmware, "ESP-IDF: Build");
    a.own("Monitor", serial_words(board), "monitor", board, board.elf.empty() ? std::string() : root + "/" + board.elf, firmware, {});
    BuildTask& config = a.add("Menuconfig", "idf.py menuconfig");
    config.keys = true;
    config.lasting = true;
    a.add("Size", "idf.py size");
    BuildTask& set = a.add("Set target", "idf.py set-target \xE2\x80\xA6");
    set.choices = place.targets;
    if (set.choices.empty() && set.missing.empty()) set.missing = "ESP-IDF at " + tilde(m, place.path) + " lists no targets";
    a.add("Reconfigure", "idf.py reconfigure");
    a.add("Clean", "idf.py clean");
    a.add("Full clean", "idf.py fullclean");
    return true;
}

// ---------------------------------------------------------------- PlatformIO

namespace {

// pio on a machine: on the PATH (nothing to set up), else in PlatformIO's
// own environment (~/.platformio/penv/bin, as its installer and VS Code's
// extension put it) or ~/.local/bin (uv tool's, pipx's), put on the PATH.
struct PioPlace {
    std::string setup, core, missing;  // (core: PlatformIO's folder, ~/.platformio)
};

PioPlace pio_look(const Machine& m) {
    PioPlace p;
    p.core = m.home + "/.platformio";
    if (const char* e = std::getenv("PLATFORMIO_CORE_DIR"); m.host.empty() && e && *e) p.core = e;
    if (!Engine::which(m.host, "pio").empty() || !Engine::which(m.host, "platformio").empty()) return p;
    for (const std::string& dir : {p.core + "/penv/bin", m.home + "/.local/bin"})
        if (is_file(m, dir + "/pio")) {
            p.setup = "export PATH=" + sh_word(dir) + ":\"$PATH\"";
            return p;
        }
    p.missing = "PlatformIO is not installed on " + machine_name(m.host) + " (no pio on the PATH, nor in " + tilde(m, p.core) + "/penv/bin)";
    return p;
}

Kept<PioPlace>& pio_kept() {
    static Kept<PioPlace> k;
    return k;
}

// platformio.ini, read as PlatformIO reads it (platformio/project/config.py,
// 6.1): Python's configparser (sections, `key = value` or `key: value`,
// values going on over indented lines, `;` and `#` comments, inline ones
// after a space), an env's options through its `extends` and [env], and
// ${section.option}, ${this.option}, ${this.__env__} and ${sysenv.NAME}
// put in.
struct Ini {
    struct Section {
        std::string name;
        std::vector<std::pair<std::string, std::string>> options;  // (names in lower case, as configparser has them)
    };
    std::vector<Section> sections;

    void read(std::string_view text) {
        Section* sec = nullptr;
        std::string* value = nullptr;
        each_line(text, [&](std::string_view line) {
            const std::string_view t = trim(line);
            if (t.empty() || t.front() == ';' || t.front() == '#') return;
            // (An inline comment: ; or # after a space.)
            std::string_view v = t;
            for (size_t i = 1; i < v.size(); ++i)
                if ((v[i] == ';' || v[i] == '#') && (v[i - 1] == ' ' || v[i - 1] == '\t')) {
                    v = trim(v.substr(0, i));
                    break;
                }
            if ((line.front() == ' ' || line.front() == '\t') && value) {  // (the value before, going on)
                *value += (value->empty() ? "" : "\n") + std::string(v);
                return;
            }
            if (t.front() == '[' && t.back() == ']') {
                const std::string name(trim(t.substr(1, t.size() - 2)));
                sec = nullptr;
                for (Section& s : sections)
                    if (s.name == name) sec = &s;
                if (!sec) sec = &sections.emplace_back(Section{name, {}});
                value = nullptr;
                return;
            }
            const size_t eq = v.find_first_of("=:");
            if (!sec || eq == std::string_view::npos || eq == 0) return;
            std::string key(trim(v.substr(0, eq)));
            for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string val(trim(v.substr(eq + 1)));
            auto it = std::ranges::find_if(sec->options, [&](const auto& o) { return o.first == key; });
            if (it == sec->options.end()) {
                sec->options.emplace_back(key, std::move(val));
                value = &sec->options.back().second;
            } else {
                it->second = std::move(val);
                value = &it->second;
            }
        });
    }

    const std::string* raw(std::string_view section, std::string_view key) const {
        for (const Section& s : sections)
            if (s.name == section)
                for (const auto& [k, v] : s.options)
                    if (k == key) return &v;
        return nullptr;
    }

    static std::vector<std::string> list(std::string_view value) {
        std::vector<std::string> out;
        std::string item;
        auto flush = [&] {
            const std::string_view w = trim(item);
            if (!w.empty()) out.emplace_back(w);
            item.clear();
        };
        for (const char c : value) {
            if (c == '\n' || c == ',') flush();
            else item += c;
        }
        flush();
        return out;
    }

    // An option of a section, through what it extends (its own first, then
    // each it extends, the last named first, then [env] for an env), with
    // its variables put in. nullopt: none says it.
    std::optional<std::string> get(const std::string& section, std::string_view key, int depth = 0) const {
        if (depth > 8) return std::nullopt;
        std::vector<std::string> queue = section.starts_with("env:") ? std::vector<std::string>{"env", section} : std::vector<std::string>{section};
        for (int steps = 0; !queue.empty() && steps < 32; ++steps) {
            const std::string s = queue.back();
            queue.pop_back();
            if (const std::string* v = raw(s, key)) return expand(section, *v, depth);
            if (const std::string* up = raw(s, "extends"))
                for (const std::string& e : list(*up)) queue.push_back(e);
        }
        return std::nullopt;
    }

    std::string expand(const std::string& section, const std::string& value, int depth) const {
        if (value.find("${") == std::string::npos) return value;
        std::string out;
        for (size_t i = 0; i < value.size(); ++i) {
            const size_t end = value.compare(i, 2, "${") == 0 ? value.find('}', i + 2) : std::string::npos;
            if (end == std::string::npos) {
                out += value[i];
                continue;
            }
            const std::string inside = value.substr(i + 2, end - i - 2);
            const size_t dot = inside.find('.');
            std::string from = dot == std::string::npos ? std::string() : inside.substr(0, dot), name = dot == std::string::npos ? inside : inside.substr(dot + 1);
            i = end;
            if (from.empty() || from.find_first_of("}()") != std::string::npos) {  // (PlatformIO's own, or SCons's: left as they are)
                out += "${" + inside + "}";
            } else if (from == "sysenv") {
                if (const char* e = std::getenv(name.c_str())) out += e;
            } else if (from == "this" && name == "__env__") {
                out += section.starts_with("env:") ? section.substr(4) : std::string();
            } else {
                if (from == "this") from = section;
                if (const std::optional<std::string> v = get(from, name, depth + 1)) out += *v;
            }
        }
        return out;
    }
};

// A platform's installed folder's name: "raspberrypi" for raspberrypi,
// platformio/raspberrypi@^1.9 and https://github.com/maxgerhardt/platform-raspberrypi.git.
std::string platform_name(std::string p) {
    if (const size_t at = p.find('@'); at != std::string::npos && p.find("://") == std::string::npos) p = p.substr(0, at);
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (const size_t hash = p.find('#'); hash != std::string::npos) p = p.substr(0, hash);
    if (const size_t slash = p.rfind('/'); slash != std::string::npos) p = p.substr(slash + 1);
    if (p.ends_with(".git")) p.resize(p.size() - 4);
    if (p.starts_with("platform-")) p = p.substr(9);
    return p;
}

// A path a port option names, if it is one (PlatformIO takes wildcards,
// which are not).
std::string plain_port(const std::optional<std::string>& v) {
    if (!v || v->empty() || v->find_first_of("*?[") != std::string::npos) return {};
    return *v;
}

}  // namespace

bool find_platformio(const std::string& root, const std::vector<FsEntry>& files, std::vector<BuildTask>& out) {
    if (!std::ranges::any_of(files, [](const FsEntry& e) { return !e.dir && e.name == "platformio.ini"; })) return false;
    std::string text;
    if (!fs_read(root + "/platformio.ini", &text, 1u << 20)) return false;
    const Machine m = machine_of(root);
    Ini ini;
    ini.read(text);
    // [platformio] extra_configs: more files read after it (paths from the
    // folder, ~ for the home folder; a wildcard in the last part).
    if (const std::optional<std::string> extra = ini.get("platformio", "extra_configs"))
        for (std::string pattern : Ini::list(*extra)) {
            if (pattern.starts_with("~/")) pattern = m.home + pattern.substr(1);
            const std::string full = pattern.starts_with("/") ? m.at + pattern : root + "/" + pattern;
            const size_t slash = full.rfind('/');
            const std::string folder = full.substr(0, slash), name = full.substr(slash + 1);
            std::vector<std::string> paths;
            if (name.find_first_of("*?[") == std::string::npos) paths.push_back(full);
            else
                for (const FsEntry& e : fs_list(folder))
                    if (!e.dir && fnmatch(name.c_str(), e.name.c_str(), 0) == 0) paths.push_back(folder + "/" + e.name);
            std::ranges::sort(paths);
            for (const std::string& p : paths)
                if (std::string more; fs_read(p, &more, 1u << 20)) ini.read(more);
        }
    // Its envs, in the file's order, those of default_envs first.
    std::vector<std::string> envs, defaults;
    for (const Ini::Section& s : ini.sections)
        if (s.name.starts_with("env:") && s.name.size() > 4) envs.push_back(s.name.substr(4));
    if (const std::optional<std::string> d = ini.get("platformio", "default_envs"))
        for (const std::string& e : Ini::list(*d))
            if (std::ranges::find(envs, e) != envs.end()) defaults.push_back(e);
    std::vector<std::string> order = defaults;
    for (const std::string& e : envs)
        if (std::ranges::find(order, e) == order.end()) order.push_back(e);

    const PioPlace place = pio_kept().get(m.host, [&] { return pio_look(m); });
    Adder a{out, "PlatformIO", place.setup, place.missing};
    const std::string build_dir = ini.get("platformio", "build_dir").value_or(".pio/build");
    const std::string core = ini.get("platformio", "core_dir").value_or(place.core);
    const std::string test_dir = ini.get("platformio", "test_dir").value_or("test");
    const bool tests = fs_kind(root + "/" + test_dir) == 2;
    if (order.empty()) {  // (a project with no env yet: pio's own default)
        a.add("Build", "pio run", true);
        a.add("Clean", "pio run -t clean");
        return true;
    }
    for (size_t i = 0; i < order.size(); ++i) {
        const std::string& env = order[i], section = "env:" + env, q = sh_word(env);
        const std::string tag = order.size() > 1 ? " (" + env + ")" : std::string();
        a.add("Build" + tag, "pio run -e " + q, i == 0);
        // The board, from the env and its board's manifest in the installed
        // platform (its MCU, and whether its firmware is reset into the boot
        // ROM by a 1200 baud touch).
        const std::string platform = platform_name(ini.get(section, "platform").value_or(""));
        const std::string board_id = ini.get(section, "board").value_or("");
        std::string mcu;
        bool touch = false;
        if (!platform.empty() && !board_id.empty() && board_id.find('/') == std::string::npos && platform.find('/') == std::string::npos)
            if (std::string manifest; fs_read(m.at + core + "/platforms/" + platform + "/boards/" + board_id + ".json", &manifest, 1u << 20)) {
                json::Doc doc;
                if (doc.parse(manifest)) {
                    mcu = std::string(doc.root()["build"]["mcu"].str());
                    touch = doc.root()["upload"]["use_1200bps_touch"].boolean();
                }
            }
        if (const std::optional<std::string> own = ini.get(section, "board_build.mcu")) mcu = *own;
        Board board;
        board.set = true;
        board.chip = mcu;
        board.port = plain_port(ini.get(section, "monitor_port"));
        if (board.port.empty()) board.port = plain_port(ini.get(section, "upload_port"));
        board.baud = std::atoi(ini.get(section, "monitor_speed").value_or("9600").c_str());  // (PlatformIO's default)
        if (board.baud <= 0) board.baud = 9600;
        board.elf = build_dir + "/" + env + "/firmware.elf";
        const std::string program = root + "/" + board.elf;
        // Uploading: blit's own flash where it can (an RP2040's or RP2350's
        // with picotool, or the .uf2 copied to its boot ROM's volume for
        // "mbed"); else PlatformIO's own upload.
        const std::string protocol = ini.get(section, "upload_protocol").value_or("");
        const bool rp = platform == "raspberrypi" || mcu.starts_with("rp2");
        if (rp && (protocol.empty() || protocol == "picotool" || protocol == "mbed")) {
            board.flash = protocol == "mbed" ? "uf2" : "picotool";
            board.touch = touch && board.flash == "picotool";
            const std::string what = board.flash == "uf2" ? "UF2 copy " + build_dir + "/" + env + "/firmware.uf2" : "picotool " + board.elf;
            a.own("Upload" + tag, what, "flash", board, program, env, "PlatformIO: Build" + tag);
        } else {
            a.add("Upload" + tag, "pio run -e " + q + " -t upload").kind = "flash";
        }
        a.own("Monitor" + tag, serial_words(board), "monitor", board, program, env, {});
        if (tests) a.add("Test" + tag, "pio test -e " + q);
        a.add("Clean" + tag, "pio run -e " + q + " -t clean");
        a.add("Compile database" + tag, "pio run -e " + q + " -t compiledb");
    }
    return true;
}

}  // namespace blit
