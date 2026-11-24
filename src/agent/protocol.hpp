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
    kHello = 1,  // u32 version, [u32 blit's level, [str session, u32 idle s, u32 keep s]] -> str agent version, str home,
                 //   str system, u32 process id, [u8 lasting]
                 //   (blit's level, from blit 3: what it understands of what the agent sends, kInFlow; an older
                 //   agent reads no further than the version. From blit 4: the session, blit's own name for itself
                 //   (the same each time it connects again, so that what it left running is its own again), and
                 //   how long the agent waits with nothing to do and nobody attached, and with what was kept
                 //   running and nobody attached, before it ends. From agent 4: lasting, 1 when it outlives this
                 //   connection.)
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
    kSpawn,      // u32 process, str folder, u32 argc, * str, u32 envc, * str "NAME=value", [u8 flags, [str done]] -> u8 ok, str error
                 //   (flags, from agent 2: kQuiet, what it writes as errors goes to /dev/null there; an older agent
                 //   reads no further than the environment, and sends them. From agent 4: kKeep, kWork; and done,
                 //   what it writes when a piece of work kBusy began has ended)
    kStdin,      // u32 process, bytes (none: its input is closed) -> nothing
    kKill,       // u32 process, i32 signal -> nothing
    kWhich,      // str program -> str path (empty: not found on the agent's PATH)
    kSpawnPty,   // u32 process, str folder, u32 rows, u32 columns, u32 argc, * str, u32 envc, * str, [u8 flags] -> u8 ok, str error
                 //   (on a pseudo-terminal: its output comes as stream 1, its input goes in with kStdin; flags as
                 //   kSpawn's, from agent 4)
    kResize,     // u32 process, u32 rows, u32 columns, [u32 width, u32 height] -> nothing (its pseudo-terminal's size;
                 //   in pixels too, from blit 3: an older agent reads no further than the columns)
    kFlow,       // u32 process, u8 paused -> nothing (from agent 2: while paused, its output is not read, so
                 //   that it waits for blit to take what it has; an older agent answers "unknown request")
    kWritePart,  // u32 write, u8 part (kFirst, kLast), str path, bytes -> (the last part only) as kWrite
                 //   (from agent 3: a file written a part at a time, so that what else is sent goes between the
                 //   parts; the parts of one write come in order, and only the last is answered)
    kResume,     // u32 n, n * (u32 process, u64 output had, u64 errors had) -> u32 n, n * (u32 process, u8 there)
                 //   (from agent 4, once kHello has named the session again: the programs kept running while
                 //   blit was away; what each wrote that blit has not had follows, then its end if it has ended.
                 //   One not there is gone: the machine restarted, say)
    kBusy,       // u32 process -> nothing (from agent 4: it has been given a piece of work, which ends when it
                 //   writes its done; until then it counts as working)
    kGoodbye,    // u8 keep -> u8 ok (from agent 4: blit is quitting. What it started ends, but with keep what is
                 //   working; the agent ends at once if nothing else holds it)
    kStop,       // -> nothing (from agent 4: everything it runs, for every blit, ends, and so does it)
    kReadAt,     // str path, u64 offset, u64 length -> u8 status (as kRead's), u64 size (the whole file's), i64 modified, bytes
                 //   (from agent 5: a part of a file, no more than kReadAtMost of it; what is past its end is not
                 //   there. blit fetches a large file as a run of these, about 1 MB each)
    kProxyEnv = 90,  // u32 n, n * str "NAME=value" (from blit-agent --attach, before blit's first: the environment of
                     //   this connection there, the PATH a login has now and SSH_AUTH_SOCK, for the programs started on it)
    // Sent by the agent.
    kReply = 100,  // the answer to a request
    kOutput,       // u32 process, u8 stream (1 output, 2 errors), bytes
    kExit,         // u32 process, i32 status (exit code, or 128 + signal)
    kInFlow,       // u32 process, u8 paused (to a blit of level 3: what it sends to the program's input waits
                   //   while paused, the program not having taken what it has been sent)
    kWorking,      // u32 process, u8 working (to a blit of level 4, for a program kept running: whether it is
                   //   working, as the agent counts it, which is what quitting asks about)
    kPart,         // u8 last, bytes (to a blit of level 5: a reply larger than kPartMost, a piece at a time, with the
                   //   number of its request; put together, the pieces are what kReply would have carried. The
                   //   reverse of kWritePart: what else the agent sends goes between the pieces)
};

// kSpawn's flags. kKeep: it outlives the connection (a terminal, a task, a
// profiling run, the kernel), its output kept meanwhile, up to a few MB.
// kWork: while it runs it counts as working (a task's terminal; a shell's
// counts only while a program is in front of it, and one given kBusy only
// until its done).
enum : uint8_t { kQuiet = 1, kKeep = 2, kWork = 4 };
// kWritePart's parts.
enum : uint8_t { kFirst = 1, kLast = 2 };
// The most of a reply one frame carries (to a blit of level 5): a larger
// one goes in kPart frames of this much, so that no frame the agent sends
// is larger than a few MB (and what else it sends waits behind a piece of a
// large answer, not all of it). And the most one kReadAt reads.
constexpr size_t kPartMost = 1u << 20;
constexpr uint64_t kReadAtMost = 16u << 20;

// The agent's own version, the number at the end of what it says to kHello
// ("blit-agent 3"). It counts what an agent can do that one before it
// could not (2: kFlow, kQuiet; 3: kWritePart, kInFlow; 4: it outlives the
// connection, kResume, kKeep, kBusy, kGoodbye, kStop, kWorking; 5: kReadAt,
// and large replies in kPart frames; 6: the pumps, `blit-agent --serial` and
// `--connect`, which blit starts as programs with kSpawn: pump.hpp), so that
// blit asks an older one only for what it knows. (A blit always brings its own agent; an older one is
// met only when the agents kept in agent/bin were built from older
// sources.) blit says its own level the same way, in kHello (5: it puts
// kPart frames together).
constexpr uint32_t kAgentLevel = 6;
constexpr uint32_t kBlitLevel = 5;

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
