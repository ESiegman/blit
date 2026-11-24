// Crash dumps in a task's or a run's output decoded (crashscan.hpp): the
// ELF they are decoded against, and the program's own panic formats. The
// ELF is the board's image of the run configuration (the task's own, if it
// is one; else the one the Run button runs, else the first with a board),
// else the newest ELF the build made (*.elf, *.axf) under the folder. It is
// read when a dump first needs it, on the thread reading the output: here,
// or from the project's machine through the connection (the agent reading
// it there is to come); and kept, with its tables as they are read, while
// it does not change.
#include <sys/stat.h>

#include <filesystem>
#include <mutex>
#include <stop_token>

#include "app.hpp"
#include "crashscan.hpp"
#include "fsio.hpp"
#include "remote.hpp"

namespace blit {

namespace {

// The ELFs read, by path, with what they were when read.
struct Cached {
    std::string path;
    i64 stamp = 0;
    u64 size = 0, used = 0;
    std::shared_ptr<const AddrSource> src;
};
std::mutex g_mu;
std::vector<Cached> g_cache;
u64 g_uses = 0;
constexpr size_t kKept = 4;  // ELFs kept at once

// The newest ELF under a folder, as a full path ("" none): walked here,
// asked of `find` there. A build's own folders are looked in (PlatformIO's
// .pio too); other hidden ones and node_modules are not.
std::string newest_elf(const std::string& root) {
    std::string host, there;
    if (split_remote(root, &host, &there)) {
        std::string out;
        const std::string cmd =
            "find . -maxdepth 6 \\( -path '*/.*' ! -path './.pio' ! -path './.pio/*' \\) -prune -o -type f \\( -name '*.elf' -o -name '*.axf' \\) -exec ls -t {} + 2>/dev/null | head -1";
        if (!fs_run(root, cmd, &out, 20'000)) return {};
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (out.empty()) return {};
        if (out.starts_with("./")) out = out.substr(2);
        return root + "/" + out;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path best;
    fs::file_time_type when;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if ((name.starts_with('.') && name != ".pio") || name == "node_modules" || it.depth() >= 5) it.disable_recursion_pending();
            continue;
        }
        if (!name.ends_with(".elf") && !name.ends_with(".axf")) continue;
        if (!it->is_regular_file(ec)) continue;
        const fs::file_time_type t = it->last_write_time(ec);
        if (best.empty() || t > when) best = it->path(), when = t;
    }
    return best.string();
}

// An ELF's tables, read now or kept from before (while its change time
// and size are the same).
std::shared_ptr<const AddrSource> elf_source(const std::string& path, std::string* why) {
    i64 stamp = 0;
    u64 size = 0;
    const bool remote = is_remote(path);
    if (remote) {
        stamp = fs_remote_stamp(path);
        if (!stamp) return *why = path + " is not there, or the connection is down", nullptr;
    } else {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return *why = path + " is not there", nullptr;
        stamp = static_cast<i64>(st.st_mtime);
        size = static_cast<u64>(st.st_size);
    }
    {
        std::lock_guard lock(g_mu);
        for (Cached& c : g_cache)
            if (c.path == path && c.stamp == stamp && c.size == size) {
                c.used = ++g_uses;
                return c.src;
            }
    }
    std::shared_ptr<const Elf> elf;
    if (remote) {
        std::string bytes;
        if (!fs_read_whole(path, &bytes, why, Elf::kMaxFile, std::stop_token{})) return nullptr;
        std::string w;
        elf = Elf::read(std::move(bytes), &w);
        if (!elf) *why = path + ": " + w;
    } else {
        elf = Elf::read_file(path, why);
    }
    if (!elf) return nullptr;
    auto src = std::make_shared<const AddrSource>(elf);
    std::lock_guard lock(g_mu);
    std::erase_if(g_cache, [&](const Cached& c) { return c.path == path; });
    if (g_cache.size() >= kKept) g_cache.erase(std::ranges::min_element(g_cache, {}, &Cached::used));
    g_cache.push_back({path, stamp, size, ++g_uses, src});
    return src;
}

}  // namespace

CrashOptions App::crash_options(const BuildTask* task, const std::string& cmd) {
    CrashOptions o;
    o.root = build_root();
    o.quiet = CrashScan::decodes_itself(cmd);
    // The run configuration whose board and patterns these are.
    const BuildTask* c = task && task->config ? task : board_config();
    if (!c) c = run_config();
    std::string elf;
    if (c) {
        o.patterns = c->crash;
        if (!c->board.elf.empty()) elf = c->board.elf.starts_with('/') || c->board.elf.starts_with("ssh://") ? c->board.elf : o.root + "/" + c->board.elf;
    }
    if (elf.empty() && o.root.empty()) return o;
    o.elf = [elf, root = o.root](std::string* why) -> std::shared_ptr<const AddrSource> {
        std::string path = elf;
        if (path.empty()) {
            path = newest_elf(root);
            if (path.empty()) return *why = "no ELF (*.elf, *.axf) under " + root + ", and no board elf in .blit/run.json", nullptr;
        }
        return elf_source(path, why);
    };
    return o;
}

}  // namespace blit
