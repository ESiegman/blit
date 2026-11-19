#include "watch.hpp"

#include <mutex>
#include <set>
#include <thread>

#if defined(__APPLE__)
extern "C" {
void* blit_watch_start(const char* dir, void (*heard)(void* ctx, const char* path, int dir), void* ctx);
void blit_watch_stop(void* w);
}
#elif defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#endif

namespace blit {

namespace {

#if defined(__APPLE__)
std::string parent(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string() : s == 0 ? std::string("/") : p.substr(0, s);
}
#endif

}  // namespace

struct DirWatch::Impl {
    std::mutex mu;
    std::set<std::string> changed, trees;  // (mu)
    std::function<void()> notify;
    std::string root;

    void heard(const std::string& dir, bool tree = false) {
        bool first;
        {
            std::lock_guard lock(mu);
            first = changed.empty() && trees.empty();
            (tree ? trees : changed).insert(dir);
        }
        if (first && notify) notify();
    }

#if defined(__APPLE__)
    void* mac = nullptr;

    static void heard_mac(void* ctx, const char* path, int dir) {
        Impl* self = static_cast<Impl*>(ctx);
        const std::string p = path;
        self->heard(parent(p));  // (what changed in a folder: that folder)
        if (dir) self->heard(p);
    }
    void start(const std::string& dir) {
        stop();
        if (!dir.empty()) mac = blit_watch_start(dir.c_str(), &Impl::heard_mac, this);
    }
    void stop() {
        blit_watch_stop(mac);
        mac = nullptr;
    }
    void add(const std::string&) {}
    ~Impl() { stop(); }
#elif defined(__linux__)
    // A folder watched: by the names it was added as (more than one when a
    // link leads to it too), and which folder it was on disk.
    struct Dir {
        std::vector<std::string> paths;
        dev_t dev = 0;
        ino_t ino = 0;
    };
    int fd = -1;
    int wake[2] = {-1, -1};
    std::map<int, Dir> dirs;             // (mu) watch descriptor -> folder
    std::map<std::string, int> watched;  // (mu) folder -> watch descriptor
    std::jthread thread;

    void start(const std::string& dir) {
        stop();
        if (dir.empty()) return;
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0 || pipe(wake) != 0) return;
        add(dir);
        thread = std::jthread([this, dir](std::stop_token st) {
            alignas(inotify_event) char buf[16384];
            while (!st.stop_requested()) {
                pollfd p[2] = {{fd, POLLIN, 0}, {wake[0], POLLIN, 0}};
                if (::poll(p, 2, -1) <= 0) continue;
                if (p[1].revents) break;
                const ssize_t n = ::read(fd, buf, sizeof buf);
                for (ssize_t at = 0; at < n;) {
                    const inotify_event* e = reinterpret_cast<const inotify_event*>(buf + at);
                    at += static_cast<ssize_t>(sizeof(inotify_event) + e->len);
                    if (e->mask & IN_Q_OVERFLOW) {
                        overflowed(dir);
                        continue;
                    }
                    std::vector<std::string> in;
                    {
                        std::lock_guard lock(mu);
                        const auto it = dirs.find(e->wd);
                        if (it == dirs.end()) continue;
                        in = it->second.paths;
                        // Gone (its watch with it), or moved: its names, and
                        // those of the folders watched under it, are let go
                        // of, to be watched again if they are read again.
                        if (e->mask & IN_IGNORED) forget(e->wd);
                        if (e->mask & IN_MOVE_SELF)
                            for (const std::string& path : in) forget_under(path);
                    }
                    for (const std::string& path : in) {
                        heard(path);
                        if ((e->mask & IN_ISDIR) && e->len) heard(path + "/" + e->name);
                    }
                }
            }
        });
    }
    // (mu) A watch let go of: the system has (IN_IGNORED), or it is asked to.
    void forget(int wd, bool remove = false) {
        const auto it = dirs.find(wd);
        if (it == dirs.end()) return;
        for (const std::string& path : it->second.paths)
            if (const auto w = watched.find(path); w != watched.end() && w->second == wd) watched.erase(w);
        dirs.erase(it);
        if (remove) inotify_rm_watch(fd, wd);
    }
    // (mu) The folder at a path, and every one watched under it.
    void forget_under(const std::string& path) {
        std::vector<int> wds;
        for (auto it = watched.lower_bound(path); it != watched.end() && it->first.starts_with(path); ++it)
            if (it->first.size() == path.size() || it->first[path.size()] == '/') wds.push_back(it->second);
        for (const int wd : wds) forget(wd, true);
    }
    // What happened since the queue filled up is lost: each folder watched
    // is asked whether it is still the one at its name (one that went, or
    // moved, may have said so in what was lost), and everything is looked
    // at again.
    void overflowed(const std::string& top) {
        {
            std::lock_guard lock(mu);
            std::vector<int> stale;
            for (auto& [wd, d] : dirs) {
                std::erase_if(d.paths, [&](const std::string& path) {
                    struct stat st;
                    if (::stat(path.c_str(), &st) == 0 && st.st_dev == d.dev && st.st_ino == d.ino) return false;
                    watched.erase(path);
                    return true;
                });
                if (d.paths.empty()) stale.push_back(wd);
            }
            for (const int wd : stale) forget(wd, true);
        }
        heard(top, true);
    }
    void add(const std::string& dir) {
        if (fd < 0) return;
        std::lock_guard lock(mu);
        if (watched.contains(dir)) return;
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0) return;
        const int wd = inotify_add_watch(fd, dir.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
        if (wd < 0) return;  // (not kept: tried again when it is read again)
        Dir& d = dirs[wd];
        d.paths.push_back(dir);
        d.dev = st.st_dev, d.ino = st.st_ino;
        watched[dir] = wd;
    }
    void stop() {
        if (thread.joinable()) {
            thread.request_stop();
            if (wake[1] >= 0) (void)::write(wake[1], "x", 1);
            thread.join();
        }
        for (int* f : {&fd, &wake[0], &wake[1]})
            if (*f >= 0) ::close(*f), *f = -1;
        std::lock_guard lock(mu);
        dirs.clear();
        watched.clear();
    }
    ~Impl() { stop(); }
#else
    void start(const std::string&) {}
    void stop() {}
    void add(const std::string&) {}
#endif
};

DirWatch::DirWatch() : impl_(std::make_unique<Impl>()) {}
DirWatch::~DirWatch() = default;

void DirWatch::set_notify(std::function<void()> fn) { impl_->notify = std::move(fn); }

void DirWatch::watch(const std::string& root) {
    if (root == impl_->root) return;
    impl_->root = root;
    impl_->start(root);
}

const std::string& DirWatch::root() const { return impl_->root; }

void DirWatch::add(const std::string& dir) { impl_->add(dir); }

DirWatch::Changed DirWatch::take() {
    std::lock_guard lock(impl_->mu);
    Changed out;
    out.dirs.assign(impl_->changed.begin(), impl_->changed.end());
    out.trees.assign(impl_->trees.begin(), impl_->trees.end());
    impl_->changed.clear();
    impl_->trees.clear();
    return out;
}

void DirWatch::held(const std::function<void()>& fn) {
    std::lock_guard lock(impl_->mu);
    fn();
}

}  // namespace blit
