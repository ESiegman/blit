#pragma once
// The wave tab's lanes as pictures (appwave.cpp places them as PictureDraws):
// tiles of kWaveTile device pixels across and a block of rows down, each a
// stretch of time at one zoom, painted from the lanes (timeline.hpp) on two
// to four workers with raster.hpp, and the same pixels on every system.
//
// A tile is painted from one query per pixel column (Lane::columns): a
// column with no change is the value before it, one an edge, two or more a
// busy band (hatched). A bit is a line high or low, z a dashed line in the
// middle, x a filled band in its own colour; a bus is a run of hexagons
// with its value written in each that has room for it; an event a tick.
// Zoomed in past a time unit a pixel, a column is a unit, so edges fall
// where their times are.
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "font.hpp"
#include "raster.hpp"
#include "waveview.hpp"

namespace blit {

class DiagramFont;

inline constexpr int kWaveTile = 1024;  // a tile's width, device pixels
inline constexpr u32 kWaveTileRows = 16;  // rows a tile holds, at most

// How tiles look: colours (rgba, 0..1), the font the values are written in
// and its size, and the display's density. `key` tells one set from another.
struct WaveLooks {
    float line[4] = {0.4f, 0.8f, 0.4f, 1};   // a signal
    float x[4] = {0.9f, 0.6f, 0.2f, 1};      // x
    float z[4] = {0.4f, 0.6f, 0.9f, 1};      // z
    float event[4] = {0.8f, 0.6f, 0.9f, 1};  // an event
    float text[4] = {0.9f, 0.9f, 0.9f, 1};   // a bus's values
    std::string font_path;
    int font_index = 0;
    std::vector<FontFile> fallbacks;
    float font_px = 26;  // the text's size in the grid, device pixels
    float scale = 1;     // device pixels to a point
    u64 key = 0;
};

// A row of a tile: a lane (~0u: nothing, a group's heading) and how its
// values are written (and which bit of it, for a bus opened).
struct WaveTileRow {
    u32 lane = ~0u;
    WaveFormat fmt;
};

struct WaveTileJob {
    u64 key = 0;  // its rows, place and zoom, and looks: equal keys, equal tiles
    u64 rows_key = 0;  // its rows and looks alone (a tile of them at another zoom can stand in)
    u64 owner = 0;     // (whose: the tab's document)
    std::shared_ptr<Wave> wave;
    std::shared_ptr<const WaveLooks> looks;
    std::vector<WaveTileRow> rows;
    double tpp = 1;  // time a device pixel
    i64 k = 0;       // which tile across: pixels [k * kWaveTile, (k + 1) * kWaveTile) of the run at tpp
    u64 t0 = 0, t1 = 0;  // the times it covers, [t0, t1) (wave_tile_times)
    int row_h = 1;   // a row's height, device pixels
};

// The times tile k covers at tpp time a pixel: [t0, t1). (Past a unit a
// pixel, its units, the first and last maybe only partly in it.)
void wave_tile_times(double tpp, i64 k, u64* t0, u64* t1);
// Where time t falls in tile (tpp, k, t0, t1), in its pixels (outside [0, kWaveTile] if not in it).
double wave_tile_x(double tpp, i64 k, u64 t0, u64 t1, u64 t);

// What painting a tile came to.
struct WaveTileStats {
    u32 busy = 0;    // busy columns
    u32 edges = 0;
    u32 labels = 0;  // values written
};

// A tile painted into `out` (kWaveTile by rows * row_h, clear where
// nothing is). The caller holds the wave's lock shared. `upto`: the time
// the dump was read up to (~0: all of it), past which nothing is drawn.
void wave_paint_tile(const WaveTileJob& j, const DiagramFont* font, RasterImage* out, u64* upto, WaveTileStats* st = nullptr);

// Workers painting tiles: two to four of them, asked for a list of tiles
// (the most wanted first) that replaces the last; `wake` is called (on a
// worker) as each is done.
class WavePainter {
public:
    struct Done {
        WaveTileJob job;
        std::vector<u8> rgba;
        int w = 0, h = 0;
        u64 upto = ~0ull;
        u64 ns = 0;  // painting it took
        WaveTileStats stats;
    };

    explicit WavePainter(std::function<void()> wake, unsigned threads = 0);
    ~WavePainter();
    WavePainter(const WavePainter&) = delete;
    WavePainter& operator=(const WavePainter&) = delete;

    // What to paint, in place of what was asked before; those being painted
    // now are not painted again.
    void want(std::vector<WaveTileJob> jobs);
    std::vector<Done> take();
    bool busy() const;  // something asked for and not painted, or painted and not taken
    unsigned threads() const { return static_cast<unsigned>(threads_.size()); }

private:
    void run(std::stop_token st);

    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    std::deque<WaveTileJob> jobs_;
    std::vector<u64> painting_;
    std::vector<Done> done_;
    std::function<void()> wake_;
    std::vector<std::jthread> threads_;  // (last: they stop before what they use goes)
};

}  // namespace blit
