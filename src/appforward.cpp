// Ports on another machine reached as ports here (forward.hpp): "Reach a
// port on the project's machine…" takes a port there (8000; a host and
// port as that machine names them, host:8000; or the port here first,
// 9000:localhost:8000, as ssh -L takes it) and listens for it here, on the
// same port number when no port here is named and that one is free, else
// on one the system gives. "Stop reaching a port on another machine…"
// lists them and stops the one chosen; each lasts until then, or until
// blit quits. What goes wrong with a connection made to one (nothing
// listens at the port there) is said on the bottom line as it happens.
#include <SDL3/SDL.h>

#include <cstdlib>

#include "app.hpp"
#include "forward.hpp"
#include "remote.hpp"

namespace blit {

struct App::ForwardUi {
    std::vector<std::shared_ptr<Forward>> open;
};

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 63;
    SDL_PushEvent(&ev);
}

bool all_digits(std::string_view s) { return !s.empty() && s.size() <= 5 && s.find_first_not_of("0123456789") == std::string_view::npos; }

// What was typed: the target there, and the port here (0: none named).
bool parse_typed(std::string t, std::string* target, u16* local) {
    while (!t.empty() && t.back() == ' ') t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    *local = 0;
    if (all_digits(t)) {
        *target = "localhost:" + t;
        return std::atoi(t.c_str()) > 0 && std::atoi(t.c_str()) < 65536;
    }
    // A port here first: "9000:host:8000" (but "host:8000" and "[::1]:8000" are a target alone).
    if (const size_t colon = t.find(':'); colon != std::string::npos && all_digits(std::string_view(t).substr(0, colon)) &&
                                         t.find(':', colon + 1) != std::string::npos) {
        const int l = std::atoi(t.substr(0, colon).c_str());
        if (l <= 0 || l > 65535) return false;
        *local = static_cast<u16>(l);
        t.erase(0, colon + 1);
    }
    const size_t last = t.rfind(':');
    if (last == std::string::npos || last == 0 || !all_digits(std::string_view(t).substr(last + 1))) return false;
    const int port = std::atoi(t.c_str() + last + 1);
    if (port <= 0 || port > 65535) return false;
    *target = t;
    return true;
}

}  // namespace

void App::forward_pick() {
    const std::string root = build_root();
    std::string host, there;
    if (!split_remote(root, &host, &there)) return notify("The project is on this machine: its ports are reached here as they are");
    find_choose(kFindInput, "Port on " + host + " to reach", "", [this, host](const std::string& typed) {
        if (!typed.empty()) forward_open(host, typed);
    });
}

void App::forward_open(const std::string& host, const std::string& typed) {
    std::string target;
    u16 local = 0;
    if (!parse_typed(typed, &target, &local)) return notify("Not a port: " + typed + " (a port, HOST:PORT, or LOCAL:HOST:PORT)", 1);
    if (!forwardui_) forwardui_ = std::make_shared<ForwardUi>();
    for (const std::shared_ptr<Forward>& f : forwardui_->open)
        if (f->host() == host && f->target() == target && (!local || f->port() == local))
            return notify(f->address() + " here reaches " + target + " on " + host + " already");
    std::string err;
    std::shared_ptr<Forward> f;
    std::string busy;
    if (local) {
        f = Forward::open(host, target, local, &err);
    } else {
        // (The same number here as there, when it is free.)
        const u16 same = static_cast<u16>(std::atoi(target.c_str() + target.rfind(':') + 1));
        f = Forward::open(host, target, same, &err);
        if (!f) {
            busy = " (" + err + ")";
            f = Forward::open(host, target, 0, &err);
        }
    }
    if (!f) return notify("Cannot reach " + target + " on " + host + ": " + err, 1);
    f->told = [] { wake(); };
    forwardui_->open.push_back(f);
    notify(f->address() + " here reaches " + target + " on " + host + busy);
}

void App::forward_stop_pick() {
    if (!forwardui_ || forwardui_->open.empty()) return notify("No port on another machine is being reached");
    std::vector<std::string> items;
    std::vector<std::weak_ptr<Forward>> which;
    for (const std::shared_ptr<Forward>& f : forwardui_->open) {
        const int n = f->connections();
        items.push_back(f->address() + "  " + f->target() + " on " + f->host() + (n ? "  " + std::to_string(n) + (n == 1 ? " connection" : " connections") : ""));
        which.push_back(f);
    }
    find_choose_item("Stop reaching", std::move(items), [this, which](int i) {
        if (i < 0 || static_cast<size_t>(i) >= which.size() || !forwardui_) return;
        const std::shared_ptr<Forward> f = which[static_cast<size_t>(i)].lock();
        if (!f) return;
        const std::string said = f->address() + " no longer reaches " + f->target() + " on " + f->host();
        std::erase(forwardui_->open, f);
        notify(said);
    });
}

void App::forward_sync() {
    if (!forwardui_) return;
    for (const std::shared_ptr<Forward>& f : forwardui_->open)
        for (const std::string& e : f->take_errors()) notify(e, 1);
}

std::string App::forward_dump() {
    if (!forwardui_ || forwardui_->open.empty()) return "no ports reached";
    std::string out;
    for (const std::shared_ptr<Forward>& f : forwardui_->open)
        out += (out.empty() ? "" : " | ") + f->address() + " " + f->target() + " on " + f->host() + " " + std::to_string(f->connections());
    return out;
}

}  // namespace blit
