// Waveforms in a tab of their own: a dump (.vcd, .fst, or an ILA capture's
// .vcd or .csv, whose triggers are drawn as markers) opened from Go to file,
// the tree or a link is read on a thread of its own (WaveLoad, wave.hpp), and
// what has arrived shows while the rest comes. Names, values at the cursor,
// the ruler and the bottom row are cells; the lanes are tiles painted on
// workers (wavetile.hpp) and placed as pictures. A tile painted at another
// zoom stands in, stretched, until its sharp one comes; panning moves tiles
// and asks for the next ones ahead. The cursor and markers are one-pixel
// pictures stretched, so moving them paints nothing. Nothing is drawn while
// nothing changes, and no timer is added.
//
// Its keys are the timeline's (evaluation 6.13), each a command
// (wave_commands, listed by appcommands.cpp), so that Command search finds
// them. The view (rows, groups, radix, names of values, markers, cursor and
// the stretch on show) is kept beside the project in .blit/waves/, data and
// not Lua, and comes back when the dump is opened again.
//
// A dump on another machine is fetched whole while it is no larger than a
// reply may be (4 MB); larger ones wait for the agent's reads in parts
// (kReadAt), which plug in at remote_read_at below.
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "wavetile.hpp"
#include "waveview.hpp"

namespace blit {

namespace {

constexpr u32 kWaveEvent = 60;                 // (the SDL user event's code: a dump read further, a tile painted, a view read)
constexpr size_t kTileBudget = 128ull << 20;   // the tiles kept on the GPU, at most (the least lately drawn let go first)
constexpr double kMostZoom = 1.0 / 64;         // time a device pixel, at the closest: 64 pixels a unit
constexpr size_t kUndo = 100;                  // changes to the rows that u can undo
constexpr u32 kRulerRows = 2, kBottomRows = 1;

constexpr Ink kMarkInk{kInkDiagnostic + 3, 0};  // markers and their deltas (the hints' ink: apart from the cursor's accent)
constexpr Ink kTriggerInk{kInkDiagnostic, 0};    // an ILA capture's triggers (the errors' ink, as Vivado draws them red)

std::string g_wave_clipboard;  // (scripted runs: what y copied, here and not in the system's clipboard)

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = kWaveEvent;
    SDL_PushEvent(&ev);
}

std::string leaf_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string size_text(u64 bytes) {
    char b[32];
    if (bytes < 1024 * 1024) std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(bytes) / 1024);
    else std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024 * 1024));
    return b;
}

u64 mix(u64 h, u64 v) { return (h ^ v) * 1099511628211ull; }
u64 mix_text(u64 h, std::string_view s) {
    for (const char c : s) h = mix(h, static_cast<u8>(c));
    return mix(h, s.size());
}

// What a row's tile paints, in a number: equal rows, equal pixels.
u64 row_key(u64 h, const WaveTileRow& r) {
    h = mix(h, r.lane);
    h = mix(h, static_cast<u64>(r.fmt.radix) | static_cast<u64>(static_cast<u32>(r.fmt.bit)) << 8 | static_cast<u64>(r.fmt.frac) << 40 | static_cast<u64>(r.fmt.sign) << 48);
    if (r.fmt.names) h = mix_text(h, r.fmt.names->text());
    return h;
}

}  // namespace

// The actions of the tab's keys and commands.
enum WaveAct : u8 {
    kWavePanLeft, kWavePanRight, kWavePageLeft, kWavePageRight, kWaveZoomIn, kWaveZoomOut, kWaveFit,
    kWaveDown, kWaveUp, kWaveMoveDown, kWaveMoveUp, kWaveNext, kWavePrev, kWaveAdd, kWaveMark, kWaveGoMark,
    kWaveRange, kWaveZoomRange, kWaveRadix, kWaveOpen, kWaveRows, kWaveGroup, kWaveRemove, kWaveUndo,
    kWaveCopyPath, kWaveCopyValue, kWaveNames, kWaveFixed, kWaveRename,
};

namespace {

// The commands, their keys (none: a key and a second, as Vim's, below) and what they do.
struct WaveKey {
    struct {
        const char* title;
        SDL_Keycode key;
        bool shift;
    } cmd;
    WaveAct act;
};
constexpr WaveKey kWaveKeys[] = {
    {{"Waves: pan left", SDLK_H, false}, kWavePanLeft},
    {{"Waves: pan right", SDLK_L, false}, kWavePanRight},
    {{"Waves: pan left a screen", SDLK_H, true}, kWavePageLeft},
    {{"Waves: pan right a screen", SDLK_L, true}, kWavePageRight},
    {{"Waves: zoom in about the cursor", SDLK_EQUALS, false}, kWaveZoomIn},
    {{"Waves: zoom out about the cursor", SDLK_MINUS, false}, kWaveZoomOut},
    {{"Waves: show the whole run", SDLK_0, false}, kWaveFit},
    {{"Waves: next row", SDLK_J, false}, kWaveDown},
    {{"Waves: previous row", SDLK_K, false}, kWaveUp},
    {{"Waves: move the row down", SDLK_J, true}, kWaveMoveDown},
    {{"Waves: move the row up", SDLK_K, true}, kWaveMoveUp},
    {{"Waves: next change of the row", SDLK_N, false}, kWaveNext},
    {{"Waves: previous change of the row", SDLK_N, true}, kWavePrev},
    {{"Waves: add a signal\xE2\x80\xA6", SDLK_SLASH, false}, kWaveAdd},
    {{"Waves: set a marker at the cursor\xE2\x80\xA6", 0, false}, kWaveMark},      // m{a-z}
    {{"Waves: go to a marker\xE2\x80\xA6", 0, false}, kWaveGoMark},                // '{a-z}
    {{"Waves: choose a stretch of time from the cursor", SDLK_V, false}, kWaveRange},
    {{"Waves: zoom to the stretch chosen, or from the cursor to the last marker", SDLK_Z, false}, kWaveZoomRange},
    {{"Waves: the row's next radix", SDLK_R, false}, kWaveRadix},
    {{"Waves: open or close the bus or the group", SDLK_RETURN, false}, kWaveOpen},
    {{"Waves: choose rows", SDLK_V, true}, kWaveRows},
    {{"Waves: group the rows chosen", 0, false}, kWaveGroup},                     // gG
    {{"Waves: remove the row", 0, false}, kWaveRemove},                           // dd
    {{"Waves: undo the last change to the rows", SDLK_U, false}, kWaveUndo},
    {{"Waves: copy the signal's path", 0, false}, kWaveCopyPath},                 // yy
    {{"Waves: copy the value at the cursor", 0, false}, kWaveCopyValue},          // yv
    {{"Waves: name the row's values\xE2\x80\xA6", 0, false}, kWaveNames},
    {{"Waves: the row in fixed point\xE2\x80\xA6", 0, false}, kWaveFixed},
    {{"Waves: rename the group\xE2\x80\xA6", 0, false}, kWaveRename},
};

// One-pixel pictures of a colour, stretched for the cursor, markers, ticks and what is chosen.
u32 dot_of(Renderer& gpu, std::map<u32, u32>& dots, const float* rgb) {
    u8 px[4];
    for (int c = 0; c < 3; ++c) px[c] = static_cast<u8>(std::lround(std::clamp(rgb[c], 0.0f, 1.0f) * 255));
    px[3] = 255;
    const u32 key = static_cast<u32>(px[0]) << 16 | static_cast<u32>(px[1]) << 8 | px[2];
    auto it = dots.find(key);
    if (it == dots.end()) it = dots.emplace(key, gpu.image_add(px, 1, 1)).first;
    return it->second;
}

}  // namespace

std::span<const App::WaveCommand> App::wave_commands() {
    static const std::vector<WaveCommand> all = [] {
        std::vector<WaveCommand> out;
        for (const WaveKey& k : kWaveKeys) out.push_back({k.cmd.title, k.cmd.key, k.cmd.shift});
        return out;
    }();
    return all;
}

// A dump's tab.
struct WaveView {
    u64 doc = 0;
    std::string path, view_path;
    Renderer* gpu = nullptr;
    std::shared_ptr<Wave> wave = std::make_shared<Wave>();
    std::unique_ptr<WaveLoad> load;
    std::shared_ptr<std::string> fetch_error = std::make_shared<std::string>();  // (another machine's dump: why it could not be had)
    std::stop_source fetch_stop;
    std::string open_error;  // (it could not be opened at all)
    // How far it is read (as last seen: the load has the wave's lock now and then).
    struct Progress {
        bool header = false, done = false, settled = false, writing = false;
        u64 reached = 0, start = 0, end = 0, changes = 0, bytes = 0;
        std::string error;
        size_t notes = 0;
    } now;
    // The file changing: which it is (here: its time stamp and its inode;
    // there: its time stamp, asked there), and that it has changed and is
    // to be looked at once the tab is on show.
    i64 stamp = 0;
    u64 dev = 0, ino = 0;
    bool changed = false;
    std::string heard_in;  // (the folder whose watch hears it as it is written)
    // Read again (a rerun): the new reading, put in place of this one once
    // it has its header (until then this one stays on show), or why it
    // could not be read (this one staying).
    struct Again {
        std::shared_ptr<Wave> wave;
        std::unique_ptr<WaveLoad> load;
        std::shared_ptr<std::string> fetch_error = std::make_shared<std::string>();
        std::stop_source fetch_stop;
        ~Again() {
            fetch_stop.request_stop();
            load.reset();
        }
    };
    std::unique_ptr<Again> again;
    std::string again_note;  // (what came of the last reading again, if it could not be put in place)
    u32 agains = 0;          // (times read again, for the tests)
    // The kept view: asked for, and come (found or not).
    bool view_come = false;
    std::string view_text;
    bool started = false;  // the rows made (from the kept view, or the dump's scopes)
    u32 dropped = 0;       // (rows of the kept view the dump has no signal for)
    // The rows.
    std::vector<WaveItem> items;
    std::map<std::string, std::shared_ptr<const WaveEnum>> enums;  // by path
    std::vector<WaveRow> rows;
    std::deque<std::vector<WaveItem>> undo;
    u32 sel = 0, top = 0;
    i64 vrow = -1;  // V: rows chosen, from this one to sel
    // Time: the cursor, markers, a stretch being chosen (v), and the view:
    // the time at the traces' left edge and a device pixel's.
    u64 cursor = 0;
    u64 marks[26] = {};
    u32 marked = 0;
    int last_mark = -1;
    bool ranging = false;
    u64 range_from = 0;
    bool fit = true;  // the whole run (and following it, as it is read)
    double left = 0, tpp = 1;
    double want_from = 0, want_to = 0;  // (the kept view's stretch, set once the traces' width is known)
    char pending = 0;  // m ' d g y: a key waiting for its second
    bool to_trigger = false;  // (an ILA capture's cursor: to its first trigger, once it is read)
    bool touched = false;  // changed by hand: kept from then on
    std::string written;   // the view as last kept
    u64 gen = 1;           // counts up with every change to what is shown
    // As laid out: the tab, its cells, and where the traces and rows are.
    int x = 0, y = 0, w = 0, h = 0, pad_x = 0, pad_y = 0;
    int tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0, ry0 = 0;  // traces [tx0, tx1), rows from ty0 to ty1, the ruler from ry0
    u32 cols = 0, lane_rows = 0, name_cols = 0, value_cols = 0, trace_col = 0;
    TextBox box;
    u64 key = 0;
    // Dragging: the cursor across the traces, or a stretch on the ruler.
    int drag = 0;  // 1 the cursor, 2 the ruler
    u64 drag_from = 0;
    // The tiles: by key (rows, zoom, place, looks).
    struct Tile {
        u64 rows_key = 0;
        double tpp = 1, ta = 0, tb = 0;  // its zoom, and the times at its left and right edges
        i64 k = 0;
        u64 t1 = 0, upto = ~0ull;
        u32 image = 0;
        int w = 0, h = 0;
        u64 used = 0;
    };
    std::unordered_map<u64, Tile> tiles;
    size_t tile_bytes = 0;
    u64 frames = 0;
    // For the tests: tiles painted, and in the last frame drawn sharp, stretched, still to come.
    u64 painted = 0, paint_ns = 0;
    u32 sharp = 0, stretched = 0, missing = 0;
    WaveTileStats last_stats;

    void drop(Tile& t) {
        if (t.image && gpu) gpu->image_drop(t.image);
        tile_bytes -= std::min<size_t>(tile_bytes, static_cast<size_t>(t.w) * static_cast<size_t>(t.h) * 4);
        t.image = 0;
    }
    ~WaveView() {
        for (auto& [k, t] : tiles) drop(t);
        fetch_stop.request_stop();
        again.reset();
        load.reset();  // (stops the reading and waits for it)
    }
    int trace_w() const { return std::max(1, tx1 - tx0); }
    double span_start() const { return static_cast<double>(now.start); }
    double span_end() const { return static_cast<double>(std::max(now.end, now.start + 1)); }
    double x_of(double t) const { return tx0 + (t - left) / tpp; }
    u64 time_at(double px) const { return static_cast<u64>(std::max(0.0, std::round(left + (px - tx0) * tpp))); }
    void fit_now() {
        left = span_start();
        tpp = std::max(kMostZoom, (span_end() - span_start()) / trace_w());
    }
    void clamp() {
        tpp = std::clamp(tpp, kMostZoom, std::max(kMostZoom, (span_end() - span_start()) * 4 / trace_w()));
        const double view = trace_w() * tpp;
        left = std::clamp(left, std::min(span_start(), span_end() - view), std::max(span_start(), span_end() - view / 2));
        left = std::max(0.0, left);
    }
    // Zoomed by `by` (above 1: in) about time t's place on the screen.
    void zoom(double by, double about) {
        const double px = (about - left) / tpp;
        tpp /= by;
        fit = false;
        clamp();
        left = about - px * tpp;
        clamp();
        ++gen;
    }
    void show_time(u64 t) {  // the view moved (by a screen's half) if t is not on it
        const double view = trace_w() * tpp;
        if (static_cast<double>(t) < left || static_cast<double>(t) > left + view) {
            left = static_cast<double>(t) - view / 2;
            fit = false;
            clamp();
        }
    }
    void zoom_to(double a, double b) {
        if (b <= a) return;
        const double margin = (b - a) * 0.05;
        tpp = (b - a + 2 * margin) / trace_w();
        left = a - margin;
        fit = false;
        clamp();
        ++gen;
    }
    void remember() {
        undo.push_back(items);
        if (undo.size() > kUndo) undo.pop_front();
    }
    void show_sel(u32 room) {
        if (rows.empty()) return void(sel = top = 0);
        sel = std::min<u32>(sel, static_cast<u32>(rows.size()) - 1);
        if (sel < top) top = sel;
        if (room && sel >= top + room) top = sel - room + 1;
    }
};

struct App::Waves {
    std::unique_ptr<WavePainter> painter;
    std::shared_ptr<const WaveLooks> looks;
    std::map<u32, u32> dots;
    // The views kept beside the projects: read and written on a thread of
    // their own (another machine's over its connection); and the time
    // stamps of dumps on another machine, asked there.
    struct Op {
        u64 doc = 0;
        std::string path, text;
        bool write = false, stamp = false;
    };
    struct Read {
        u64 doc = 0;
        std::string text;
        bool stamp = false;
        i64 when = 0;
    };
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<Op> ops;
    std::vector<Read> reads;
    bool io_busy = false;
    std::jthread io;  // (last: it stops first)

    Waves() {
        io = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~Waves() {
        io.request_stop();
        cv.notify_all();
        if (io.joinable()) io.join();
    }
    void post(Op op) {
        {
            std::lock_guard l(mu);
            if (op.write) std::erase_if(ops, [&](const Op& o) { return o.write && o.path == op.path; });  // (only the newest of a view is written)
            ops.push_back(std::move(op));
        }
        cv.notify_all();
    }
    void run(std::stop_token st) {
        for (;;) {
            Op op;
            {
                std::unique_lock l(mu);
                // (Asked to stop, it still writes what it was given.)
                cv.wait(l, st, [&] { return !ops.empty(); });
                if (ops.empty()) return;
                op = std::move(ops.front());
                ops.pop_front();
                io_busy = true;
            }
            Read r;
            r.doc = op.doc;
            r.stamp = op.stamp;
            if (op.write) {
                std::string err;
                fs_mkdirs(parent_of(op.path), &err);
                fs_write(op.path, op.text, &err);
            } else if (op.stamp) {
                r.when = fs_remote_stamp(op.path);
            } else {
                fs_read(op.path, &r.text, 1 << 20);
            }
            {
                std::lock_guard l(mu);
                io_busy = false;
                if (!op.write) reads.push_back(std::move(r));
            }
            if (!op.write) wake();
        }
    }
};

bool App::is_wave_path(std::string_view path) {
    if (path.size() < 4) return false;
    std::string ext(path.substr(path.size() - 4));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".vcd" || ext == ".fst") return true;
    // An ILA capture's CSV, by its first line (here only: another machine's is not asked for it on the way).
    if (ext != ".csv" || is_remote(path)) return false;
    char head[16];
    FILE* f = std::fopen(std::string(path).c_str(), "rb");
    if (!f) return false;
    const size_t n = std::fread(head, 1, sizeof head, f);
    std::fclose(f);
    return dump_kind(std::string_view(head, n)) == DumpKind::kIlaCsv;
}

namespace {

// Another machine's dump, as the load reads it. Fetched whole while it is
// no larger than one reply may be; the agent's reads in parts (kReadAt,
// with progress) plug in here for larger ones.
// Another machine's dump: fetched whole on the load's first read, in
// parts of about 1 MB (fs_read_range: other traffic between them, its
// progress on the bottom row, where a click stops it), up to most bytes.
WaveLoad::ReadAt remote_read_at(const std::string& path, std::stop_token stop, std::shared_ptr<std::string> why, u64 most) {
    auto bytes = std::make_shared<std::string>();
    auto fetched = std::make_shared<bool>(false);
    return [path, stop, why, bytes, fetched, most](u64 offset, char* buf, size_t len) -> i64 {
        if (!*fetched) {
            *fetched = true;
            std::string err;
            if (!fs_read_range(path, 0, ~0ull, most, bytes.get(), &err, stop)) {
                *why = err.starts_with("the file is too big") ? "waves larger than " + std::to_string(most >> 20) + " MB on another machine are not shown (wave.remote_fetch_mb)" : err;
                errno = EIO;
                return -1;
            }
        }
        if (offset >= bytes->size()) return 0;
        const size_t n = std::min<size_t>(len, bytes->size() - offset);
        std::memcpy(buf, bytes->data() + offset, n);
        return static_cast<i64>(n);
    };
}

}  // namespace

namespace {

// Another machine's FST, which libfst reads only by its path: fetched in
// parts (as remote_read_at's VCD is, its progress on the bottom row) into
// a file in blit's cache folder here, which the load removes once read.
WaveLoad::Fetch remote_fst(const std::string& path, u64 most) {
    return [path, most](std::stop_token stop, std::string* out, std::string* why) {
        std::string bytes, err;
        if (!fs_read_range(path, 0, ~0ull, most, &bytes, &err, stop)) {
            *why = err.starts_with("the file is too big") ? "waves larger than " + std::to_string(most >> 20) + " MB on another machine are not shown (wave.remote_fetch_mb)" : err;
            return false;
        }
        static std::atomic<u32> seq{0};
        const std::string base = cache_dir();
        const std::string dir = (base.empty() ? std::string("/tmp") : base) + "/waves";
        std::string made;
        if (!fs_mkdirs(dir, &made)) return *why = "cannot make " + dir + ": " + made, false;
        *out = dir + "/" + std::to_string(::getpid()) + "-" + std::to_string(seq++) + ".fst";
        FILE* f = std::fopen(out->c_str(), "wb");
        const bool wrote = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        if (f) std::fclose(f);
        if (!wrote) return std::remove(out->c_str()), *why = "cannot write " + *out, false;
        return true;
    };
}

bool is_fst(const std::string& path) { return path.size() >= 4 && (path.ends_with(".fst") || path.ends_with(".FST")); }

// The reading of a dump into `into`: another machine's fetched (an FST
// whole, into a file here; a VCD in parts), an FST here by its path (libfst
// reads it so), a VCD here from the file, followed as it is written while
// the tab is open (`follow`). Null, and why, if it cannot be opened.
std::unique_ptr<WaveLoad> load_of(const std::string& full, const std::shared_ptr<Wave>& into, u64 most, u64 fetch_most, std::stop_token fetch_stop,
                                  std::shared_ptr<std::string> fetch_error, WaveLoad::Follow follow, std::string* why) {
    if (is_remote(full) && is_fst(full)) return std::make_unique<WaveLoad>(into, remote_fst(full, fetch_most), most, wake);
    if (is_remote(full)) return std::make_unique<WaveLoad>(into, remote_read_at(full, fetch_stop, std::move(fetch_error), fetch_most), 0, most, wake);
    if (is_fst(full)) {
        if (fs_kind(full) == 1) return std::make_unique<WaveLoad>(into, full, most, wake);
        *why = fs_kind(full) == 0 ? "No such file or directory" : "not a file";
        return nullptr;
    }
    WaveLoad::ReadAt read_at;
    u64 size = 0;
    if (!WaveLoad::local_file(full, &read_at, &size, why)) return nullptr;
    // (An empty one is a simulation's that has not written its header yet: waited for too.)
    if (size == 0 && follow == WaveLoad::Follow::kAfterHeader) follow = WaveLoad::Follow::kAlways;
    return std::make_unique<WaveLoad>(into, std::move(read_at), size, most, wake, follow);
}

// Which file a path is here now (its time stamp, device and inode); false if there is none.
bool file_id(const std::string& path, i64* stamp, u64* dev, u64* ino) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
#if defined(__APPLE__)
    *stamp = static_cast<i64>(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
    *stamp = static_cast<i64>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
    *dev = static_cast<u64>(st.st_dev), *ino = static_cast<u64>(st.st_ino);
    return true;
}

}  // namespace

// A dump in a tab of its own (open_path() sends it here).
bool App::wave_open(const std::string& full) {
    if (!waves_) waves_ = std::make_shared<Waves>();
    auto fresh = std::make_shared<Doc>();
    fresh->ed.path = full;
    fresh->read_only = true;
    fresh->id = next_doc_id_++;
    auto v = std::make_shared<WaveView>();
    fresh->wave = v;
    v->doc = fresh->id;
    v->path = full;
    v->gpu = &gpu_;
    // Kept beside the project (the folder worked in, if the dump is in it), else beside the dump.
    const std::string* root = panel_root();
    const bool inside = root && full.size() > root->size() && full.starts_with(*root) && full[root->size()] == '/';
    v->view_path = wave_view_path(inside ? *root : parent_of(full), full);
    waves_->post({fresh->id, v->view_path, {}, false});
    // (Which file it is, to know when it changes: here, from the file; there, asked there.)
    if (is_remote(full)) waves_->post({fresh->id, full, {}, false, true});
    else file_id(full, &v->stamp, &v->dev, &v->ino);
    v->load = load_of(full, v->wave, static_cast<u64>(cfg_.timeline_max_mb) << 20, static_cast<u64>(cfg_.wave_remote_fetch_mb) << 20, v->fetch_stop.get_token(), v->fetch_error,
                      WaveLoad::Follow::kAfterHeader, &v->open_error);
    if (!is_remote(full)) {
        recent_add(full, false);
        // (The folder worked in watched, whether the tree has been read or
        // not: a rerun's dump is read again, or one still being written
        // read on, as it changes; and the dump heard as it is written, not
        // only once it is closed.)
        watch_follow();
        if (watch_) watch_->file(full);
    }
    const bool reuse = pane_->doc->ed.path.empty() && !pane_->doc->ed.modified() && pane_->doc->ed.buf.size() == 0 && !pane_->doc->read_only;
    size_t at = pane_->tab + 1;
    if (reuse) {
        stop_lsp();
        forget_doc(pane_->doc);  // (the empty file it takes the place of: nothing may still point at it)
        at = pane_->tab;
        pane_->docs[pane_->tab] = std::move(fresh);
        pane_->doc = pane_->docs[at].get();
        pane_->tab = at;
        pane_->scroll_x = pane_->scroll_y = pane_->target_x = pane_->target_y = 0;
        pane_->cache_rev = ~0ull;
    } else {
        pane_->docs.insert(pane_->docs.begin() + static_cast<std::ptrdiff_t>(at), std::move(fresh));
    }
    tab_show(at);
    pick_theme();
    dirty_ = true;
    return true;
}

// A dump in a wave tab. One open already is read again (`again`: a run
// that wrote it has ended; once it is on show), and brought to the front
// of its split with the keyboard (`front`). Else it is opened in a split
// that is not showing a terminal (a run's terminal stays on show beside
// it), and the keyboard stays where it was unless `front`.
void App::wave_show(const std::string& full, bool front, bool again) {
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i) {
            WaveView* v = p->docs[i]->wave.get();
            if (!v || v->path != full) continue;
            if (again) v->changed = true, ++v->gen;
            if (front) {
                focus_pane(p.get());
                tab_show(i);
            }
            dirty_ = true;
            return;
        }
    Pane* const keys = pane_;
    Pane* into = pane_->doc && pane_->doc->term ? nullptr : pane_;
    for (size_t i = 0; !into && i < panes_.size(); ++i)
        if (panes_[i]->doc && !panes_[i]->doc->term) into = panes_[i].get();
    focus_pane(into ? into : pane_);
    wave_open(full);
    if (front) return;
    bool still = false;
    for (const std::unique_ptr<Pane>& p : panes_) still |= p.get() == keys;
    if (still) focus_pane(keys);
}

namespace {

// How far the dump is read, as the load has it now; false if the load has
// the lock (a moment: it is taken again at its next news).
bool peek(WaveView& v) {
    std::shared_lock lock(v.wave->mu, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const Wave& w = *v.wave;
    WaveView::Progress& p = v.now;
    const WaveView::Progress was = p;
    p.header = w.header, p.done = w.done, p.settled = w.settled, p.writing = w.writing, p.reached = w.reached, p.start = w.start, p.end = w.end, p.changes = w.changes;
    p.error = w.error, p.notes = w.notes.size();
    p.bytes = v.load ? v.load->read() : 0;
    if (p.header != was.header || p.done != was.done || p.settled != was.settled || p.reached != was.reached || p.end != was.end || p.error != was.error ||
        p.bytes != was.bytes)
        ++v.gen;
    return true;
}

// The rows made, once the dump's header and the kept view (if there is one) are here.
void start(WaveView& v) {
    // (Not a dump that could not be had: the reader, given nothing, says it
    // has read a header of nothing; nor an FST not yet closed, whose rows
    // come once it has been.)
    if (v.started || !v.now.header || !v.view_come || !v.open_error.empty() || !v.fetch_error->empty() || v.now.writing) return;
    std::shared_lock lock(v.wave->mu, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const Wave& w = *v.wave;
    if (w.done && w.vars.empty() && !w.error.empty()) return;  // (nothing in it to show: the bottom row says why)
    WaveSaved saved;
    if (!v.view_text.empty() && wave_view_parse(w, v.view_text, &saved, &v.dropped) && (!saved.items.empty() || v.dropped == 0)) {
        v.items = std::move(saved.items);
        v.enums = std::move(saved.enums);
        std::copy(std::begin(saved.marks), std::end(saved.marks), v.marks);
        v.marked = saved.marked;
        if (saved.has_cursor) v.cursor = saved.cursor;
        if (saved.to > saved.from) v.want_from = saved.from, v.want_to = saved.to, v.fit = false;
        v.written = v.view_text;
    } else {
        v.items = wave_default_items(w);
        v.cursor = w.start;
        v.to_trigger = true;
    }
    v.rows = wave_rows(v.items, w);
    v.started = true;
    ++v.gen;
}

// An ILA capture's cursor at its first trigger, once it is read (unless it was moved by then).
void to_trigger(WaveView& v) {
    if (!v.to_trigger || !v.started || !v.now.done) return;
    std::shared_lock lock(v.wave->mu, std::try_to_lock);
    if (!lock.owns_lock()) return;
    v.to_trigger = false;
    if (v.touched || v.wave->triggers.empty()) return;
    v.cursor = v.wave->triggers.front();
    ++v.gen;
}

// The view as it is kept: rows, names of values, markers, the cursor and
// the stretch on show (none while it shows the whole run).
WaveSaved saved_of(const WaveView& v) {
    WaveSaved s;
    s.items = v.items;
    s.enums = v.enums;
    std::copy(std::begin(v.marks), std::end(v.marks), s.marks);
    s.marked = v.marked;
    s.cursor = v.cursor, s.has_cursor = true;
    s.from = v.left, s.to = v.left + v.trace_w() * v.tpp;
    if (v.fit) s.from = s.to = 0;
    return s;
}

// The rows again, after the list changed.
void rows_again(WaveView& v) {
    std::shared_lock lock(v.wave->mu);  // (the lanes' kinds and widths: fixed once the header is read, and the lock is a moment's)
    v.rows = wave_rows(v.items, *v.wave);
    if (v.sel >= v.rows.size()) v.sel = v.rows.empty() ? 0 : static_cast<u32>(v.rows.size()) - 1;
    ++v.gen;
}

// The time in a unit of 10^unit seconds (-15 fs to 0 s), as the ruler writes it.
std::string ruler_text(const Wave& w, u64 t, int unit) {
    if (w.samples) return std::to_string(t);
    static constexpr std::string_view kUnits[] = {"fs", "ps", "ns", "us", "ms", "s"};
    long double v = static_cast<long double>(t) * w.unit_mult;
    for (int e = w.unit_exp - unit; e > 0; --e) v *= 10;
    for (int e = w.unit_exp - unit; e < 0; ++e) v /= 10;
    char b[48];
    if (std::fabs(v - std::roundl(v)) < 1e-6L) std::snprintf(b, sizeof b, "%.0Lf", v);
    else std::snprintf(b, sizeof b, "%.3Lg", v);
    return std::string(b) + " " + std::string(kUnits[std::clamp((unit + 15) / 3, 0, 5)]);
}

// A time exactly, in the largest unit it has a whole one of: "1.4135 us"
// (for the tests: time_text's three decimals round a half one way on one
// system and the other on another).
std::string exact_text(const Wave& w, u64 t) {
    if (w.samples) return std::to_string(t);
    static constexpr std::string_view kUnits[] = {"fs", "ps", "ns", "us", "ms", "s"};
    // t * mult in units of 10^e seconds (e a multiple of 3, from fs to s), then a unit up while it has a whole one.
    unsigned __int128 v = static_cast<unsigned __int128>(t) * w.unit_mult;
    int e = w.unit_exp;
    while (e > -15 && e % 3 != 0) v *= 10, --e;
    while (e > 0) v *= 10, --e;
    int u = (e + 15) / 3, frac = 0;  // (frac: how many of v's digits are after the point)
    for (unsigned __int128 next = 1000; u < 5 && frac < 30 && v >= next; next *= 1000) frac += 3, ++u;
    std::string digits;
    for (unsigned __int128 x = v; x || digits.size() <= static_cast<size_t>(frac); x /= 10) digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(x % 10)));
    std::string out = digits.substr(0, digits.size() - static_cast<size_t>(frac));
    std::string after = digits.substr(digits.size() - static_cast<size_t>(frac));
    while (!after.empty() && after.back() == '0') after.pop_back();
    if (!after.empty()) out += "." + after;
    return out + " " + std::string(kUnits[std::clamp(u, 0, 5)]);
}

std::string exact_delta(const Wave& w, u64 from, u64 to) { return (to >= from ? "+" : "-") + exact_text(w, to >= from ? to - from : from - to); }

std::string delta_text(const Wave& w, u64 from, u64 to) { return (to >= from ? "+" : "-") + w.time_text(to >= from ? to - from : from - to); }

// How far it is read, for the bottom row.
std::string reading(const WaveView& v) {
    if (!v.load || !v.load->size()) return "Reading\xE2\x80\xA6";
    return "Reading " + std::to_string(v.load->read() * 100 / v.load->size()) + "%";
}

// Whether two readings of a dump have the same signals (a rerun of the same design).
bool same_signals(const Wave& a, const Wave& b) {
    if (a.vars.size() != b.vars.size() || a.scopes.size() != b.scopes.size()) return false;
    for (size_t i = 0; i < a.scopes.size(); ++i)
        if (a.scopes[i].name != b.scopes[i].name || a.scopes[i].parent != b.scopes[i].parent) return false;
    for (size_t i = 0; i < a.vars.size(); ++i)
        if (a.vars[i].name != b.vars[i].name || a.vars[i].scope != b.vars[i].scope || a.vars[i].lane != b.vars[i].lane || a.vars[i].width != b.vars[i].width) return false;
    return true;
}

// A reading again put in place of the one on show. The view is kept as
// the kept view brings it back (rows by their paths, with their radixes
// and groups, names of values, markers, the cursor, the stretch on show
// or the whole run): rows whose signals the dump has not got now are left
// out, and said. Undoing goes on as before for a dump with the same
// signals. Its tiles stay on show until they are painted again.
void put_in_place(WaveView& v) {
    std::unique_ptr<WaveView::Again> a = std::move(v.again);
    bool same = false;
    {
        std::shared_lock lock(v.wave->mu), next(a->wave->mu);
        if (v.started) v.view_text = wave_view_json(*v.wave, saved_of(v));  // (else the kept view, as it was read)
        same = v.started && same_signals(*v.wave, *a->wave);
    }
    if (!same) v.undo.clear();
    std::swap(v.fetch_stop, a->fetch_stop);  // (the last reading's fetch, if it is still going, stopped with `a`)
    v.wave = std::move(a->wave);
    v.load = std::move(a->load);
    v.fetch_error = std::move(a->fetch_error);
    v.open_error.clear();
    v.now = {};
    v.view_come = true;
    v.started = false;
    v.items.clear();
    v.rows.clear();
    v.dropped = 0;
    for (auto& [k, t] : v.tiles) t.upto = 0;  // (painted again, from the new reading)
    v.again_note.clear();
    ++v.agains;
    ++v.gen;
}

}  // namespace

// The dumps open looked at again (a file changed on disk, or a run on
// another machine ended): one that has changed is read again, or read on
// (wave_again), once its tab is on show.
void App::wave_recheck() {
    if (!waves_) return;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::shared_ptr<Doc>& d : pane->docs) {
            if (!d->wave) continue;
            WaveView& v = *d->wave;
            if (is_remote(v.path)) {  // (asked there, on the views' thread: wave_sync hears)
                waves_->post({v.doc, v.path, {}, false, true});
                continue;
            }
            i64 stamp = 0;
            u64 dev = 0, ino = 0;
            if (!file_id(v.path, &stamp, &dev, &ino) || (stamp == v.stamp && dev == v.dev && ino == v.ino)) continue;
            v.changed = true;
            ++v.gen;
            dirty_ = true;
        }
}

// A dump on show that has changed: a VCD being followed, still the same
// file, is read on from where its reading got to (which sees for itself
// whether the file was written again from its start); else, or `whole`,
// it is read again, beside what is on show, which the new reading takes
// the place of once it has its header (wave_sync).
void App::wave_again(WaveView& v, bool whole) {
    v.changed = false;
    const bool here = !is_remote(v.path);
    i64 stamp = 0;
    u64 dev = 0, ino = 0;
    if (here && !file_id(v.path, &stamp, &dev, &ino)) return;  // (gone: what was read stays on show)
    const bool same_file = here && dev == v.dev && ino == v.ino;
    if (here) v.stamp = stamp, v.dev = dev, v.ino = ino;
    WaveLoad* on = v.again ? v.again->load.get() : v.load.get();
    if (!whole && same_file && !is_fst(v.path) && on && !on->finished()) return on->more();
    auto a = std::make_unique<WaveView::Again>();
    a->wave = std::make_shared<Wave>();
    std::string why;
    a->load = load_of(v.path, a->wave, static_cast<u64>(cfg_.timeline_max_mb) << 20, static_cast<u64>(cfg_.wave_remote_fetch_mb) << 20, a->fetch_stop.get_token(), a->fetch_error,
                      WaveLoad::Follow::kAlways, &why);
    v.again.reset();
    if (a->load) v.again = std::move(a);
    else v.again_note = "Not read again: " + why;
    if (here && watch_) watch_->file(v.path);  // (a file put in its place is another to hear)
    ++v.gen;
    dirty_ = true;
}

// What the load, the painters and the view's reader have done.
void App::wave_sync() {
    sim_sync();  // (a simulation's dump found: opened, or read again)
    if (!waves_) return;
    std::vector<Waves::Read> reads;
    {
        std::lock_guard l(waves_->mu);
        reads.swap(waves_->reads);
    }
    bool changed = !reads.empty();
    u64 gens = 0;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::shared_ptr<Doc>& d : pane->docs)
            if (d->wave) gens += d->wave->gen;
    for (Waves::Read& r : reads)
        if (Doc* d = find_doc(r.doc); d && d->wave) {
            WaveView& v = *d->wave;
            if (r.stamp) {  // (another machine's dump's time stamp: the first, or one that has changed)
                if (r.when && v.stamp && r.when != v.stamp) v.changed = true;
                if (r.when) v.stamp = r.when;
            } else {
                v.view_text = std::move(r.text);
                v.view_come = true;
            }
            ++v.gen;
        }
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::shared_ptr<Doc>& d : pane->docs) {
            if (!d->wave) continue;
            WaveView& v = *d->wave;
            // Read again: put in place once it has its header; if it could
            // not be read, what is on show stays (and the bottom row says
            // why), unless nothing is.
            if (v.again) {
                const Wave& n = *v.again->wave;
                const WaveLoad& l = *v.again->load;
                bool ready = false, failed = false, writing = false;
                std::string why;
                {
                    std::shared_lock lock(n.mu, std::try_to_lock);
                    if (lock.owns_lock()) {
                        ready = n.header && n.error.empty() && !n.writing;
                        failed = l.finished() && !ready;
                        writing = n.writing;
                        why = !v.again->fetch_error->empty() ? *v.again->fetch_error : n.error;
                    }
                }
                if (ready || (failed && !v.started)) {
                    put_in_place(v);
                } else if (failed) {
                    v.again_note = writing ? "Its new run is still being written" : "Not read again: " + why;
                    v.again.reset();
                    ++v.gen;
                }
            }
            // (A followed VCD written again from its start: read again whole.)
            if (!v.again && v.load && v.load->rewritten()) wave_again(v, true);
            peek(v);
            start(v);
            to_trigger(v);
        }
    if (waves_->painter)
        for (WavePainter::Done& t : waves_->painter->take()) {
            Doc* d = find_doc(t.job.owner);
            if (!d || !d->wave || t.job.wave != d->wave->wave) continue;  // (not a tile of a reading put in another's place)
            WaveView& v = *d->wave;
            const u32 id = gpu_.image_add(t.rgba.data(), t.w, t.h);
            if (!id) continue;
            WaveView::Tile& tile = v.tiles[t.job.key];
            v.drop(tile);
            tile.rows_key = t.job.rows_key;
            tile.tpp = t.job.tpp, tile.k = t.job.k, tile.t1 = t.job.t1;
            tile.ta = t.job.tpp >= 1 ? static_cast<double>(t.job.t0) : static_cast<double>(t.job.k) * kWaveTile * t.job.tpp;
            tile.tb = t.job.tpp >= 1 ? static_cast<double>(t.job.t1) : static_cast<double>(t.job.k + 1) * kWaveTile * t.job.tpp;
            tile.upto = t.upto;
            tile.image = id;
            tile.w = t.w, tile.h = t.h;
            tile.used = v.frames;
            v.tile_bytes += static_cast<size_t>(t.w) * static_cast<size_t>(t.h) * 4;
            ++v.painted;
            v.paint_ns += t.ns;
            v.last_stats = t.stats;
            changed |= tile.ta < v.left + v.trace_w() * v.tpp && tile.tb > v.left;  // (one painted ahead, off the view, draws nothing yet)
        }
    // (Drawn again only if something on show has changed: a dump followed
    // as it grows wakes this often, and draws no more than it must.)
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::shared_ptr<Doc>& d : pane->docs)
            if (d->wave) gens -= d->wave->gen;
    if (changed || gens != 0) dirty_ = true;
}

void App::wave_layout(FrameDesc& fd) {
    if (!waves_) return;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    const Look lk = look();
    // How tiles look (made again when the theme, the font or the density change).
    {
        u64 h = mix(mix(1469598103934665603ull, font.generation()), static_cast<u64>(scale_ * 1000));
        for (const u32 ink : {static_cast<u32>(kInkAdded), kInkDiagnostic + 1, kInkDiagnostic + 2, static_cast<u32>(kInkChanged), 0u})
            for (int c = 0; c < 4; ++c) h = mix(h, static_cast<u64>(theme_.palette[ink][c] * 1000));
        if (!waves_->looks || waves_->looks->key != h) {
            auto l = std::make_shared<WaveLooks>();
            std::copy(theme_.palette[kInkAdded], theme_.palette[kInkAdded] + 4, l->line);
            std::copy(theme_.palette[kInkDiagnostic + 1], theme_.palette[kInkDiagnostic + 1] + 4, l->x);
            std::copy(theme_.palette[kInkDiagnostic + 2], theme_.palette[kInkDiagnostic + 2] + 4, l->z);
            std::copy(theme_.palette[kInkChanged], theme_.palette[kInkChanged] + 4, l->event);
            std::copy(theme_.palette[0], theme_.palette[0] + 4, l->text);
            l->line[3] = l->x[3] = l->z[3] = l->event[3] = l->text[3] = 1;
            l->font_path = font.path();
            l->font_index = font.index();
            l->fallbacks = font_fallbacks();
            l->font_px = font.px();
            l->scale = scale_;
            l->key = h;
            waves_->looks = std::move(l);
        }
    }
    std::vector<WaveTileJob> want, later;
    // (What changed on disk is read only for a tab on show, in a window that is: live views' rule.)
    const bool window_shown = !win_ || !(SDL_GetWindowFlags(win_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED));
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->wave || fd.popup_count + 2 >= std::size(fd.popups)) continue;
        WaveView& v = *pane->doc->wave;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 24 || h < ch * 6) continue;
        if (v.changed && window_shown) wave_again(v, false);
        if (watch_ && !watch_->root().empty() && v.heard_in != watch_->root() && !is_remote(v.path)) {  // (watched since: heard as it is written too)
            watch_->file(v.path);
            v.heard_in = watch_->root();
        }
        ++v.frames;
        v.x = x, v.y = y, v.w = w, v.h = h;
        const u32 cols = static_cast<u32>(w / cw), rows = static_cast<u32>(h / ch);
        const int pad_x = (w - static_cast<int>(cols) * cw) / 2, pad_y = (h - static_cast<int>(rows) * ch) / 2;
        v.pad_x = pad_x, v.pad_y = pad_y, v.cols = cols;
        // Columns: names, values at the cursor, then the traces to the right edge.
        const u32 edge = static_cast<u32>(std::max(1, (lk.edge - pad_x + cw - 1) / cw));
        v.name_cols = std::clamp<u32>(cols / 5, 12, 40);
        v.value_cols = std::clamp<u32>(cols / 9, 8, 24);
        v.trace_col = edge + v.name_cols + 1 + v.value_cols + 1;
        v.lane_rows = rows > kRulerRows + kBottomRows ? rows - kRulerRows - kBottomRows : 0;
        v.tx0 = x + pad_x + static_cast<int>(v.trace_col) * cw;
        v.tx1 = x + pad_x + static_cast<int>(cols - edge) * cw;
        v.ry0 = y + pad_y;
        v.ty0 = v.ry0 + static_cast<int>(kRulerRows) * ch;
        v.ty1 = v.ty0 + static_cast<int>(v.lane_rows) * ch;
        if (v.tx1 <= v.tx0 + cw) continue;
        peek(v);
        start(v);
        to_trigger(v);
        if (v.started) {
            if (v.want_to > v.want_from) {  // (the kept view's stretch, now that the width is known)
                v.left = v.want_from, v.tpp = (v.want_to - v.want_from) / v.trace_w();
                v.clamp();
                v.want_from = v.want_to = 0;
                ++v.gen;
            }
            if (v.fit) {
                const double was_l = v.left, was_t = v.tpp;
                v.fit_now();
                if (was_l != v.left || was_t != v.tpp) ++v.gen;
            }
            v.show_sel(v.lane_rows);
        }
        const Wave& wv = *v.wave;
        // The cells: made again only when something they show has changed.
        u64 key = 1469598103934665603ull;
        key = mix(key, v.gen), key = mix(key, cols), key = mix(key, rows), key = mix(key, font.generation());
        key = mix(key, std::bit_cast<u64>(v.left)), key = mix(key, std::bit_cast<u64>(v.tpp));
        if (key != v.key) {
            std::shared_lock lock(v.wave->mu, std::try_to_lock);  // (the values at the cursor: if the load has the lock, next time)
            if (lock.owns_lock() || !v.started) {
                v.key = key;
                v.box.resize(cols, rows);
                const u32 bottom = rows - 1;
                const u32 vc = edge + v.name_cols + 1;
                u32 c = put(v.box, font, bottom, edge, leaf_of(v.path), ink::kStrong);
                std::string err = !v.open_error.empty() ? v.open_error : !v.fetch_error->empty() ? *v.fetch_error : v.now.error;
                const u32 room = cols > c + 3 + edge ? cols - c - 3 - edge : 1;
                if (v.now.writing && v.open_error.empty() && v.fetch_error->empty()) {
                    c = put(v.box, font, bottom, c + 3, fit_text("Still being written: shown once its writer closes it", room), ink::kMuted);
                } else if (!v.now.header || !v.open_error.empty() || !v.fetch_error->empty() || (v.now.done && wv.vars.empty() && !err.empty())) {
                    if (!err.empty()) c = put(v.box, font, bottom, c + 3, fit_text("Cannot show it: " + err, room), ink::kError);
                    else c = put(v.box, font, bottom, c + 3, v.now.settled ? std::string("Nothing in it yet") : reading(v), ink::kMuted);
                } else {
                    c = put(v.box, font, bottom, c + 3, (wv.samples ? "samples " : "") + wv.time_text(v.now.start) + " to " + wv.time_text(v.now.end), ink::kMuted);
                    if (wv.samples && v.now.done && !wv.windows.empty()) c = put(v.box, font, bottom, c + 3, std::to_string(wv.windows.size() + 1) + " windows", ink::kMuted);
                    c = put(v.box, font, bottom, c + 3, std::to_string(wv.vars.size()) + (wv.vars.size() == 1 ? " signal" : " signals"), ink::kMuted);
                    if (v.load && v.load->size()) c = put(v.box, font, bottom, c + 3, size_text(v.load->size()), ink::kMuted);
                    if (!v.now.done && !v.now.settled) c = put(v.box, font, bottom, c + 3, reading(v), ink::kMuted);
                    else if (v.now.settled && v.load && v.load->grown()) c = put(v.box, font, bottom, c + 3, "Following it as it grows", ink::kMuted);
                    if (!err.empty() && err != "stopped") c = put(v.box, font, bottom, c + 3, fit_text("Stopped: " + err, cols > c + 3 + edge ? cols - c - 3 - edge : 1), ink::kError);
                    if (v.dropped) c = put(v.box, font, bottom, c + 3, std::to_string(v.dropped) + " kept rows not in the dump", ink::kWarning);
                    if (!v.again_note.empty()) c = put(v.box, font, bottom, c + 3, fit_text(v.again_note, cols > c + 3 + edge ? cols - c - 3 - edge : 1), ink::kWarning);
                    const std::string across = wv.time_text(static_cast<u64>(std::llround(v.trace_w() * v.tpp))) + (wv.samples ? " samples across" : " across");
                    if (c + 3 + width_of(across) + edge <= cols) put(v.box, font, bottom, cols - edge - width_of(across), across, ink::kMuted);
                }
                if (v.started) {
                    // The cursor's time (and what is chosen), over the names and values.
                    put(v.box, font, 0, edge, "Cursor", ink::kHeading);
                    put(v.box, font, 0, vc, fit_text(wv.time_text(v.cursor), v.value_cols), ink::kAccent);
                    if (v.ranging) {
                        put(v.box, font, 1, edge, "Chosen", ink::kHeading);
                        put(v.box, font, 1, vc, fit_text(wv.time_text(v.cursor > v.range_from ? v.cursor - v.range_from : v.range_from - v.cursor), v.value_cols), ink::kText);
                    }
                    // The ruler: times at steps of 1, 2 or 5, at least 12 cells apart.
                    const double least = 12.0 * cw * v.tpp;
                    u64 step = 1;
                    for (u64 p = 1; step < least && p < (1ull << 62) / 10; p *= 10)
                        for (const u64 m : {1ull, 2ull, 5ull})
                            if (step < least) step = p * m;
                    const long double step_s = static_cast<long double>(step) * wv.unit_mult * std::pow(10.0L, wv.unit_exp);
                    int unit = -15;
                    while (unit < 0 && step_s >= std::pow(10.0L, unit + 3)) unit += 3;
                    const u64 first = static_cast<u64>(std::ceil(std::max(0.0, v.left) / static_cast<double>(step))) * step;
                    u32 free_col = v.trace_col;
                    for (u64 t = first; v.x_of(static_cast<double>(t)) < v.tx1 && t >= first; t += step) {
                        const double px = v.x_of(static_cast<double>(t));
                        const u32 col = static_cast<u32>(std::max(0.0, std::ceil((px - x - pad_x + 0.4 * cw) / cw)));  // (just after its tick)
                        const std::string label = ruler_text(wv, t, unit);
                        if (col < free_col || col + width_of(label) > cols - edge) continue;
                        free_col = put(v.box, font, 0, col, label, ink::kMuted) + 2;
                    }
                    // Along the second row: an ILA capture's triggers (T), markers and the
                    // windows after a capture's first (w2, w3...), each where it falls if
                    // there is room for it, a trigger and a marker with how far it is from
                    // the cursor.
                    std::vector<u8> used(cols, 0);
                    auto along = [&](u64 t, std::string label, Ink in) {
                        const double px = v.x_of(static_cast<double>(t));
                        if (px < v.tx0 || px >= v.tx1) return;
                        const u32 col = static_cast<u32>(std::max(0.0, std::ceil((px - x - pad_x) / cw)));
                        if (col >= cols - edge) return;
                        label = fit_text(label, cols - edge - col);
                        const u32 end = std::min<u32>(cols, col + width_of(label) + 2);
                        for (u32 u = col > 0 ? col - 1 : 0; u < end; ++u)
                            if (used[u]) return;
                        put(v.box, font, 1, col, label, in);
                        std::fill(used.begin() + col, used.begin() + end, 1);
                    };
                    if (v.now.done)
                        for (const u64 t : wv.triggers) along(t, t == v.cursor ? std::string("T") : "T " + delta_text(wv, v.cursor, t), kTriggerInk);
                    for (u32 k = 0; k < 26; ++k)
                        if (v.marked & (1u << k)) along(v.marks[k], std::string(1, static_cast<char>('a' + k)) + (v.marks[k] != v.cursor ? " " + delta_text(wv, v.cursor, v.marks[k]) : ""), kMarkInk);
                    if (v.now.done)
                        for (size_t i = 0; i < wv.windows.size(); ++i) along(wv.windows[i], "w" + std::to_string(i + 2), ink::kMuted);
                    // A row each: its name (indented), and its value at the cursor.
                    std::string value;
                    const char* open_mark = font.can_draw(0x25BE) ? "\xE2\x96\xBE " : "v ";
                    const char* shut_mark = font.can_draw(0x25B8) ? "\xE2\x96\xB8 " : "> ";
                    for (u32 r = 0; r < v.lane_rows && v.top + r < v.rows.size(); ++r) {
                        const WaveRow& row = v.rows[v.top + r];
                        const u32 at = kRulerRows + r;
                        const u32 indent = std::min<u32>(row.depth * 2u, v.name_cols / 2);
                        std::string name = wave_row_name(v.items, row, wv);
                        Ink ink = ink::kText;
                        if (row.kind == WaveRow::kGroup) {
                            const WaveItem& it = v.items[row.item];
                            name = (it.open ? open_mark : shut_mark) + name + (it.open ? "" : " (" + std::to_string(it.members.size()) + ")");
                            ink = ink::kHeading;
                        } else if (row.kind == WaveRow::kBit) {
                            ink = ink::kMuted;
                        }
                        put(v.box, font, at, edge + indent, fit_text(name, v.name_cols - indent, row.kind == WaveRow::kSignal), ink);
                        if (const WaveSignal* s = wave_row_signal(v.items, row); s && s->var < wv.vars.size() && lock.owns_lock()) {
                            const Lane& lane = wv.lanes[wv.vars[s->var].lane];
                            const u64 i = lane.at(v.cursor);
                            const WaveFormat f = wave_row_format(v.items, row);
                            wave_value(lane, i, f, &value);
                            const bool odd = i != ~0ull && lane.kind() == LaneKind::kBits && lane.has_xz() && (f.bit >= 0 ? lane.xz(i, static_cast<u32>(f.bit), 1) != 0 : [&] {
                                for (u32 b = 0; b < lane.width(); b += 64)
                                    if (lane.xz(i, b, std::min<u32>(64, lane.width() - b))) return true;
                                return false;
                            }());
                            put(v.box, font, at, vc, fit_text(value, v.value_cols), odd ? ink::kWarning : ink::kText);
                        }
                        const bool chosen = v.vrow >= 0 && static_cast<i64>(v.top + r) >= std::min<i64>(v.vrow, v.sel) && static_cast<i64>(v.top + r) <= std::max<i64>(v.vrow, v.sel);
                        if (v.top + r == v.sel || chosen) v.box.select_row(at);
                    }
                    // A key waiting for its second: said where Vim says it.
                    if (v.pending) put(v.box, font, bottom, cols - edge - 1, std::string(1, v.pending), ink::kAccent);
                }
            }
        }
        Popup& q = fd.popups[fd.popup_count++];
        q = Popup{};
        q.x = x, q.y = y, q.w = w, q.h = h;
        q.pad_x = pad_x, q.pad_y = pad_y;
        q.cols = v.box.cols, q.rows = v.box.rows;
        q.cells = v.box.cells.data();
        q.border_px = 0;
        q.bg = theme_.bg;
        if (!v.started) continue;
        // Pictures over the cells: tiles (or stand-ins), then the lines.
        const int clip[4] = {v.tx0, v.ty0, v.tx1, v.ty1};
        auto picture = [&](u32 image, double x0, double y0, double x1, double y1, const int* cut, float opacity = 1) {
            if (!image || fd.picture_count >= std::size(fd.pictures)) return;
            PictureDraw& d = fd.pictures[fd.picture_count++];
            d = PictureDraw{};
            d.image = image;
            d.rect[0] = static_cast<float>(x0), d.rect[1] = static_cast<float>(y0), d.rect[2] = static_cast<float>(x1), d.rect[3] = static_cast<float>(y1);
            std::copy(cut, cut + 4, d.clip);
            d.after = fd.popup_count;
            d.opacity = opacity;
            d.clear = true;  // (a tile is see-through where nothing is drawn)
        };
        v.sharp = v.stretched = v.missing = 0;
        const double tpp_bits = v.tpp;
        const i64 kmin = static_cast<i64>(std::floor(v.left / v.tpp / kWaveTile)), kmax = static_cast<i64>(std::floor((v.left + v.trace_w() * v.tpp) / v.tpp / kWaveTile));
        const u32 b0 = v.top / kWaveTileRows, b1 = v.lane_rows ? (v.top + v.lane_rows - 1) / kWaveTileRows : b0;
        const bool progressed_done = v.now.done;
        for (u32 b = b0; b <= b1 && b * kWaveTileRows < v.rows.size(); ++b) {
            const u32 r0 = b * kWaveTileRows, r1 = std::min<u32>(r0 + kWaveTileRows, static_cast<u32>(v.rows.size()));
            std::vector<WaveTileRow> trows(r1 - r0);
            u64 rows_key = mix(waves_->looks->key, static_cast<u64>(ch));
            for (u32 r = r0; r < r1; ++r) {
                WaveTileRow& tr = trows[r - r0];
                if (const WaveSignal* s = wave_row_signal(v.items, v.rows[r]); s && s->var < wv.vars.size()) {
                    tr.lane = wv.vars[s->var].lane;
                    tr.fmt = wave_row_format(v.items, v.rows[r]);
                }
                rows_key = row_key(rows_key, tr);
            }
            const double by0 = v.ty0 + (static_cast<double>(r0) - v.top) * ch, by1 = by0 + static_cast<double>(r1 - r0) * ch;
            for (i64 k = std::max<i64>(0, kmin - 1); k <= kmax + 1; ++k) {
                const bool on_show = k >= kmin && k <= kmax;
                const u64 tkey = mix(mix(rows_key, std::bit_cast<u64>(tpp_bits)), static_cast<u64>(k));
                auto job = [&] {
                    WaveTileJob j;
                    j.key = tkey, j.rows_key = rows_key, j.owner = v.doc;
                    j.wave = v.wave, j.looks = waves_->looks, j.rows = trows;
                    j.tpp = v.tpp, j.k = k, j.row_h = ch;
                    wave_tile_times(v.tpp, k, &j.t0, &j.t1);
                    return j;
                };
                const double sx0 = std::round(v.tx0 + static_cast<double>(k) * kWaveTile - v.left / v.tpp);
                const auto it = v.tiles.find(tkey);
                if (it != v.tiles.end()) {
                    WaveView::Tile& t = it->second;
                    t.used = v.frames;
                    // (Painted while the dump was being read: again, once more of it has come.)
                    const bool stale = t.upto != ~0ull && t.upto < t.t1 && (progressed_done || v.now.reached > t.upto);
                    if (stale) (on_show ? want : later).push_back(job());
                    if (on_show) {
                        picture(t.image, sx0, by0, sx0 + t.w, by0 + t.h, clip);
                        ++v.sharp;
                    }
                    continue;
                }
                (on_show ? want : later).push_back(job());
                if (!on_show) continue;
                ++v.missing;
                // Stand-ins: tiles of these rows at other zooms, over this one's place, stretched.
                const int cut[4] = {std::max(v.tx0, static_cast<int>(sx0)), std::max(v.ty0, static_cast<int>(by0)), std::min(v.tx1, static_cast<int>(sx0) + kWaveTile),
                                    std::min(v.ty1, static_cast<int>(by1))};
                if (cut[2] <= cut[0] || cut[3] <= cut[1]) continue;
                const double ta = v.left + (cut[0] - v.tx0) * v.tpp, tb = v.left + (cut[2] - v.tx0) * v.tpp;
                std::vector<WaveView::Tile*> under;
                for (auto& [kk, t] : v.tiles)
                    if (t.rows_key == rows_key && t.tb > ta && t.ta < tb) under.push_back(&t);
                std::ranges::sort(under, [&](const WaveView::Tile* a, const WaveView::Tile* c) { return std::fabs(std::log(a->tpp / v.tpp)) > std::fabs(std::log(c->tpp / v.tpp)); });
                if (under.size() > 4) under.erase(under.begin(), under.end() - 4);
                for (WaveView::Tile* t : under) {
                    picture(t->image, v.x_of(t->ta), by0, v.x_of(t->tb), by0 + t->h, cut);
                    t->used = v.frames;
                    ++v.stretched;
                }
            }
        }
        // The ruler's ticks, the markers, a stretch chosen and the cursor: one-pixel pictures stretched.
        const int lw = std::max(1, static_cast<int>(std::lround(scale_)));
        const int all[4] = {v.tx0, v.ry0, v.tx1, v.ty1};
        {
            const double least = 12.0 * cw * v.tpp;
            u64 step = 1;
            for (u64 p = 1; step < least && p < (1ull << 62) / 10; p *= 10)
                for (const u64 m : {1ull, 2ull, 5ull})
                    if (step < least) step = p * m;
            const u64 first = static_cast<u64>(std::ceil(std::max(0.0, v.left) / static_cast<double>(step))) * step;
            const u32 tick = dot_of(gpu_, waves_->dots, theme_.palette[kInkMuted]);
            for (u64 t = first; v.x_of(static_cast<double>(t)) < v.tx1 && t >= first; t += step) {
                const double px = std::round(v.x_of(static_cast<double>(t)));
                picture(tick, px, v.ry0 + ch / 4.0, px + lw, v.ry0 + ch, all, 0.7f);
            }
        }
        if (v.ranging) {
            const double a = std::round(v.x_of(static_cast<double>(std::min(v.cursor, v.range_from)))), b = std::round(v.x_of(static_cast<double>(std::max(v.cursor, v.range_from))));
            picture(dot_of(gpu_, waves_->dots, theme_.sel), a, v.ry0, std::max(a + lw, b), v.ty1, all, 0.35f);
        }
        if (v.now.done) {  // (an ILA capture's windows, then its triggers)
            const u32 win = dot_of(gpu_, waves_->dots, theme_.palette[kInkMuted]), trig = dot_of(gpu_, waves_->dots, theme_.palette[kTriggerInk.ink]);
            for (const u64 t : wv.windows) {
                const double px = std::round(v.x_of(static_cast<double>(t)));
                picture(win, px, v.ry0 + ch, px + lw, v.ty1, all, 0.6f);
            }
            for (const u64 t : wv.triggers) {
                const double px = std::round(v.x_of(static_cast<double>(t)));
                picture(trig, px, v.ry0 + ch, px + lw, v.ty1, all);
            }
        }
        const u32 mark = dot_of(gpu_, waves_->dots, theme_.palette[kMarkInk.ink]);
        for (u32 k = 0; k < 26; ++k)
            if (v.marked & (1u << k)) {
                const double px = std::round(v.x_of(static_cast<double>(v.marks[k])));
                picture(mark, px, v.ry0 + ch, px + lw, v.ty1, all);
            }
        const double cx = std::round(v.x_of(static_cast<double>(v.cursor)));
        picture(dot_of(gpu_, waves_->dots, theme_.palette[kInkAccent]), cx, v.ry0, cx + lw, v.ty1, all);
        // The tiles least lately drawn let go, past the budget.
        if (v.tile_bytes > kTileBudget) {
            std::vector<std::pair<u64, u64>> by_use;
            for (const auto& [kk, t] : v.tiles) by_use.emplace_back(t.used, kk);
            std::ranges::sort(by_use);
            for (const auto& [used, kk] : by_use) {
                if (v.tile_bytes <= kTileBudget || used >= v.frames) break;
                v.drop(v.tiles[kk]);
                v.tiles.erase(kk);
            }
        }
        // The view kept, once it has been changed by hand.
        if (v.touched && v.started) {
            std::string text = wave_view_json(wv, saved_of(v));
            if (text != v.written) {
                v.written = text;
                waves_->post({v.doc, v.view_path, std::move(text), true});
            }
        }
    }
    if (!want.empty() || !later.empty() || waves_->painter) {
        if (!waves_->painter) waves_->painter = std::make_unique<WavePainter>(wake);
        want.insert(want.end(), std::make_move_iterator(later.begin()), std::make_move_iterator(later.end()));
        waves_->painter->want(std::move(want));
    }
}

bool App::wave_hit(int px, int py, Pane** pane) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->wave) continue;
        const WaveView& v = *p->doc->wave;
        if (px < v.x || py < v.y || px >= v.x + v.w || py >= v.y + v.h) continue;
        *pane = p.get();
        return true;
    }
    return false;
}

namespace {

// The change after (or before) the cursor of a row: a bus's next value, a
// bit's next level (a bit of a bus opened: where that bit changes).
bool next_change(const Wave& w, const std::vector<WaveItem>& items, const WaveRow& row, u64 from, bool forward, u64* out) {
    const WaveSignal* s = wave_row_signal(items, row);
    if (!s || s->var >= w.vars.size()) return false;
    const Lane& lane = w.lanes[w.vars[s->var].lane];
    if (row.kind != WaveRow::kBit) return forward ? lane.next_change(from, out) : lane.prev_change(from, out);
    const u64 here = lane.at(from);
    const Bit level = here == ~0ull ? Bit::kX : lane.bit(here, row.bit);
    if (forward) {
        for (u64 i = here == ~0ull ? 0 : here + 1; i < lane.size(); ++i)
            if (lane.bit(i, row.bit) != level || here == ~0ull) return *out = lane.time(i), true;
        return false;
    }
    // Back: to the start of the level the cursor is in, or (at it) of the one before.
    if (here == ~0ull) return false;
    u64 i = here;
    while (i > 0 && lane.bit(i - 1, row.bit) == level) --i;
    if (lane.time(i) < from) return *out = lane.time(i), true;
    if (i == 0) return false;
    const Bit before = lane.bit(i - 1, row.bit);
    u64 j = i - 1;
    while (j > 0 && lane.bit(j - 1, row.bit) == before) --j;
    return *out = lane.time(j), true;
}

}  // namespace

// What a key or a command does to the wave tab in front.
void App::wave_act(int act) {
    if (!pane_->doc->wave) {
        notify("The tab in front shows no waves");
        return;
    }
    WaveView& v = *pane_->doc->wave;
    if (!v.started) return;
    const Wave& w = *v.wave;
    const double view = v.trace_w() * v.tpp;
    const double about = static_cast<double>(v.cursor) >= v.left && static_cast<double>(v.cursor) <= v.left + view ? static_cast<double>(v.cursor) : v.left + view / 2;
    const WaveRow* row = v.sel < v.rows.size() ? &v.rows[v.sel] : nullptr;
    WaveSignal* sig = row ? wave_row_signal(v.items, *row) : nullptr;
    auto edited = [&] {
        rows_again(v);
        v.touched = true;
    };
    switch (act) {
        case kWavePanLeft: case kWavePanRight: case kWavePageLeft: case kWavePageRight:
            v.left += (act == kWavePanLeft || act == kWavePageLeft ? -1 : 1) * (act == kWavePanLeft || act == kWavePanRight ? 0.1 : 1.0) * view;
            v.fit = false;
            v.clamp();
            break;
        case kWaveZoomIn: v.zoom(2, about); break;
        case kWaveZoomOut: v.zoom(0.5, about); break;
        case kWaveFit: v.fit = true; v.fit_now(); break;
        case kWaveDown: if (v.sel + 1 < v.rows.size()) ++v.sel; break;
        case kWaveUp: if (v.sel > 0) --v.sel; break;
        case kWaveMoveDown: case kWaveMoveUp: {
            if (!row || row->kind == WaveRow::kBit) break;
            const int by = act == kWaveMoveDown ? 1 : -1;
            v.remember();
            if (row->member != ~0u) {  // within its group
                std::vector<WaveSignal>& m = v.items[row->item].members;
                const i64 to = static_cast<i64>(row->member) + by;
                if (to < 0 || to >= static_cast<i64>(m.size())) return v.undo.pop_back();
                std::swap(m[row->member], m[static_cast<size_t>(to)]);
            } else {
                const i64 to = static_cast<i64>(row->item) + by;
                if (to < 0 || to >= static_cast<i64>(v.items.size())) return v.undo.pop_back();
                std::swap(v.items[row->item], v.items[static_cast<size_t>(to)]);
            }
            const WaveRow was = *row;
            edited();
            // (The same row chosen, where it is now.)
            for (u32 r = 0; r < v.rows.size(); ++r) {
                const WaveRow& n = v.rows[r];
                const bool same = was.member != ~0u ? n.item == was.item && n.member == static_cast<u32>(static_cast<i64>(was.member) + by) && n.kind == was.kind
                                                     : n.item == static_cast<u32>(static_cast<i64>(was.item) + by) && n.member == ~0u && n.kind == was.kind;
                if (same) {
                    v.sel = r;
                    break;
                }
            }
            break;
        }
        case kWaveNext: case kWavePrev: {
            if (!row) break;
            u64 t = 0;
            std::shared_lock lock(v.wave->mu);
            if (next_change(w, v.items, *row, v.cursor, act == kWaveNext, &t)) {
                v.cursor = t;
                v.show_time(t);
                v.touched = true;
            }
            break;
        }
        case kWaveAdd: {
            std::vector<std::string> paths;
            paths.reserve(w.vars.size());
            for (u32 i = 0; i < w.vars.size(); ++i) paths.push_back(wave_var_name(w, i));
            const u64 doc = pane_->doc->id;
            find_choose_item("Add a signal", std::move(paths), [this, doc](int i) {
                Doc* d = find_doc(doc);
                if (!d || !d->wave || i < 0) return;
                wave_add(*d->wave, static_cast<u32>(i));
            });
            return;
        }
        case kWaveMark: case kWaveGoMark: {
            const u64 doc = pane_->doc->id;
            const bool go = act == kWaveGoMark;
            find_choose(kFindInput, go ? "Go to the marker (a to z)" : "Set a marker at the cursor (a to z)", {}, [this, doc, go](const std::string& s) {
                Doc* d = find_doc(doc);
                if (d && d->wave && s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') wave_mark(*d->wave, s[0], go);
            });
            return;
        }
        case kWaveRange:
            v.ranging = !v.ranging;
            v.range_from = v.cursor;
            break;
        case kWaveZoomRange:
            if (v.ranging) {
                v.zoom_to(static_cast<double>(std::min(v.cursor, v.range_from)), static_cast<double>(std::max(v.cursor, v.range_from)));
                v.ranging = false;
            } else if (v.last_mark >= 0 && v.marks[v.last_mark] != v.cursor) {
                v.zoom_to(static_cast<double>(std::min(v.cursor, v.marks[v.last_mark])), static_cast<double>(std::max(v.cursor, v.marks[v.last_mark])));
            }
            v.touched = true;
            break;
        case kWaveRadix: {
            if (!sig || row->kind == WaveRow::kBit) break;
            const WaveVar& var = w.vars[sig->var];
            const Lane& lane = w.lanes[var.lane];
            if (lane.kind() != LaneKind::kBits || lane.width() < 2) break;
            v.remember();
            WaveRadix r = sig->fmt.radix;
            for (u32 n = 0; n < kWaveRadixCount; ++n) {
                r = static_cast<WaveRadix>((static_cast<u32>(r) + 1) % kWaveRadixCount);
                // (Names only where the view names its values; fixed point once its Qm.n is given; floats of 16, 32 or 64 bits.)
                if (r == WaveRadix::kEnum && !sig->fmt.names) continue;
                if (r == WaveRadix::kFixed && sig->fmt.frac == 0 && sig->fmt.sign) continue;
                if (r == WaveRadix::kFloat && lane.width() != 16 && lane.width() != 32 && lane.width() != 64) continue;
                break;
            }
            sig->fmt.radix = r;
            edited();
            break;
        }
        case kWaveOpen: {
            if (!row) break;
            v.remember();
            if (row->kind == WaveRow::kGroup) {
                v.items[row->item].open = !v.items[row->item].open;
            } else if (sig && (row->kind == WaveRow::kBit || (w.lanes[w.vars[sig->var].lane].kind() == LaneKind::kBits && w.lanes[w.vars[sig->var].lane].width() > 1))) {
                sig->open = row->kind == WaveRow::kBit ? false : !sig->open;
                if (row->kind == WaveRow::kBit)  // (closed from one of its bits: the bus chosen)
                    while (v.sel > 0 && v.rows[v.sel].kind == WaveRow::kBit) --v.sel;
            } else {
                v.undo.pop_back();
                break;
            }
            edited();
            break;
        }
        case kWaveRows: v.vrow = v.vrow >= 0 ? -1 : static_cast<i64>(v.sel); break;
        case kWaveGroup: {
            // The signals of the rows chosen (or the row), out of where they were, into a group where the first was.
            const u32 a = v.vrow >= 0 ? static_cast<u32>(std::min<i64>(v.vrow, v.sel)) : v.sel, b = v.vrow >= 0 ? static_cast<u32>(std::max<i64>(v.vrow, v.sel)) : v.sel;
            std::vector<std::pair<u32, u32>> take;  // (item, member)
            for (u32 r = a; r <= b && r < v.rows.size(); ++r)
                if (v.rows[r].kind == WaveRow::kSignal) take.emplace_back(v.rows[r].item, v.rows[r].member);
            if (take.empty()) break;
            v.remember();
            WaveItem g;
            g.group = true;
            u32 number = 1;  // (the first "Group N" not taken)
            while (std::ranges::any_of(v.items, [&](const WaveItem& it) { return it.group && it.name == "Group " + std::to_string(number); })) ++number;
            g.name = "Group " + std::to_string(number);
            for (const auto& [i, m] : take) g.members.push_back(m == ~0u ? v.items[i].sig : v.items[i].members[m]);
            const u32 at = take.front().second == ~0u ? take.front().first : take.front().first + 1;
            for (auto it = take.rbegin(); it != take.rend(); ++it) {
                if (it->second == ~0u) v.items[it->first].group = true, v.items[it->first].name = "\x01";  // (marked to go)
                else v.items[it->first].members.erase(v.items[it->first].members.begin() + it->second);
            }
            v.items.insert(v.items.begin() + std::min<u32>(at, static_cast<u32>(v.items.size())), std::move(g));
            std::erase_if(v.items, [](const WaveItem& it) { return it.group && (it.name == "\x01" || it.members.empty()); });
            v.vrow = -1;
            edited();
            for (u32 r = 0; r < v.rows.size(); ++r)
                if (v.rows[r].kind == WaveRow::kGroup && v.items[v.rows[r].item].name == "Group " + std::to_string(number)) v.sel = r;
            break;
        }
        case kWaveRemove: {
            if (!row) break;
            v.remember();
            if (row->kind == WaveRow::kBit) {
                if (sig) sig->open = false;
                while (v.sel > 0 && v.rows[v.sel].kind == WaveRow::kBit) --v.sel;
            } else if (row->member != ~0u) {
                v.items[row->item].members.erase(v.items[row->item].members.begin() + row->member);
            } else {
                v.items.erase(v.items.begin() + row->item);
            }
            edited();
            break;
        }
        case kWaveUndo:
            if (v.undo.empty()) break;
            v.items = std::move(v.undo.back());
            v.undo.pop_back();
            edited();
            break;
        case kWaveCopyPath: case kWaveCopyValue: {
            if (!sig) break;
            std::string text;
            if (act == kWaveCopyPath) {
                text = w.path(sig->var);
            } else {
                std::shared_lock lock(v.wave->mu);
                const Lane& lane = w.lanes[w.vars[sig->var].lane];
                wave_value(lane, lane.at(v.cursor), wave_row_format(v.items, *row), &text);
            }
            if (win_) SDL_SetClipboardText(text.c_str());
            else g_wave_clipboard = text;
            break;
        }
        case kWaveNames: case kWaveFixed: {
            if (!sig || row->kind == WaveRow::kBit || w.lanes[w.vars[sig->var].lane].kind() != LaneKind::kBits) break;
            const u64 doc = pane_->doc->id;
            const bool names = act == kWaveNames;
            const u32 width = w.vars[sig->var].width;
            const auto known = v.enums.find(w.path(sig->var));
            const std::string typed = names ? (known != v.enums.end() ? known->second->text() : std::string()) : (sig->fmt.frac ? sig->fmt.q_text(width) : std::string());
            find_choose(kFindInput, names ? "Names of its values (IDLE START, or 0=IDLE 4=DATA)" : "Fixed point (Q" + std::to_string(width / 2) + "." + std::to_string(width - width / 2) + ", UQ for unsigned)", typed,
                        [this, doc, names](const std::string& s) {
                            Doc* d = find_doc(doc);
                            if (d && d->wave) wave_format(*d->wave, names, s);
                        });
            return;
        }
        case kWaveRename: {
            if (!row || row->kind != WaveRow::kGroup) break;
            const u64 doc = pane_->doc->id;
            const u32 item = row->item;
            find_choose(kFindInput, "Name of the group", v.items[item].name, [this, doc, item](const std::string& s) {
                Doc* d = find_doc(doc);
                if (!d || !d->wave || item >= d->wave->items.size() || !d->wave->items[item].group || s.empty()) return;
                d->wave->remember();
                d->wave->items[item].name = s;
                rows_again(*d->wave);
                d->wave->touched = true;
                dirty_ = true;
            });
            return;
        }
        default: break;
    }
    if (act == kWaveFit || act == kWavePanLeft || act == kWavePanRight || act == kWavePageLeft || act == kWavePageRight || act == kWaveZoomIn || act == kWaveZoomOut || act == kWaveMark)
        v.touched = true;
    v.show_sel(v.lane_rows);
    ++v.gen;
    dirty_ = true;
}

void App::wave_run(int index) {
    if (index >= 0 && static_cast<size_t>(index) < std::size(kWaveKeys)) wave_act(kWaveKeys[index].act);
}

// A signal added after the row chosen (into its group, if it is in one); if it is listed already, that row chosen.
void App::wave_add(WaveView& v, u32 var) {
    if (!v.started || var >= v.wave->vars.size()) return;
    for (u32 r = 0; r < v.rows.size(); ++r)
        if (const WaveSignal* s = wave_row_signal(v.items, v.rows[r]); s && s->var == var && v.rows[r].kind == WaveRow::kSignal) {
            v.sel = r;
            ++v.gen;
            dirty_ = true;
            return;
        }
    v.remember();
    WaveSignal s;
    s.var = var;
    s.fmt.radix = wave_radix_default(v.wave->vars[var]);
    if (const auto it = v.enums.find(v.wave->path(var)); it != v.enums.end()) s.fmt.names = it->second;
    const WaveRow* row = v.sel < v.rows.size() ? &v.rows[v.sel] : nullptr;
    if (row && row->kind != WaveRow::kGroup && row->member != ~0u) {
        auto& m = v.items[row->item].members;
        m.insert(m.begin() + row->member + 1, s);
    } else if (row && row->kind == WaveRow::kGroup && v.items[row->item].open) {
        v.items[row->item].members.insert(v.items[row->item].members.begin(), s);
    } else {
        WaveItem it;
        it.sig = s;
        v.items.insert(v.items.begin() + (row ? row->item + 1 : static_cast<u32>(v.items.size())), std::move(it));
    }
    rows_again(v);
    for (u32 r = 0; r < v.rows.size(); ++r)
        if (const WaveSignal* t = wave_row_signal(v.items, v.rows[r]); t && t->var == var && v.rows[r].kind == WaveRow::kSignal) v.sel = r;
    v.show_sel(v.lane_rows);
    v.touched = true;
    dirty_ = true;
}

// m{a-z}: a marker at the cursor; '{a-z}: the cursor to it.
void App::wave_mark(WaveView& v, char letter, bool go) {
    const u32 k = static_cast<u32>(letter - 'a');
    if (k >= 26) return;
    if (go) {
        if (!(v.marked & (1u << k))) return notify(std::string("No marker ") + letter);
        v.cursor = v.marks[k];
        v.show_time(v.cursor);
    } else {
        v.marks[k] = v.cursor;
        v.marked |= 1u << k;
        v.last_mark = static_cast<int>(k);
    }
    v.touched = true;
    ++v.gen;
    dirty_ = true;
}

// Names for the row's values, or its fixed point, as typed.
void App::wave_format(WaveView& v, bool names, const std::string& text) {
    const WaveRow* row = v.sel < v.rows.size() ? &v.rows[v.sel] : nullptr;
    WaveSignal* sig = row ? wave_row_signal(v.items, *row) : nullptr;
    if (!sig) return;
    const std::string path = v.wave->path(sig->var);
    const u32 width = v.wave->vars[sig->var].width;
    if (names) {
        auto e = std::make_shared<WaveEnum>();
        if (!WaveEnum::parse(text, e.get())) return notify("Not names of values: " + text, 1);
        v.remember();
        v.enums[path] = e;
        // (Every row of that signal names them.)
        auto give = [&](WaveSignal& s) {
            if (s.var == sig->var) s.fmt.names = e;
        };
        for (WaveItem& it : v.items) {
            if (!it.group) give(it.sig);
            for (WaveSignal& m : it.members) give(m);
        }
        sig->fmt.radix = WaveRadix::kEnum;
    } else {
        WaveFormat f = sig->fmt;
        if (!WaveFormat::parse_q(text, width, &f)) return notify("Not a fixed point for " + std::to_string(width) + " bits: " + text + " (Qm.n, m + n = " + std::to_string(width) + ")", 1);
        v.remember();
        sig->fmt.frac = f.frac, sig->fmt.sign = f.sign;
        sig->fmt.radix = WaveRadix::kFixed;
    }
    rows_again(v);
    v.touched = true;
    dirty_ = true;
}

bool App::wave_key(const SDL_KeyboardEvent& k) {
    if (!pane_->doc->wave || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) return false;
    if (cfg_.modal && vim_.mode() == Vim::kCommand) return false;  // (Vim's command line has the keys)
    WaveView& v = *pane_->doc->wave;
    const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    auto took = [&] {
        term_drop_text_ = true;  // (a key's text is not typed after it)
        ++v.gen;
        dirty_ = true;
        return true;
    };
    if (k.key == SDLK_ESCAPE) {
        if (!v.pending && !v.ranging && v.vrow < 0) return false;
        v.pending = 0, v.ranging = false, v.vrow = -1;
        return took();
    }
    // The second key of two: m{a-z} '{a-z} dd gG yy yv.
    if (const char first = v.pending) {
        v.pending = 0;
        const bool letter = k.key >= SDLK_A && k.key <= SDLK_Z;
        const char c = letter ? static_cast<char>('a' + (k.key - SDLK_A)) : 0;
        if ((first == 'm' || first == '\'') && letter && !shift) wave_mark(v, c, first == '\'');
        else if (first == 'd' && c == 'd' && !shift) wave_act(kWaveRemove);
        else if (first == 'g' && c == 'g' && shift) wave_act(kWaveGroup);
        else if (first == 'g' && c == 'w' && shift && cfg_.modal) sim_last_waves();  // (gW, as in a file)
        else if (first == 'y' && c == 'y' && !shift) wave_act(kWaveCopyPath);
        else if (first == 'y' && c == 'v' && !shift) wave_act(kWaveCopyValue);
        return took();
    }
    switch (k.key) {
        case SDLK_M: case SDLK_D: case SDLK_G: case SDLK_Y:
            if (shift) break;
            v.pending = static_cast<char>('a' + (k.key - SDLK_A));
            return took();
        case SDLK_APOSTROPHE:
            if (shift) break;
            v.pending = '\'';
            return took();
        case SDLK_PLUS: case SDLK_KP_PLUS: wave_act(kWaveZoomIn); return took();
        case SDLK_KP_MINUS: wave_act(kWaveZoomOut); return took();
        case SDLK_LEFT: wave_act(shift ? kWavePageLeft : kWavePanLeft); return took();
        case SDLK_RIGHT: wave_act(shift ? kWavePageRight : kWavePanRight); return took();
        case SDLK_DOWN: wave_act(kWaveDown); return took();
        case SDLK_UP: wave_act(kWaveUp); return took();
        case SDLK_KP_ENTER: wave_act(kWaveOpen); return took();
        default: break;
    }
    for (const WaveKey& w : kWaveKeys)
        if (w.cmd.key && w.cmd.key == k.key && (w.cmd.shift == shift || (w.act == kWaveZoomIn))) {
            wave_act(w.act);
            return took();
        }
    // (Other letters are nobody's here: not typed into the empty text behind the tab.)
    if (k.key >= SDLK_A && k.key <= SDLK_Z) return took();
    return false;
}

bool App::wave_wheel(const SDL_MouseWheelEvent& e) {
    Pane* pane = nullptr;
    const float mx = mouse_x_ * density_, my = mouse_y_ * density_;
    if (!wave_hit(static_cast<int>(mx), static_cast<int>(my), &pane)) return false;
    WaveView& v = *pane->doc->wave;
    if (!v.started) return true;
    const bool precise = e.y != std::floor(e.y) || e.x != std::floor(e.x);
    if (SDL_GetModState() & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) {  // zoomed about the mouse
        const double about = mx >= v.tx0 && mx < v.tx1 ? v.left + (mx - v.tx0) * v.tpp : v.left + v.trace_w() * v.tpp / 2;
        v.zoom(std::pow(1.1, e.y), about);
    } else {
        // Sideways (or with Shift) the time; up and down the rows.
        const double step = precise ? 10.0 * scale_ : cfg_.scroll_lines * font_->cell_h();
        const bool across = SDL_GetModState() & SDL_KMOD_SHIFT;
        const double dx = across ? -e.y : e.x;
        if (dx != 0) {
            v.left += dx * step * v.tpp;
            v.fit = false;
            v.clamp();
        }
        if (!across && e.y != 0) {
            const i64 by = precise ? static_cast<i64>(std::lround(-e.y * 10 * scale_ / font_->cell_h())) : static_cast<i64>(-e.y * 3);
            const i64 most = std::max<i64>(0, static_cast<i64>(v.rows.size()) - static_cast<i64>(v.lane_rows));
            v.top = static_cast<u32>(std::clamp<i64>(static_cast<i64>(v.top) + by, 0, most));
            if (v.sel < v.top) v.sel = v.top;
            if (v.lane_rows && v.sel >= v.top + v.lane_rows) v.sel = v.top + v.lane_rows - 1;
        }
    }
    v.touched = true;
    ++v.gen;
    dirty_ = true;
    return true;
}

bool App::wave_pinch(float by) {
    Pane* pane = nullptr;
    const float mx = mouse_x_ * density_, my = mouse_y_ * density_;
    if (!wave_hit(static_cast<int>(mx), static_cast<int>(my), &pane)) return false;
    WaveView& v = *pane->doc->wave;
    if (v.started && by > 0) {
        v.zoom(by, mx >= v.tx0 && mx < v.tx1 ? v.left + (mx - v.tx0) * v.tpp : v.left + v.trace_w() * v.tpp / 2);
        v.touched = true;
    }
    dirty_ = true;
    return true;
}

// A click: on the traces, the cursor there and the row under it chosen
// (dragging moves the cursor); on the ruler, a drag zooms to the stretch.
bool App::wave_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    const float px = b.x * density_, py = b.y * density_;
    if (!wave_hit(static_cast<int>(px), static_cast<int>(py), &pane)) return false;
    focus_pane(pane);
    dirty_ = true;
    WaveView& v = *pane->doc->wave;
    if (b.button != SDL_BUTTON_LEFT || !v.started) return true;
    const int ch = font_->cell_h();
    if (py >= v.ty0 && py < v.ty1) {
        const u32 r = v.top + static_cast<u32>((py - v.ty0) / ch);
        if (r < v.rows.size()) v.sel = r;
    }
    if (px >= v.tx0 && px < v.tx1 && py >= v.ry0 && py < v.ty1) {
        const u64 t = v.time_at(px);
        if (py < v.ty0) {
            v.drag = 2, v.drag_from = t;
            v.ranging = true, v.range_from = t;
        } else {
            v.drag = 1;
        }
        v.cursor = t;
        v.touched = true;
    }
    ++v.gen;
    return true;
}

bool App::wave_mouse_motion(const SDL_MouseMotionEvent& m) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->wave || !p->doc->wave->drag) continue;
        WaveView& v = *p->doc->wave;
        v.cursor = v.time_at(std::clamp<double>(m.x * density_, v.tx0, v.tx1 - 1));
        ++v.gen;
        dirty_ = true;
        return true;
    }
    return false;
}

void App::wave_mouse_up() {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->wave || !p->doc->wave->drag) continue;
        WaveView& v = *p->doc->wave;
        if (v.drag == 2) {
            v.ranging = false;
            if (std::fabs(static_cast<double>(v.cursor) - static_cast<double>(v.drag_from)) / v.tpp >= 4)
                v.zoom_to(static_cast<double>(std::min(v.cursor, v.drag_from)), static_cast<double>(std::max(v.cursor, v.drag_from)));
        }
        v.drag = 0;
        ++v.gen;
        dirty_ = true;
    }
}

bool App::waves_busy() const {
    if (!waves_) return false;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::shared_ptr<Doc>& d : pane->docs) {
            if (!d->wave) continue;
            const WaveView& v = *d->wave;
            // (Read, and its end seen by the tab too: a load that ends between a frame and this would leave the rows unmade. A followed
            // one at the end of what is there, waiting for more, is read, once the tab has seen all it read.)
            const bool read = !v.load || (v.load->finished() && v.now.done) || (v.load->waiting() && v.now.settled && v.now.bytes == v.load->read());
            if (!read || !v.view_come || (v.again && !v.again->load->waiting())) return true;
        }
    for (const std::unique_ptr<Pane>& pane : panes_)
        if (pane->doc && pane->doc->wave && (pane->doc->wave->changed || (pane->doc->wave->started && pane->doc->wave->missing))) return true;
    {
        std::lock_guard l(waves_->mu);
        if (!waves_->ops.empty() || waves_->io_busy || !waves_->reads.empty()) return true;
    }
    return waves_->painter && waves_->painter->busy();
}

void App::wave_wait(bool rows) {
    const u64 until = now_ns() + 20'000'000'000ull;
    auto shown = [&] {  // (rows: only until the tab in front has its rows, the dump maybe still being read)
        const WaveView* v = pane_->doc->wave.get();
        return v && (v->started || !v->open_error.empty() || !v->fetch_error->empty() || (v->now.done && !v->now.error.empty() && v->wave->vars.empty()));
    };
    while (now_ns() < until) {
        wave_sync();
        frame();
        if (rows ? shown() : !waves_busy()) break;
        SDL_Delay(2);
    }
    wave_sync();
    frame();
}

// (The tests: ms milliseconds of the main loop as it runs when idle, what
// changes on disk heard and settled as it hears them: how many frames it
// drew, and how far the first wave tab was read meanwhile.)
std::string App::wave_frames(int ms) {
    const u64 until = now_ns() + static_cast<u64>(std::max(0, ms)) * 1'000'000ull;
    const u64 before = frames_;
    auto bytes = [&] {
        for (const std::unique_ptr<Pane>& pane : panes_)
            for (const std::shared_ptr<Doc>& d : pane->docs)
                if (d->wave && d->wave->load) return d->wave->load->read();
        return u64{0};
    };
    const u64 read = bytes();
    while (now_ns() < until) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle(e);
        watch_tick();
        if (dirty_) frame();
        SDL_Delay(1);
    }
    return std::to_string(frames_ - before) + " frames drawn, " + std::to_string(bytes() - read) + " bytes read";
}

// The wave tab in front (the first there is), as the tests see it:
//   (none)   what it shows: rows (name = value at the cursor, radix; > the
//            one chosen), the cursor, the stretch on show, markers
//   tiles    tiles painted, and in the last frame drawn sharp and stretched
//   at:R,T[,DX,DY,N]  where time T is on row R (from 0) in the window, in
//            pixels (the middle of the row); and the colours of N pixels
//            running rightwards from DX, DY away from there
//   clip     what y copied last
std::string App::wave_dump(std::string_view what) {
    if (what == "load")  // (the first wave tab, on show or not: how far it is read, and how often again)
        for (const std::unique_ptr<Pane>& pane : panes_)
            for (const std::shared_ptr<Doc>& d : pane->docs) {
                if (!d->wave) continue;
                const WaveView& v = *d->wave;
                const WaveLoad* l = v.load.get();
                return "load: " + leaf_of(v.path) + (pane->doc == d.get() ? " on show" : " behind another tab") + ", " + std::to_string(l ? l->read() : 0) + " bytes read, grown " +
                       std::to_string(l ? l->grown() : 0) + " times, read again " + std::to_string(v.agains) + " times" + (v.changed ? ", changed" : "") +
                       (!l ? "" : l->waiting() ? ", waiting for more" : l->finished() ? ", finished" : ", reading");
            }
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->wave) continue;
        WaveView& v = *pane->doc->wave;
        const Wave& w = *v.wave;
        std::shared_lock lock(v.wave->mu);
        if (what.starts_with("tiles")) {
            char b[200];
            std::snprintf(b, sizeof b, "tiles: %llu painted, %u sharp, %u stretched, %u to come, last %u busy %u edges %u values", static_cast<unsigned long long>(v.painted), v.sharp, v.stretched,
                          v.missing, v.last_stats.busy, v.last_stats.edges, v.last_stats.labels);
            std::string out = b;
            if (what == "tiles:time" && v.painted) {  // (for the benchmarks: how long a tile took, on average)
                std::snprintf(b, sizeof b, ", %.2f ms a tile on %u workers", static_cast<double>(v.paint_ns) * 1e-6 / static_cast<double>(v.painted), waves_->painter ? waves_->painter->threads() : 0);
                out += b;
            }
            return out;
        }
        if (what == "clip") return "copied: " + g_wave_clipboard;
        if (what.starts_with("at:")) {
            const std::string arg(what.substr(3));
            unsigned long long row = 0, t = 0;
            int dx = 0, dy = 0, n = 0;
            std::sscanf(arg.c_str(), "%llu,%llu,%d,%d,%d", &row, &t, &dx, &dy, &n);
            const int px = static_cast<int>(std::floor(v.x_of(static_cast<double>(t))));
            const int py = v.ty0 + (static_cast<int>(row) - static_cast<int>(v.top)) * font_->cell_h() + font_->cell_h() / 2;
            std::string out = "at " + std::to_string(px) + "," + std::to_string(py);
            // (And the colours of n pixels running rightwards from dx, dy away, in the last frame.)
            std::vector<u8> rgba;
            int fw = 0, fh = 0;
            if (n > 0 && gpu_.read_pixels(rgba, &fw, &fh))
                for (int k = 0; k < n; ++k) {
                    const int x = px + dx + k, y = py + dy;
                    if (x < 0 || y < 0 || x >= fw || y >= fh) {
                        out += " | none";
                        continue;
                    }
                    const u8* c = rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4;
                    out += " | " + std::to_string(c[0]) + " " + std::to_string(c[1]) + " " + std::to_string(c[2]);
                }
            return out;
        }
        // (And whether rows were made all the same: a ruler and a cursor over nothing.)
        if (!v.open_error.empty()) return leaf_of(v.path) + ": cannot show it: " + v.open_error + (v.started ? ", rows made" : "");
        if (!v.fetch_error->empty()) return leaf_of(v.path) + ": cannot show it: " + *v.fetch_error + (v.started ? ", rows made" : "");
        if (w.writing) return leaf_of(v.path) + ": still being written" + (v.started ? ", rows made" : "");
        if (w.done && w.vars.empty() && !w.error.empty()) return leaf_of(v.path) + ": cannot show it: " + w.error + (v.started ? ", rows made" : "");
        if (!w.header && w.settled) return leaf_of(v.path) + ": nothing in it yet";
        if (!v.started) return leaf_of(v.path) + ": reading";
        std::string out = leaf_of(v.path) + ": " + (w.done || w.settled ? "read" : "reading") + (w.settled && v.load && v.load->grown() ? ", following" : "") + ", cursor " +
                          exact_text(w, v.cursor) + ", view " + exact_text(w, static_cast<u64>(std::llround(std::max(0.0, v.left)))) + " to " +
                          exact_text(w, static_cast<u64>(std::llround(v.left + v.trace_w() * v.tpp)));
        if (!w.error.empty()) out += ", stopped: " + w.error;
        if (v.dropped) out += ", " + std::to_string(v.dropped) + " kept rows dropped";
        if (!v.again_note.empty()) out += ", " + v.again_note;
        if (v.ranging) out += ", chosen from " + exact_text(w, v.range_from);
        std::string marks;
        for (u32 k = 0; k < 26; ++k)
            if (v.marked & (1u << k)) marks += std::string(marks.empty() ? "" : " ") + static_cast<char>('a' + k) + "=" + exact_text(w, v.marks[k]) + "(" + exact_delta(w, v.cursor, v.marks[k]) + ")";
        if (!marks.empty()) out += ", markers " + marks;
        auto list = [&](const std::vector<u64>& ts) {
            std::string l;
            for (const u64 t : ts) l += (l.empty() ? "" : " ") + exact_text(w, t);
            return l;
        };
        if (!w.triggers.empty()) out += ", triggers at " + list(w.triggers);
        if (!w.windows.empty()) out += ", windows from " + list(w.windows);
        std::string value;
        for (u32 r = 0; r < v.rows.size(); ++r) {
            const WaveRow& row = v.rows[r];
            out += "\n" + std::string(r == v.sel ? ">" : " ") + std::string(row.depth * 2u, ' ') + wave_row_name(v.items, row, w);
            if (row.kind == WaveRow::kGroup) {
                out += v.items[row.item].open ? " (open)" : " (shut)";
                continue;
            }
            const WaveSignal* s = wave_row_signal(v.items, row);
            if (!s) continue;
            const Lane& lane = w.lanes[w.vars[s->var].lane];
            const WaveFormat f = wave_row_format(v.items, row);
            wave_value(lane, lane.at(v.cursor), f, &value);
            out += " = " + value;
            if (lane.kind() == LaneKind::kBits && lane.width() > 1 && row.kind == WaveRow::kSignal) out += " " + std::string(wave_radix_name(f.radix));
        }
        return out;
    }
    return "no wave tab";
}

// (The benchmarks: step i of zooming about the middle, in by an eighth
// twenty times, then out by a tenth twenty times, never back to a zoom
// it was at; or of panning, a twentieth of the view (a page: all of it),
// forty times one way, then back.)
void App::wave_bench_step(bool zoom, int i, bool page) {
    if (!pane_->doc->wave) return;
    WaveView& v = *pane_->doc->wave;
    if (!v.started) return;
    if (zoom) v.zoom((i / 20) % 2 ? 0.9 : 1.125, v.left + v.trace_w() * v.tpp / 2);
    else {
        v.left += ((i / 40) % 2 ? -1 : 1) * (page ? 1 : 0.05) * v.trace_w() * v.tpp;
        v.fit = false;
        v.clamp();
        ++v.gen;
    }
    dirty_ = true;
}

bool App::wave_any() const {
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::shared_ptr<Doc>& d : pane->docs)
            if (d->wave) return true;
    return false;
}

// A time Cmd+clicked in a terminal: the wave tab's cursor there (one in
// front of a split first, else the first open, which is brought to the
// front of its split), in the dump's unit (a bare number is in it already).
bool App::wave_go_time(const TextPlace& time) {
    Pane* where = nullptr;
    size_t tab = 0;
    for (const std::unique_ptr<Pane>& pane : panes_)
        if (pane->doc && pane->doc->wave) {
            where = pane.get(), tab = pane->tab;
            break;
        }
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (size_t i = 0; !where && i < pane->docs.size(); ++i)
            if (pane->docs[i]->wave) where = pane.get(), tab = i;
    if (!where) return false;
    WaveView& v = *where->docs[tab]->wave;
    if (!v.started) return false;
    u64 t = 0;
    {
        std::shared_lock lock(v.wave->mu);
        const Wave& w = *v.wave;
        if (!w.time_of(time.digits, time.frac, time.unit, time.mult, time.exp, &t)) return false;  // (a time with a unit, in a capture's samples: the click is a click)
        if ((w.done || w.settled) && (t < w.start || t > w.end)) {  // (read to its end, or to where it has got, followed)
            notify(exact_text(w, t) + " is not in " + leaf_of(v.path) + " (" + exact_text(w, w.start) + " to " + exact_text(w, w.end) + ")");
            return true;
        }
    }
    v.cursor = t;
    v.show_time(t);
    v.touched = true;
    ++v.gen;
    if (where->tab != tab) {
        focus_pane(where);
        tab_show(tab);
    }
    dirty_ = true;
    return true;
}

bool App::wave_sharp() const {
    for (const std::unique_ptr<Pane>& pane : panes_)
        if (pane->doc && pane->doc->wave && pane->doc->wave->started && pane->doc->wave->missing) return false;
    return true;
}

}  // namespace blit
