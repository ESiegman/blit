#pragma once
// What blit and its agent on another machine say to one another, over the
// standard input and output of the agent (which ssh carries). Each message is
// a frame: its length (4 bytes, little-endian, not counting itself), an
// operation (1 byte), a request's number (4 bytes), and what goes with it.
// Numbers are little-endian; a string or a run of bytes is its length (4
// bytes) and then it.
//
// C++26 and the standard library, as the rest of blit: the agent is built
// here, with the same compiler, for the machines it goes to (build.sh).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace blit_wire {

// Bumped when a message changes: blit and an agent of another version do
// not talk (an agent is kept per version anyway, see remote.cpp).
constexpr uint32_t kVersion = 2;

enum Op : uint8_t {
    // Asked by blit, answered with kReply and the same number.
    kHello = 1,  // u32 version -> str agent version, str home, str system, u32 process id
    kStat,       // str path -> u8 kind (0 none, 1 file, 2 folder, 3 other), u64 size, i64 modified (ns), u32 mode
    kRead,       // str path, u64 most -> u8 status (0 read, 1 none, 2 not a file, 3 too big, 4 failed), bytes, i64 modified
    kWrite,      // str path, bytes -> u8 ok, str error, i64 modified (through a temporary file, keeping the mode; through a link to what it names)
    kList,       // str path -> u8 ok, u32 n, n * (str name, u8 kind: 1 file, 2 folder)
    kMkdir,      // str path, u8 parents -> u8 ok, str error
    kCreate,     // str path -> u8 ok, str error (a new empty file; fails if there is one)
    kRename,     // str from, str to -> u8 ok, str error
    kTrash,      // str path -> u8 ok, str error (into the freedesktop trash of the agent's user)
    kRealpath,   // str path -> u8 ok, str real path
    kWalk,       // str root, u8 gitignore, u8 hidden, u32 most, u32 rules, * str -> u32 walk, u8 truncated, u32 n, n * str path (from the root)
    kGrep,       // u32 walk, u32 n (0: every file), n * u32 file, str typed, u64 most bytes, u32 most lines
                 //   -> u8 capped, u32 n, n * (u32 file, u32 line, u32 column, str text), u32 files, * u32 file (those with a line)
    kSpawn,      // u32 process, str folder, u32 argc, * str, u32 envc, * str "NAME=value" -> u8 ok, str error
    kStdin,      // u32 process, bytes (none: its input is closed) -> nothing
    kKill,       // u32 process, i32 signal -> nothing
    kWhich,      // str program -> str path (empty: not found on the agent's PATH)
    kSpawnPty,   // u32 process, str folder, u32 rows, u32 columns, u32 argc, * str, u32 envc, * str -> u8 ok, str error
                 //   (on a pseudo-terminal: its output comes as stream 1, its input goes in with kStdin)
    kResize,     // u32 process, u32 rows, u32 columns -> nothing (its pseudo-terminal's size)
    // Sent by the agent.
    kReply = 100,  // the answer to a request
    kOutput,       // u32 process, u8 stream (1 output, 2 errors), bytes
    kExit,         // u32 process, i32 status (exit code, or 128 + signal)
};

// A message being put together.
struct Writer {
    std::string b;
    Writer& u8(uint8_t v) {
        b.push_back(static_cast<char>(v));
        return *this;
    }
    Writer& u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        return *this;
    }
    Writer& u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        return *this;
    }
    Writer& i64(int64_t v) { return u64(static_cast<uint64_t>(v)); }
    Writer& str(const char* p, size_t n) {
        u32(static_cast<uint32_t>(n));
        b.append(p, n);
        return *this;
    }
    Writer& str(const std::string& s) { return str(s.data(), s.size()); }
};

// A message being read. Reading past its end gives zeros and sets ok false.
struct Reader {
    const char* p = nullptr;
    size_t n = 0;
    bool ok = true;
    Reader(const char* data, size_t size) : p(data), n(size) {}
    bool take(size_t k) {
        if (!ok || n < k) return ok = false;
        return true;
    }
    uint8_t u8() {
        if (!take(1)) return 0;
        const uint8_t v = static_cast<uint8_t>(*p);
        ++p, --n;
        return v;
    }
    uint32_t u32() {
        if (!take(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
        p += 4, n -= 4;
        return v;
    }
    uint64_t u64() {
        if (!take(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
        p += 8, n -= 8;
        return v;
    }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    std::string str() {
        const uint32_t k = u32();
        if (!take(k)) return {};
        std::string s(p, k);
        p += k, n -= k;
        return s;
    }
};

// A whole frame: the length, the operation, the number, the rest.
inline std::string frame(uint8_t op, uint32_t id, const std::string& payload) {
    Writer w;
    w.u32(static_cast<uint32_t>(5 + payload.size())).u8(op).u32(id);
    w.b += payload;
    return std::move(w.b);
}

}  // namespace blit_wire
