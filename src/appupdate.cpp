// Updates: the latest release on GitHub looked for (at the start, once a
// day, with updates.check; or with "Check for updates"), and, if it is newer
// than this blit and the user says so, fetched, its SHA-256 checked against
// the .sha256 published beside it, and put in place of this one: blit.app
// in its folder (the new one unpacked beside it, then the two swapped), or
// the AppImage (written beside it, then renamed over it). The new blit is
// the one that starts next time. A blit installed some other way (the
// tarball, Homebrew, a build) is only told of it.
//
// BLIT_UPDATES_URL names another place to ask (the tests: a file:// one),
// BLIT_UPDATE_TARGET another blit to replace, and BLIT_UPDATE_VERSION
// another version for this one.
#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "app.hpp"
#include "fsio.hpp"
#include "grammars.hpp"
#include "json.hpp"

#ifndef BLIT_VERSION
#define BLIT_VERSION "0.0.0"
#endif

namespace blit {

namespace {

constexpr i64 kDaySeconds = 24 * 60 * 60;

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string latest_url() {
    const std::string own = env("BLIT_UPDATES_URL");
    return own.empty() ? "https://api.github.com/repos/ESiegman/blit/releases/latest" : own;
}

// Whether version a comes after `than`, as semver orders them: "1.2.3"
// (a "v" in front, and a "+build" after, left out) by its numbers; then a
// pre-release ("-rc.1") before the release, and two of them by their
// dot-separated words in turn (numbers as numbers and before any other
// word, which go in ASCII order; fewer words first, the rest the same).
bool newer(std::string_view a, std::string_view than) {
    struct Version {
        std::vector<long> core;
        std::vector<std::string_view> pre;
    };
    auto parse = [](std::string_view v) {
        if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) v.remove_prefix(1);
        v = v.substr(0, v.find('+'));
        Version out;
        const size_t dash = v.find('-');
        const std::string_view core = v.substr(0, dash);
        for (size_t at = 0; at <= core.size();) {
            size_t dot = core.find('.', at);
            if (dot == std::string_view::npos) dot = core.size();
            out.core.push_back(std::strtol(std::string(core.substr(at, dot - at)).c_str(), nullptr, 10));
            at = dot + 1;
        }
        while (out.core.size() < 3) out.core.push_back(0);
        if (dash != std::string_view::npos)
            for (size_t at = dash + 1; at <= v.size();) {
                size_t dot = v.find('.', at);
                if (dot == std::string_view::npos) dot = v.size();
                out.pre.push_back(v.substr(at, dot - at));
                at = dot + 1;
            }
        return out;
    };
    auto numeric = [](std::string_view w) { return !w.empty() && std::ranges::all_of(w, [](char c) { return c >= '0' && c <= '9'; }); };
    const Version x = parse(a), y = parse(than);
    if (x.core != y.core) return x.core > y.core;
    if (x.pre.empty() || y.pre.empty()) return x.pre.empty() && !y.pre.empty();  // (the release after its pre-releases)
    for (size_t i = 0; i < x.pre.size() && i < y.pre.size(); ++i) {
        const std::string_view p = x.pre[i], q = y.pre[i];
        if (p == q) continue;
        const bool pn = numeric(p), qn = numeric(q);
        if (pn && qn) {  // (as numbers, of any length)
            const std::string_view m = p.substr(std::min(p.find_first_not_of('0'), p.size())), n = q.substr(std::min(q.find_first_not_of('0'), q.size()));
            if (m == n) continue;
            return m.size() != n.size() ? m.size() > n.size() : m > n;
        }
        if (pn != qn) return qn;
        return p > q;
    }
    return x.pre.size() > y.pre.size();
}

// This blit's version, as the updates see it.
std::string running() {
    const std::string own = env("BLIT_UPDATE_VERSION");
    return own.empty() ? std::string(BLIT_VERSION) : own;
}

// The blit this one would be replaced as: its blit.app, or its AppImage
// (empty: installed some other way).
std::string target() {
    if (std::string own = env("BLIT_UPDATE_TARGET"); !own.empty()) return own;
#if defined(__APPLE__)
    if (const char* base = SDL_GetBasePath()) {
        const std::string b = base;
        if (const size_t app = b.find(".app/"); app != std::string::npos) return b.substr(0, app + 4);
    }
    return {};
#else
    return env("APPIMAGE");
#endif
}

// The release's file for it.
std::string asset_for(const std::string& where, const std::string& version) {
    if (where.ends_with(".app")) return "blit-" + version + "-macos-arm64.zip";
    return "blit-" + version + "-x86_64.AppImage";
}

// What a program said, its first line (for a toast).
std::string said(const std::string& err, const std::string& out) {
    const std::string& s = err.empty() ? out : err;
    std::string line = s.substr(0, s.find('\n'));
    return line.empty() ? std::string("it failed") : line;
}

// A SHA-256 as shasum or a .sha256 gives it: the first word, in lower case
// (hex in upper case is the same sum).
std::string sum_of(std::string_view s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    const size_t b = s.find_first_of(" \t\r\n", a);
    std::string out(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string folder_of(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string(".") : s == 0 ? std::string("/") : p.substr(0, s);
}

std::string stamp_path() { return grammar_dir() + "/update-checked"; }

}  // namespace

std::string App::version() { return BLIT_VERSION; }

// The latest release asked about: by the user (asked: told either way), or
// at the start (at most once a day, and only with updates.check on).
void App::update_check(bool asked) {
    if (!asked) {
        if (!cfg_.update_check) return;
#if BLIT_DEV
        if (env("BLIT_UPDATES_URL").empty()) return;  // (a build of one's own is not replaced by a release)
#endif
        std::string last;
        std::string why;
        if (fs_read_whole(stamp_path(), &last, &why, 64, {}) && std::time(nullptr) - std::atoll(last.c_str()) < kDaySeconds) return;
        SDL_CreateDirectory(grammar_dir().c_str());
        const std::string now = std::to_string(static_cast<long long>(std::time(nullptr)));
        SDL_SaveFile(stamp_path().c_str(), now.data(), now.size());
    }
    if (update_busy_) {
        if (asked) notify("Already looking for an update");
        return;
    }
    update_busy_ = true;
    SDL_CreateDirectory(grammar_dir().c_str());
    script_run({"curl", "-fsSL", "--proto", "=https,file", "--max-time", "30", "-H", "Accept: application/vnd.github+json", latest_url()}, grammar_dir(),
               [this, asked](int code, const std::string& out, const std::string& err) {
                   update_busy_ = false;
                   if (code != 0) {
                       if (asked) notify("Could not ask for the latest release: " + said(err, out), 2);
                       return;
                   }
                   json::Doc doc;
                   if (!doc.parse(out)) return asked ? notify("The answer about the latest release was not JSON", 2) : void();
                   const std::string tag(doc.root()["tag_name"].str());
                   const std::string version = tag.starts_with('v') ? tag.substr(1) : tag;
                   if (version.empty() || !newer(version, running())) {
                       if (asked) notify("blit " + running() + " is the latest");
                       return;
                   }
                   const std::string where = target();
                   if (where.empty()) return notify("blit " + version + " is out (this one is " + running() + ")");
                   const std::string want = asset_for(where, version);
                   std::string url, sum;
                   for (const json::Value a : doc.root()["assets"]) {
                       const std::string name(a["name"].str());
                       if (name == want) url = std::string(a["browser_download_url"].str());
                       if (name == want + ".sha256") sum = std::string(a["browser_download_url"].str());
                   }
                   if (url.empty() || sum.empty()) return notify("blit " + version + " is out, with no " + want + " (and its .sha256) to install", 1);
                   if (!confirm("blit " + version + " is out (this one is " + running() + "). Install it? It starts next time.", "Install")) return;
                   update_fetch(version, where, url, sum);
               });
}

// The release's file and its sum fetched (into blit's data folder), the
// sum checked, then put in place.
void App::update_fetch(const std::string& version, const std::string& where, const std::string& url, const std::string& sum) {
    const std::string dir = grammar_dir() + "/update";
    SDL_CreateDirectory(dir.c_str());
    const std::string file = dir + "/" + url.substr(url.find_last_of('/') + 1);
    update_busy_ = true;
    notify("Fetching blit " + version + "\xE2\x80\xA6");
    script_run({"curl", "-fsSL", "--proto", "=https,file", "--max-time", "600", "-o", file, url, "-o", file + ".sha256", sum}, dir,
               [this, version, where, file](int code, const std::string& out, const std::string& err) {
                   if (code != 0) {
                       update_busy_ = false;
                       return notify("Could not fetch blit " + version + ": " + said(err, out), 2);
                   }
#if defined(__APPLE__)
                   std::vector<std::string> sum_argv = {"shasum", "-a", "256", file};
#else
                   std::vector<std::string> sum_argv = {"sha256sum", file};
#endif
                   script_run(std::move(sum_argv), folder_of(file), [this, version, where, file](int summed, const std::string& said, const std::string&) {
                       std::string want, why;
                       fs_read_whole(file + ".sha256", &want, &why, 4096, {});
                       if (summed != 0 || sum_of(said).empty() || sum_of(said) != sum_of(want)) {
                           update_busy_ = false;
                           SDL_RemovePath(file.c_str());
                           return notify("blit " + version + " came with the wrong SHA-256: not installed", 2);
                       }
                       update_install(version, where, file);
                   });
               });
}

// The checked file put in place of the blit there, by a few lines of sh
// (rm -rf and mv, which follow no links): blit.app unpacked beside it and
// the two swapped, the old one put back if the new one cannot go in; an
// AppImage copied beside it, made a program, and renamed over it.
void App::update_install(const std::string& version, const std::string& where, const std::string& file) {
    const std::string folder = folder_of(where);
    if (::access(folder.c_str(), W_OK) != 0) {
        update_busy_ = false;
        return notify("blit " + version + " is fetched, but " + folder + " cannot be written to: it is in " + file, 2);
    }
    const char* app_swap =
        "fresh=\"$(dirname \"$1\")/.blit-update\"; old=\"$(dirname \"$1\")/.blit-old.app\"; "
        "rm -rf \"$fresh\" \"$old\" && mkdir \"$fresh\" && ditto -x -k \"$2\" \"$fresh\" || exit 1; "
        "new=$(ls -d \"$fresh\"/*.app | head -1); [ -d \"$new\" ] || { rm -rf \"$fresh\"; echo 'no app in it' >&2; exit 1; }; "
        "mv \"$1\" \"$old\" || exit 1; "
        "mv \"$new\" \"$1\" || { mv \"$old\" \"$1\"; exit 1; }; "
        "rm -rf \"$old\" \"$fresh\"";
    const char* file_swap =
        "fresh=\"$(dirname \"$1\")/.blit-update.AppImage\"; "
        "cp \"$2\" \"$fresh\" && chmod 755 \"$fresh\" && mv -f \"$fresh\" \"$1\" || { rm -f \"$fresh\"; exit 1; }";
    script_run({"/bin/sh", "-c", where.ends_with(".app") ? app_swap : file_swap, "sh", where, file}, folder,
               [this, version, file](int code, const std::string& out, const std::string& err) {
                   update_busy_ = false;
                   if (code != 0) return notify("blit " + version + " could not be put in place: " + said(err, out), 2);
                   SDL_RemovePath(file.c_str());
                   SDL_RemovePath((file + ".sha256").c_str());
                   notify("blit " + version + " is installed: it starts next time");
               });
}

}  // namespace blit
