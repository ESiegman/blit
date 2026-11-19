#include "history.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "fsio.hpp"

namespace blit {

namespace {

u64 fnv(std::string_view s) {
    u64 h = 1469598103934665603ull;
    for (const unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

std::string hex(u64 v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// "<ms>-<s|t>-<hash>.gz"
bool parse_name(const std::string& name, HistoryEntry* e) {
    if (!name.ends_with(".gz")) return false;
    const size_t a = name.find('-'), b = a == std::string::npos ? a : name.find('-', a + 1);
    if (b == std::string::npos || b != a + 2 || (name[a + 1] != 's' && name[a + 1] != 't')) return false;
    char* end = nullptr;
    e->when_ms = std::strtoll(name.c_str(), &end, 10);
    if (end != name.c_str() + a) return false;
    e->saved = name[a + 1] == 's';
    e->hash = std::strtoull(name.c_str() + b + 1, &end, 16);
    return end == name.c_str() + name.size() - 3;
}

}  // namespace

u64 history_hash(std::string_view text) { return fnv(text); }

std::string history_dir(const std::string& data_dir, const std::string& path) { return data_dir + "/history/" + hex(fnv(path)); }

std::vector<HistoryEntry> history_list(const std::string& dir) {
    std::vector<HistoryEntry> out;
    for (const FsEntry& f : fs_list(dir)) {
        HistoryEntry e;
        if (f.dir || !parse_name(f.name, &e)) continue;
        e.file = dir + "/" + f.name;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return a.when_ms > b.when_ms; });
    return out;
}

bool history_read(const std::string& file, std::string* text) {
    gzFile f = gzopen(file.c_str(), "rb");
    if (!f) return false;
    text->clear();
    char buf[1 << 16];
    int n = 0;
    while ((n = gzread(f, buf, sizeof buf)) > 0) text->append(buf, static_cast<size_t>(n));
    const bool ok = n == 0;
    gzclose(f);
    return ok;
}

bool history_keep(const std::string& dir, const std::string& path, std::string_view text, bool saved, i64 when_ms, size_t most, std::string* err) {
    std::string unasked;
    if (!err) err = &unasked;  // (fs_mkdirs and fs_write say why to somewhere)
    const u64 h = fnv(text);
    std::vector<HistoryEntry> have = history_list(dir);
    if (!have.empty() && have.front().hash == h) {  // (as the newest is: kept already; a save marks it saved)
        if (saved && !have.front().saved) {
            const std::string to = dir + "/" + std::to_string(have.front().when_ms) + "-s-" + hex(h) + ".gz";
            std::rename(have.front().file.c_str(), to.c_str());
        }
        return true;
    }
    if (!fs_mkdirs(dir, err)) return false;
    const std::string where = dir + "/path";
    if (fs_kind(where) != 1) fs_write(where, path + "\n", err);
    i64 when = when_ms;
    if (!have.empty() && when <= have.front().when_ms) when = have.front().when_ms + 1;  // (in order, however fast)
    const std::string file = dir + "/" + std::to_string(when) + (saved ? "-s-" : "-t-") + hex(h) + ".gz";
    const std::string part = file + ".part";
    gzFile f = gzopen(part.c_str(), "wb6");
    if (!f) {
        *err = "cannot write " + part;
        return false;
    }
    size_t at = 0;
    bool ok = true;
    while (at < text.size() && ok) {
        const unsigned n = static_cast<unsigned>(std::min<size_t>(text.size() - at, 1u << 20));
        ok = gzwrite(f, text.data() + at, n) == static_cast<int>(n);
        at += n;
    }
    ok = gzclose(f) == Z_OK && ok;
    if (!ok || std::rename(part.c_str(), file.c_str()) != 0) {
        std::remove(part.c_str());
        *err = "cannot write " + file;
        return false;
    }
    have.insert(have.begin(), HistoryEntry{when, saved, h, file});
    for (size_t i = most; i < have.size(); ++i) std::remove(have[i].file.c_str());
    return true;
}

}  // namespace blit
