#pragma once
// Folders watched for what changes in them on disk (another program, git,
// a build): FSEvents on a Mac, everything under the top folder; inotify on
// Linux, each folder added as it is read. What changed is told as the
// folders it changed in, gathered on the watcher's thread (set_notify says
// there is something) and taken with take().
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace blit {

class DirWatch {
public:
    DirWatch();
    ~DirWatch();
    DirWatch(const DirWatch&) = delete;
    DirWatch& operator=(const DirWatch&) = delete;

    void set_notify(std::function<void()> fn);
    // The top folder (another stops the last); empty: none.
    void watch(const std::string& root);
    const std::string& root() const;
    // A folder under it, as it is read (Linux: each is watched by itself; a
    // Mac's watch covers it already).
    void add(const std::string& dir);
    // The folders something changed in since last time (each once).
    std::vector<std::string> take();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace blit
