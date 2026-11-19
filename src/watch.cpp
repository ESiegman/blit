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
    std::set<std::string> changed;  // (mu)
    std::function<void()> notify;
    std::string root;

    void heard(const std::string& dir) {
        bool first;
        {
            std::lock_guard lock(mu);
            first = changed.empty();
            changed.insert(dir);
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
    int fd = -1;
    int wake[2] = {-1, -1};
    std::map<int, std::string> dirs;  // (mu) watch descriptor -> folder
    std::set<std::string> watched;     // (mu)
    std::jthread thread;

    void start(const std::string& dir) {
        stop();
        if (dir.empty()) return;
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0 || pipe(wake) != 0) return;
        add(dir);
        thread = std::jthread([this](std::stop_token st) {
            alignas(inotify_event) char buf[16384];
            while (!st.stop_requested()) {
                pollfd p[2] = {{fd, POLLIN, 0}, {wake[0], POLLIN, 0}};
                if (::poll(p, 2, -1) <= 0) continue;
                if (p[1].revents) break;
                const ssize_t n = ::read(fd, buf, sizeof buf);
                for (ssize_t at = 0; at < n;) {
                    const inotify_event* e = reinterpret_cast<const inotify_event*>(buf + at);
                    std::string in;
                    {
                        std::lock_guard lock(mu);
                        if (const auto it = dirs.find(e->wd); it != dirs.end()) in = it->second;
                    }
                    if (!in.empty()) {
                        heard(in);
                        if ((e->mask & IN_ISDIR) && e->len) heard(in + "/" + e->name);
                    }
                    at += static_cast<ssize_t>(sizeof(inotify_event) + e->len);
                }
            }
        });
    }
    void add(const std::string& dir) {
        if (fd < 0) return;
        std::lock_guard lock(mu);
        if (!watched.insert(dir).second) return;
        const int wd = inotify_add_watch(fd, dir.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF);
        if (wd >= 0) dirs[wd] = dir;
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

std::vector<std::string> DirWatch::take() {
    std::lock_guard lock(impl_->mu);
    std::vector<std::string> out(impl_->changed.begin(), impl_->changed.end());
    impl_->changed.clear();
    return out;
}

}  // namespace blit
