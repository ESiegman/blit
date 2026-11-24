#include "libraries.hpp"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <cstdio>
#include <ctime>

#include <algorithm>
#include <string_view>

#include "fsio.hpp"
#include "json.hpp"
#include "missing.hpp"
#include "remote.hpp"

namespace blit {

namespace {

bool is_root(const std::string& dir) { return fs_kind(dir + "/scripts/buildsystems/vcpkg.cmake") == 1 && fs_kind(dir + "/ports") == 2; }

std::string trim(std::string_view s) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return std::string(s);
}

// For sh: in single quotes, with any single quote in it closed and written out.
std::string sh_quote(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

}  // namespace

std::string vcpkg_root(const std::string& where) {
    std::string host, there;
    if (split_remote(where, &host, &there)) {
        // That machine's: where it is usually cloned (its home, from the path, when it is under one).
        std::vector<std::string> homes;
        for (const char* base : {"/home/", "/Users/"})
            if (there.starts_with(base)) {
                const size_t end = there.find('/', std::string_view(base).size());
                homes.push_back(there.substr(0, end));
            }
        if (there == "/root" || there.starts_with("/root/")) homes.push_back("/root");
        std::vector<std::string> tries;
        for (const std::string& h : homes)
            for (const char* sub : {"/vcpkg", "/Dev/vcpkg", "/.vcpkg", "/src/vcpkg"}) tries.push_back(h + sub);
        for (const char* t : {"/opt/vcpkg", "/usr/local/vcpkg", "/usr/share/vcpkg"}) tries.emplace_back(t);
        for (const std::string& t : tries)
            if (is_root(remote_path(host, t))) return t;
        return {};
    }
    if (const char* env = SDL_getenv("VCPKG_ROOT"); env && *env && is_root(env)) return env;
    // The vcpkg on the PATH, when it is in its folder (a clone's).
    if (const char* path = SDL_getenv("PATH"))
        for (std::string_view p(path); !p.empty();) {
            const size_t colon = p.find(':');
            const std::string dir(p.substr(0, colon));
            p = colon == std::string_view::npos ? std::string_view() : p.substr(colon + 1);
            char real[PATH_MAX];
            if (dir.empty() || fs_kind(dir + "/vcpkg") != 1 || !::realpath((dir + "/vcpkg").c_str(), real)) continue;
            std::string at(real);
            at.erase(at.find_last_of('/'));
            if (is_root(at)) return at;
        }
    const char* home = SDL_getenv("HOME");
    const std::string h = home && *home ? home : "";
    for (const std::string& t : {h + "/vcpkg", h + "/Dev/vcpkg", h + "/.vcpkg", h + "/src/vcpkg", std::string("/opt/vcpkg"), std::string("/usr/local/vcpkg")})
        if (is_root(t)) return t;
    return {};
}

std::vector<VcpkgPort> vcpkg_ports(const std::string& root, bool descriptions) {
    std::vector<VcpkgPort> out;
    if (root.empty()) return out;
    // Every port and its version, from one file; the ports' folders otherwise.
    std::string text;
    json::Doc d;
    if (fs_read(root + "/versions/baseline.json", &text, 16u << 20) && d.parse(std::move(text)))
        for (json::Value p : d.root()["default"]) {
            VcpkgPort port;
            port.name = std::string(p.key());
            port.version = std::string(p["baseline"].str());
            out.push_back(std::move(port));
        }
    if (out.empty())
        for (const FsEntry& e : fs_list(root + "/ports"))
            if (e.dir) out.push_back({e.name, {}, {}});
    std::sort(out.begin(), out.end(), [](const VcpkgPort& a, const VcpkgPort& b) { return a.name < b.name; });
    if (descriptions)
        for (VcpkgPort& p : out) {
            json::Doc m;
            if (!fs_read(root + "/ports/" + p.name + "/vcpkg.json", &text, 1u << 20) || !m.parse(std::move(text))) continue;
            const json::Value desc = m.root()["description"];
            if (desc.is_string()) p.description = std::string(desc.str());
            else if (desc.is_array() && desc.size()) p.description = std::string(desc.at(0).str());
            if (p.version.empty())
                for (const char* key : {"version", "version-semver", "version-date", "version-string"})
                    if (p.version.empty()) p.version = std::string(m.root()[key].str());
        }
    return out;
}

std::string vcpkg_usage(const std::string& root, const std::string& port) {
    std::string text;
    if (root.empty() || !fs_read(root + "/ports/" + port + "/usage", &text, 1u << 16)) return {};
    // The first block of CMake in it (later ones are other ways of doing the same).
    std::string out;
    bool in_block = false, linked = false;
    for (std::string_view rest(text); !rest.empty();) {
        const size_t nl = rest.find('\n');
        const std::string line = trim(rest.substr(0, nl));
        rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
        const bool cmake = line.starts_with("find_package(") || line.starts_with("find_path(") || line.starts_with("find_library(") ||
                           line.starts_with("target_link_libraries(") || line.starts_with("target_include_directories(") ||
                           line.starts_with("target_compile_definitions(") || line.starts_with("target_compile_features(");
        if (cmake) {
            if (linked && line.starts_with("find_")) break;  // (a second way: find_package again after the targets)
            in_block = true;
            linked |= line.starts_with("target_");
            out += line + "\n";
        } else if (in_block && line.empty() && linked) {
            break;
        } else if (in_block && !line.empty() && !line.starts_with("#")) {
            if (linked) break;
        }
    }
    return out;
}

std::string vcpkg_baseline(const std::string& root) {
    std::string head;
    if (root.empty() || !fs_read(root + "/.git/HEAD", &head, 4096)) return {};
    head = trim(head);
    if (!head.starts_with("ref: ")) return head.size() == 40 ? head : std::string();
    const std::string ref = head.substr(5);
    std::string commit;
    if (fs_read(root + "/.git/" + ref, &commit, 4096) && trim(commit).size() == 40) return trim(commit);
    std::string packed;
    if (fs_read(root + "/.git/packed-refs", &packed, 8u << 20))
        for (size_t at = 0; at < packed.size();) {
            const size_t nl = packed.find('\n', at);
            const std::string line = packed.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? packed.size() : nl + 1;
            if (line.size() > 41 && line.substr(41) == ref) return line.substr(0, 40);
        }
    return {};
}

// ---------------------------------------------------------------- ROCm

std::string rocm_root(const std::string& where) {
    std::string host, there;
    const bool remote = split_remote(where, &host, &there);
    auto at = [&](const std::string& path) { return remote ? remote_path(host, path) : path; };
    auto is_rocm = [&](const std::string& path) { return fs_kind(at(path) + "/lib/cmake") == 2; };
    if (!remote)
        if (const char* env = SDL_getenv("ROCM_PATH"); env && *env && is_rocm(env)) return env;
    if (remote) {  // (its ROCM_PATH, as a login shell there has it)
        std::string out;
        if (fs_run(where, "printf %s \"$ROCM_PATH\"", &out, 5000) && !out.empty() && is_rocm(trim(out))) return trim(out);
    }
    if (is_rocm("/opt/rocm")) return "/opt/rocm";
    std::string best;
    for (const FsEntry& e : fs_list(at("/opt")))
        if (e.dir && e.name.starts_with("rocm-") && e.name > best && is_rocm("/opt/" + e.name)) best = e.name;
    return best.empty() ? std::string() : "/opt/" + best;
}

std::vector<VcpkgPort> rocm_packages(const std::string& root) {
    std::vector<VcpkgPort> out;
    if (root.empty()) return out;
    for (const FsEntry& e : fs_list(root + "/lib/cmake"))
        if (e.dir) out.push_back({e.name, {}, {}});
    return out;
}

std::string rocm_usage(const std::string& root, const std::string& package) {
    // Its imported targets: add_library(roc::rocblas SHARED IMPORTED) and the like, in its *targets*.cmake.
    const std::string dir = root + "/lib/cmake/" + package;
    std::vector<std::string> targets;
    for (const FsEntry& e : fs_list(dir)) {
        std::string lower = e.name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (e.dir || lower.find("targets") == std::string::npos || !lower.ends_with(".cmake") || lower.find("-targets-") != std::string::npos) continue;
        std::string text;
        if (!fs_read(dir + "/" + e.name, &text, 1u << 20)) continue;
        for (size_t at = 0; (at = text.find("add_library(", at)) != std::string::npos;) {
            at += 12;
            const size_t end = text.find(')', at);
            if (end == std::string::npos) break;
            const std::string args = text.substr(at, end - at);
            if (args.find("IMPORTED") == std::string::npos) continue;
            const std::string name = args.substr(0, args.find_first_of(" \t\n"));
            if (!name.empty() && std::find(targets.begin(), targets.end(), name) == targets.end()) targets.push_back(name);
        }
    }
    if (targets.empty()) return {};
    std::string out = "find_package(" + package + " REQUIRED)\ntarget_link_libraries(main PRIVATE";
    for (size_t i = 0; i < targets.size() && i < 3; ++i) out += " " + targets[i];
    return out + ")\n";
}

std::string gpu_targets(const std::string& where, const std::string& rocm) {
    std::string out;
    const std::string bin = rocm.empty() ? std::string("/opt/rocm") : rocm;
    if (!fs_run(where, "rocm_agent_enumerator 2>/dev/null || " + sh_quote(bin + "/bin/rocm_agent_enumerator"), &out, 8000) && out.empty()) return {};
    std::vector<std::string> seen;
    for (size_t at = 0; at < out.size();) {
        const size_t nl = out.find('\n', at);
        const std::string word = trim(std::string_view(out).substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        at = nl == std::string::npos ? out.size() : nl + 1;
        if (word.starts_with("gfx") && word != "gfx000" && std::find(seen.begin(), seen.end(), word) == seen.end()) seen.push_back(word);
    }
    std::string list;
    for (const std::string& w : seen) list += (list.empty() ? "" : ";") + w;
    return list;
}

// ---------------------------------------------------------------- pkg-config

std::vector<VcpkgPort> pkgconfig_modules(const std::string& where, bool* installed) {
    std::vector<VcpkgPort> out;
    std::string text;
    const bool ran = fs_run(where,
                            "command -v pkg-config >/dev/null 2>&1 || command -v pkgconf >/dev/null 2>&1 || { echo blit-no-pkg-config; exit 1; }; "
                            "pkg-config --list-all 2>/dev/null || pkgconf --list-all",
                            &text, 15000);
    if (installed) *installed = ran || !text.starts_with("blit-no-pkg-config");
    if (!ran && (text.empty() || text.starts_with("blit-no-pkg-config"))) return out;
    for (size_t at = 0; at < text.size();) {
        const size_t nl = text.find('\n', at);
        const std::string_view line = std::string_view(text).substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string_view::npos || sp == 0) continue;
        out.push_back({std::string(line.substr(0, sp)), {}, trim(line.substr(sp))});
    }
    std::sort(out.begin(), out.end(), [](const VcpkgPort& a, const VcpkgPort& b) { return a.name < b.name; });
    out.erase(std::unique(out.begin(), out.end(), [](const VcpkgPort& a, const VcpkgPort& b) { return a.name == b.name; }), out.end());
    return out;
}

// ---------------------------------------------------------------- PyPI

std::vector<VcpkgPort> pypi_top(const std::string& cache, std::string* why) {
    std::vector<VcpkgPort> out;
    const std::string file = cache + "/pypi-top.json";
    struct stat st;
    const bool fresh = ::stat(file.c_str(), &st) == 0 && st.st_mtime + 7 * 24 * 3600 > ::time(nullptr);
    std::string said;  // (why the fetch failed: curl's own words, or that there is no curl)
    if (!fresh) {
        std::string err;
        fs_mkdirs(cache, &err);
        const char* own = std::getenv("BLIT_PYPI_URL");  // (another, for the tests)
        std::string url = own && *own ? own : "https://hugovk.dev/top-pypi-packages/top-pypi-packages.min.json";
        if (url.starts_with('/')) url = "file://" + url;
        if (!fs_run(cache, "command -v curl >/dev/null 2>&1 || { echo blit-no-curl; exit 1; }; curl -fsSL --compressed -o pypi-top.json.part " + sh_quote(url) +
                               " 2>&1 && mv pypi-top.json.part pypi-top.json",
                    &said, 30000)) {
            said = said.starts_with("blit-no-curl") ? not_installed("curl", {}) : trim(said);
            said = said.substr(said.find_last_of('\n') + 1);  // (npos + 1: all of it)
            if (said.empty()) said = "curl did not finish";
        } else {
            said.clear();
        }
    }
    std::string text;
    json::Doc d;
    if (!fs_read(file, &text, 32u << 20) || !d.parse(std::move(text))) {
        if (why) *why = said.empty() ? "its list could not be read" : said;
        return out;
    }
    for (json::Value row : d.root()["rows"]) {
        const double n = row["download_count"].number();
        char note[32];
        if (n >= 1e9) std::snprintf(note, sizeof note, "%.1fB a month", n / 1e9);
        else if (n >= 1e6) std::snprintf(note, sizeof note, "%.1fM a month", n / 1e6);
        else std::snprintf(note, sizeof note, "%.0fK a month", n / 1e3);
        out.push_back({std::string(row["project"].str()), {}, note});
    }
    return out;
}

}  // namespace blit
