#include "notify.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "remote.hpp"

#if defined(__APPLE__)
#include "macapp.h"
#endif

extern char** environ;

namespace blit {

namespace {

std::atomic<int> g_handing{0};  // (notifications on their way)

// A program's path, as a shell's PATH would find it ("": none).
std::string on_path(const char* name) {
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "";
    while (!rest.empty()) {
        const size_t colon = rest.find(':');
        const std::string_view dir = rest.substr(0, colon);
        rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        if (dir.empty()) continue;
        std::string at = std::string(dir) + "/" + name;
        if (::access(at.c_str(), X_OK) == 0) return at;
    }
    return {};
}

#if !defined(__APPLE__)
// A string as GVariant's text has it (what gdbus reads its arguments as).
std::string gvariant(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'' || c == '\\') out += '\\';
        out += c;
    }
    return out + "'";
}
#endif

// The program run with its output thrown away, and waited for.
void run(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (const int fd : {0, 1, 2}) posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
    pid_t pid = 0;
    if (posix_spawn(&pid, args[0], &fa, nullptr, args.data(), environ) == 0) {
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
    }
    posix_spawn_file_actions_destroy(&fa);
}

}  // namespace

std::vector<std::string> notify_command(const std::string& title, const std::string& body) {
#if defined(__APPLE__)
    // (Its words are argv's, so nothing in them is AppleScript.)
    if (std::string osa = on_path("osascript"); !osa.empty())
        return {osa, "-e", "on run argv", "-e", "display notification (item 2 of argv) with title (item 1 of argv)", "-e", "end run", title, body};
#else
    if (std::string gdbus = on_path("gdbus"); !gdbus.empty())
        return {gdbus, "call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path", "/org/freedesktop/Notifications", "--method",
                "org.freedesktop.Notifications.Notify", "'blit'", "0", "'blit'", gvariant(title), gvariant(body), "[]", "{}", "int32 -1"};  // (typed: a bare -1 is read as an option, and nothing is sent)
    if (std::string send = on_path("notify-send"); !send.empty()) return {send, "--app-name=blit", "--icon=blit", "--", title, body};
#endif
    return {};
}

std::string notify_missing() {
    if (!notify_command({}, {}).empty()) return {};
#if defined(__APPLE__)
    if (program_path().find(".app/Contents/MacOS/") != std::string::npos) return {};  // (blit.app: the notification centre, no program)
    return "osascript";
#else
    return "notify-send";
#endif
}

void system_notify(const std::string& title, const std::string& body) {
    ++g_handing;
    std::thread([title, body] {
#if defined(__APPLE__)
        if (blit_mac_notify(title.c_str(), body.c_str())) {  // (blit.app: its own, under its name and icon)
            --g_handing;
            return;
        }
#endif
        if (const std::vector<std::string> argv = notify_command(title, body); !argv.empty()) run(argv);
        --g_handing;
    }).detach();
}

void system_notify_wait() {
    for (int waited = 0; g_handing.load() > 0 && waited < 5000; waited += 5) ::usleep(5000);
}

}  // namespace blit
