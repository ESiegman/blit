// One blit takes the files (instance.hpp): those a later `blit` hands
// over, and folders dropped on the window or opened from the Finder.

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

#include "app.hpp"
#include "applua.hpp"
#include "remote.hpp"
#if defined(__APPLE__)
#include "macapp.h"
#endif

namespace blit {

void App::instance_start() {
    if (!win_) return;  // (a scripted run takes nothing from anyone)
    instance_ = std::make_unique<InstanceServer>();
    const bool listening = instance_->start([this](InstanceAsk ask) {
        {
            std::lock_guard lock(instance_mu_);
            instance_in_.push_back(std::move(ask));
        }
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        ev.user.code = 21;
        SDL_PushEvent(&ev);
    });
    if (!listening) instance_.reset();  // (another blit of this program has the socket: this one is on its own)
}

void App::instance_stop() {
    if (instance_) instance_->stop();
    std::lock_guard lock(instance_mu_);
    for (InstanceAsk& ask : instance_in_) InstanceServer::answer(ask.fd, "done\n");
    instance_in_.clear();
    for (InstanceAsk& ask : instance_waits_) InstanceServer::answer(ask.fd, "done\n");  // (blit going: their editor is done)
    instance_waits_.clear();
}

// A file at a line (and column), or a folder to work in. False when it could
// not be opened.
bool App::open_place(const OpenAsk& f) {
    if (f.path.empty()) return false;
    if (!is_remote(f.path)) {
        if (struct stat st; ::stat(f.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!project_open(full_path(f.path)) && !panel_px()) panel_toggle();  // (the folder, shown)
            dirty_ = true;
            return true;
        }
    }
    if (!open_path(f.path.c_str())) return false;
    if (f.line > 0) show_place(f.line - 1, f.col > 0 ? f.col - 1 : 0);
    return true;
}

void App::instance_take() {
    std::vector<InstanceAsk> asks;
    {
        std::lock_guard lock(instance_mu_);
        asks.swap(instance_in_);
    }
    for (InstanceAsk& ask : asks) {
        bool any = false;
        for (const OpenAsk& f : ask.files) any = open_place(f) || any;
        if (ask.wait && any) instance_waits_.push_back(std::move(ask));
        else InstanceServer::answer(ask.fd, "done\n");  // (one that did not wait has had its answer)
    }
    if (win_) {
        if (SDL_GetWindowFlags(win_) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(win_);
        SDL_RaiseWindow(win_);
#if defined(__APPLE__)
        blit_mac_activate();
#endif
    }
    dirty_ = true;
}

// An asker waits until none of its files is open in a tab any more.
void App::instance_check_waits() {
    std::erase_if(instance_waits_, [&](InstanceAsk& ask) {
        for (const OpenAsk& f : ask.files) {
            Pane* where = nullptr;
            size_t at = 0;
            if (find_open(is_remote(f.path) ? f.path : full_path(f.path), &where, &at)) return false;
        }
        InstanceServer::answer(ask.fd, "done\n");
        return true;
    });
}

// `blit` in a terminal: a link to this program in a folder on the PATH,
// /usr/local/bin when it may be written to, ~/.local/bin otherwise.
void App::install_command() {
    const std::string self = program_path();
    const char* home = std::getenv("HOME");
    if (self.empty() || !home || !*home) {
        notify("blit cannot tell where it is: no command made", kError);
        return;
    }
    std::string dir = "/usr/local/bin";
    if (::access(dir.c_str(), W_OK) != 0) {
        dir = std::string(home) + "/.local";
        ::mkdir(dir.c_str(), 0755);
        dir += "/bin";
        ::mkdir(dir.c_str(), 0755);
    }
    const std::string dest = dir + "/blit";
    std::string shown = dest;
    if (shown.starts_with(home)) shown = "~" + shown.substr(std::strlen(home));
    if (struct stat st; ::lstat(dest.c_str(), &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            notify("There is a file at " + shown + " already: it was left as it is", kWarn);
            return;
        }
        char was[4096];
        const ssize_t n = ::readlink(dest.c_str(), was, sizeof was - 1);
        if (n > 0 && std::string_view(was, static_cast<size_t>(n)) == self) {
            notify("The blit command is there already: " + shown);
            return;
        }
        ::unlink(dest.c_str());
    }
    if (::symlink(self.c_str(), dest.c_str()) != 0) {
        notify("No blit command: " + shown + ": " + std::strerror(errno), kError);
        return;
    }
    bool on_path = false;
    if (const char* path = std::getenv("PATH")) {
        std::string_view rest = path;
        while (!rest.empty() && !on_path) {
            const size_t colon = rest.find(':');
            on_path = rest.substr(0, colon) == dir;
            rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        }
    }
    if (on_path) notify("blit in a terminal now opens files here (" + shown + ")");
    else notify("Made " + shown + "; its folder is not on the PATH blit was started with: add it in your shell's settings", kWarn);
}

}  // namespace blit
