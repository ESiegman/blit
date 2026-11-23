#pragma once
// Waveforms: a simulation's dump read into lanes (timeline.hpp), with the
// scopes and variables it declares. VCD is read here, as IEEE 1364-2005
// section 18 has it, and as the tools write it (Verilator, Icarus, Yosys,
// pyvcd), with GTKWave's string values; FST through libfst, in wavefst.cpp.
// Like timeline.cpp, this knows no window and nothing of the app, so that
// the agent can carry it.
//
// A dump is read in one pass, eagerly, every variable's changes into its
// lane as the file goes by; fed in pieces split anywhere, so that a file
// can come from another machine a part at a time, or be followed as it
// grows; under a memory budget (the setting timeline.max_mb), at which it
// stops and says so; on a thread of its own (WaveLoad), with progress and
// a way to stop it.
#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "timeline.hpp"

namespace blit {

enum class ScopeKind : u8 {
    kModule, kTask, kFunction, kBegin, kFork,
    // SystemVerilog's and the simulators' own:
    kGenerate, kStruct, kUnion, kClass, kInterface, kPackage, kProgram,
    kOther,
};

enum class VarKind : u8 {
    kWire, kReg, kLogic, kBit, kInteger, kInt, kShortInt, kLongInt, kByte, kTime, kParameter,
    kReal, kRealTime, kShortReal, kString, kEvent, kEnum, kPort,
    kSupply0, kSupply1, kTri, kTriAnd, kTriOr, kTriReg, kTri0, kTri1, kWAnd, kWOr, kUWire,
    kOther,
};

// As the dump says them ("module", "reg"); kOther as "other".
std::string_view scope_kind_name(ScopeKind k);
std::string_view var_kind_name(VarKind k);

struct WaveScope {
    std::string name;
    ScopeKind kind = ScopeKind::kModule;
    u32 parent = 0;            // (the root's is itself)
    std::vector<u32> scopes;   // the scopes inside it, in the order declared
    std::vector<u32> vars;     // and its variables
};

struct WaveVar {
    std::string name;      // as declared, without its range: "count"
    std::string code;      // the dump's identifier code (an FST's handle, in decimal); variables with the same one share a lane
    u32 scope = 0;
    u32 lane = 0;
    VarKind kind = VarKind::kWire;
    u32 width = 1;         // as declared
    bool ranged = false;   // declared with a range or a bit: [7:0] (msb 7, lsb 0), [3] (both 3)
    i32 msb = 0, lsb = 0;
};

struct Wave {
    std::vector<WaveScope> scopes{1};  // [0] is the root, unnamed: the dump's top scopes are in it
    std::vector<WaveVar> vars;
    std::vector<Lane> lanes;
    // A unit of the dump's time is unit_mult * 10^unit_exp seconds (1 ps:
    // 1 and -12; 10 ns: 10 and -9). unit_given: the dump said so (else 1 ns).
    u32 unit_mult = 1;
    i32 unit_exp = -9;
    bool unit_given = false;
    u64 start = 0, end = 0;  // the first and last time the dump reaches
    // The stretches between a $dumpoff and its $dumpon (~0: none came).
    std::vector<std::pair<u64, u64>> off;
    std::string date, version;
    // How far it is read. Once `header` is set the scopes and variables are
    // all there and stay as they are; while it reads, the lanes have every
    // change before `reached` (and maybe some at it); `done` once it is read
    // to the end, or stopped (`error` says why).
    bool header = false;
    bool done = false;
    u64 reached = 0;
    std::string error;
    // What was wrong but did not stop it, said once each ("line 12: ...").
    std::vector<std::string> notes;
    u64 changes = 0;  // in all the lanes (once done)
    // A load (WaveLoad) holds this exclusively while it adds a piece of the
    // file (at most 1 MB of it, a few milliseconds); hold it shared around
    // any reading while one may be running.
    mutable std::shared_mutex mu;

    // "tb.u_cnt.count": the scopes' names and the variable's, with dots.
    std::string path(u32 var) const;
    // The variable at such a path, or -1.
    i64 find(std::string_view path) const;
    // A time as a person reads it, in the unit that suits it: "12.5 ns".
    std::string time_text(u64 t) const;
    // The memory the lanes and the names hold, in bytes.
    size_t bytes() const;
};

// The reader: fed a dump's bytes in pieces split anywhere, it fills a Wave
// (empty at the start). It takes the Wave's lock for each piece of at most
// 1 MB; a thread of the caller's own (WaveLoad's) is where it belongs.
class VcdReader {
public:
    // max_bytes: the memory the Wave may take (0: no limit). Past it the
    // reader stops, and error says how far it got.
    explicit VcdReader(Wave* into, u64 max_bytes = 0);
    ~VcdReader();
    VcdReader(const VcdReader&) = delete;
    VcdReader& operator=(const VcdReader&) = delete;

    // The next bytes of the dump. False once it has stopped (the budget, or
    // finish()); what was read until then stays.
    bool feed(std::string_view bytes);
    // The end of the dump: a last token with no newline after it is read,
    // the lanes give back what they took on beyond their size, and the
    // Wave is done.
    void finish();
    u64 memory() const { return mem_; }

private:
    struct Ids;

    void run(std::string_view in, bool last);
    void stop(std::string why);
    void token(std::string_view t);
    void header_token(std::string_view t);
    void declaration();
    void body_token(std::string_view t);
    void value(char kind, std::string_view code);
    void keep_pending();
    void over();
    void scalar(char c, u32 lane);
    void vector(std::string_view v, u32 lane);
    void real(std::string_view v, u32 lane);
    void dumpoff(bool on);
    void note(std::string s);
    i64 lane_of(std::string_view code) const;

    Wave* w_;
    u64 max_ = 0, mem_ = 0;
    std::unique_ptr<Ids> ids_;
    std::string carry_;     // a token cut by the end of the last piece
    u64 line_ = 1;          // (for notes: the line the token being read is on)
    // In the definitions: the keyword being read and its words so far.
    std::string kw_;
    std::vector<std::string> args_;
    bool body_ = false;
    bool skip_ = false;     // in the body, in a $comment (or another block): until $end
    char want_ = 0;         // a 'b', 'r' or 's' value read: its code is the next token
    std::string_view pend_; // (the value: in the piece being read, or in pending_ if owned_)
    bool owned_ = false;
    std::string pending_;
    u32 cur_scope_ = 0;
    u64 now_ = 0;
    bool timed_ = false;    // a #time has been read
    bool stopped_ = false;
    u32 notes_dropped_ = 0;
    std::vector<u64> val_, xz_;  // a vector value being put together
};

// What a dump is, from its first bytes (9 are enough): FST if they are an
// FST header block (or the gzip wrapper some writers put around a whole
// file), else VCD's text.
enum class WaveFormat : u8 { kVcd, kFst };
WaveFormat wave_format(std::string_view head);

// An FST file read into a Wave (empty at the start) through libfst
// (third_party/libfst), on the caller's thread, which it keeps until the
// file is read: the header and the hierarchy (variables with their kinds,
// widths and ranges, aliases sharing a lane, $dumpoff stretches), then
// every change, eagerly, into the lanes. libfst reads a file by its path,
// so a remote one is fetched whole first (kReadAt parts into a local file)
// until the agent reads it where it is.
//
// As VcdReader does, it holds the Wave's lock while it sets the hierarchy
// up, then for each piece of changes (about 1 MB of them, buffered while
// libfst uncompresses the next, so the lock is never held through that);
// `reached` says how far the lanes go. It stops at max_bytes (0: no limit),
// or when `stop` is asked, keeping what it read; a file that is not FST,
// or is cut short, is said in `error`. `progress` is
// told after each piece about how many bytes into the file it has got.
// Returns the memory the Wave took, as the budget counts it.
u64 read_fst(Wave* into, const std::string& path, u64 max_bytes = 0, std::stop_token stop = {},
             const std::function<void(u64 bytes)>& progress = {});

// A dump read on a thread of its own. The bytes come through read_at,
// called on that thread: a local file's (local_file) or, for a remote
// one, kReadAt parts.
class WaveLoad {
public:
    // Reads at most len bytes at offset; returns how many (0: the end), or -1 on an error.
    using ReadAt = std::function<i64(u64 offset, char* buf, size_t len)>;

    // size: the file's (for progress; 0 if not known). notify is called on
    // the load's thread once the definitions are read, then at most every
    // tenth of a second while it reads, and at the end.
    WaveLoad(std::shared_ptr<Wave> into, ReadAt read_at, u64 size, u64 max_bytes, std::function<void()> notify = {});
    // An FST file, by its path (read_fst), told about in the same way;
    // read() is an estimate, from where libfst has got to.
    WaveLoad(std::shared_ptr<Wave> into, std::string fst_path, u64 max_bytes, std::function<void()> notify = {});
    ~WaveLoad();  // stops it, and waits for it
    WaveLoad(const WaveLoad&) = delete;
    WaveLoad& operator=(const WaveLoad&) = delete;

    // A local file's ReadAt, and its size; false (and why) if it cannot be opened.
    static bool local_file(const std::string& path, ReadAt* out, u64* size, std::string* error);

    const std::shared_ptr<Wave>& wave() const { return wave_; }
    u64 read() const { return read_.load(std::memory_order_relaxed); }  // bytes so far
    u64 size() const { return size_; }
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    void stop() { thread_.request_stop(); }
    void wait();  // (for tests and tools)

private:
    void run(std::stop_token stop, ReadAt read_at, u64 max_bytes);
    void run_fst(std::stop_token stop, const std::string& path, u64 max_bytes);

    std::shared_ptr<Wave> wave_;
    u64 size_ = 0;
    std::function<void()> notify_;
    std::atomic<u64> read_{0};
    std::atomic<bool> finished_{false};
    std::jthread thread_;  // (last: it stops before what it uses goes)
};

}  // namespace blit
