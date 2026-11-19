#pragma once
// Files and folders, here or on another machine (ssh://host/path, through
// blit's agent): what the build systems are found from and new projects
// are written with.
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct FsEntry {
    std::string name;
    bool dir = false;
};

// The folder's entries, sorted by name (none if it cannot be read).
std::vector<FsEntry> fs_list(const std::string& dir);
// 0: nothing there, 1: a file, 2: a folder.
int fs_kind(const std::string& path);
// The file's text (at most `most` bytes); false if it cannot be read.
bool fs_read(const std::string& path, std::string* text, u64 most = 4u << 20);
// A whole file, as large as `most` bytes (a picture, a PDF), here or on
// another machine, whose connection is waited for a while (it may be coming
// up; not past `stop`, so that quitting does not wait). False, and why, if
// it cannot be read or is larger.
bool fs_read_whole(const std::string& path, std::string* bytes, std::string* why, u64 most, std::stop_token stop);
bool fs_write(const std::string& path, std::string_view text, std::string* err);
// The folder and those above it that are not there yet.
bool fs_mkdirs(const std::string& path, std::string* err);
// blit's cache folder, here ("" if there is none): the GPU's compiled
// pipelines, pictures fetched from the web and a notebook's pictures.
std::string cache_dir();
// A command run by a login shell in `dir` (another machine's: run there by
// blit's agent), its output collected; false if it could not run, failed,
// or took longer than `ms`. (For small questions: what is installed where.)
bool fs_run(const std::string& dir, const std::string& command, std::string* out, int ms = 10000);

}  // namespace blit
