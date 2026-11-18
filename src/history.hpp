#pragma once
// Local history: a file's text kept as it was at each save and every few
// minutes of typing, by the file's path, in blit's data folder: a folder a
// file (named by a hash of its path), each version a gzip file named by
// its time, whether it was a save, and a hash of its text (so that a text
// is kept once: not again while it is the newest).
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct HistoryEntry {
    i64 when_ms = 0;   // when it was kept (milliseconds since 1970)
    bool saved = false;  // at a save (else a pause in typing)
    u64 hash = 0;      // of its text
    std::string file;  // where it is kept
};

// The folder a file's versions are kept in, under `data_dir`.
std::string history_dir(const std::string& data_dir, const std::string& path);
// Its versions, the newest first.
std::vector<HistoryEntry> history_list(const std::string& dir);
// The text kept (false: none, or it could not be read).
bool history_read(const std::string& file, std::string* text);
// Keeps a version of `path`'s text, unless it is the newest's already, and
// lets the oldest go past `most`. False if it could not be written.
bool history_keep(const std::string& dir, const std::string& path, std::string_view text, bool saved, i64 when_ms, size_t most, std::string* err);
// A text's hash, as the versions are named by.
u64 history_hash(std::string_view text);

}  // namespace blit
