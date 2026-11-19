#pragma once
// Folders watched for what changes in them on disk (another program, git,
// a build): FSEvents on a Mac, everything under the top folder; inotify on
// Linux, each folder added as it is read (and let go of once it has gone or
// moved). What changed is told as the folders it changed in, gathered on
// the watcher's thread (set_notify says there is something) and taken with
// take().
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
    // What changed since last time (each folder once): the folders
    // something changed in, and those that it and every folder under it are
    // to be looked at again (the system lost track of what changed there:
    // FSEvents dropped some, inotify's queue overflowed).
    struct Changed {
        std::vector<std::string> dirs, trees;
    };
    Changed take();
    // For the tests: fn run while what is heard is held back (inotify's
    // queue may overflow meanwhile).
    void held(const std::function<void()>& fn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace blit
