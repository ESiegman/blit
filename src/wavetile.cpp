// The wave tab's tiles and the workers that paint them (wavetile.hpp).
#include "wavetile.hpp"

#include <algorithm>
#include <cmath>
#include <shared_mutex>

#include "diagram.hpp"
#include "qos.hpp"
#include "threads.hpp"

namespace blit {

void wave_tile_times(double tpp, i64 k, u64* t0, u64* t1) {
    const double a = static_cast<double>(k) * kWaveTile * tpp, b = static_cast<double>(k + 1) * kWaveTile * tpp;
    *t0 = a <= 0 ? 0 : static_cast<u64>(std::floor(a));
    *t1 = b <= 0 ? 0 : static_cast<u64>(tpp >= 1 ? std::floor(b) : std::ceil(b));
    if (*t1 <= *t0) *t1 = *t0 + 1;
}

double wave_tile_x(double tpp, i64 k, u64 t0, u64 t1, u64 t) {
    if (tpp >= 1) return (static_cast<double>(t) - static_cast<double>(t0)) * kWaveTile / static_cast<double>(t1 - t0);
    return static_cast<double>(t) / tpp - static_cast<double>(k) * kWaveTile;
}

namespace {

// A column of a tile: its pixels [x0, x1) and its changes.
struct Col {
    int x0 = 0, x1 = 0;
    LaneColumn c;
};

// The tile's columns for a lane: a pixel each, or (zoomed in past a unit a
// pixel) a unit each, over the pixels it takes.
void columns_of(const Lane& lane, const WaveTileJob& j, std::vector<Col>* out, std::vector<LaneColumn>* scratch) {
    out->clear();
    if (j.tpp >= 1) {
        scratch->resize(kWaveTile);
        lane.columns(j.t0, j.t1, *scratch);
        for (int x = 0; x < kWaveTile; ++x) out->push_back({x, x + 1, (*scratch)[static_cast<size_t>(x)]});
        return;
    }
    const u64 n = j.t1 - j.t0;
    scratch->resize(static_cast<size_t>(n));
    lane.columns(j.t0, j.t1, *scratch);
    for (u64 u = 0; u < n; ++u) {
        const int x0 = static_cast<int>(std::clamp(std::lround(wave_tile_x(j.tpp, j.k, j.t0, j.t1, j.t0 + u)), 0L, static_cast<long>(kWaveTile)));
        const int x1 = static_cast<int>(std::clamp(std::lround(wave_tile_x(j.tpp, j.k, j.t0, j.t1, j.t0 + u + 1)), 0L, static_cast<long>(kWaveTile)));
        if (x1 > x0) out->push_back({x0, x1, (*scratch)[static_cast<size_t>(u)]});
        else if (!out->empty() && (*scratch)[static_cast<size_t>(u)].count) {  // (a unit cut to nothing at the tile's edge: its change still counts)
            Col& last = out->back();
            last.c.count += (*scratch)[static_cast<size_t>(u)].count;
        }
    }
}

struct Painter {
    RasterImage& img;
    const WaveTileJob& j;
    const WaveLooks& lk;
    int lw = 1;      // a line's width
    int gx0 = 0;     // the tile's first pixel, of the whole run's (for patterns that go on from tile to tile)

    // A rectangle filled over what is there (colours not multiplied; alpha a).
    void rect(int x0, int y0, int x1, int y1, const float* rgba, float alpha = 1) {
        x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, img.w()), y1 = std::min(y1, img.h());
        if (x0 >= x1 || y0 >= y1) return;
        const float a = rgba[3] * alpha;
        const float r = rgba[0] * a * 255, g = rgba[1] * a * 255, b = rgba[2] * a * 255, keep = 1 - a;
        std::vector<u8>& px = img.pixels();
        for (int y = y0; y < y1; ++y) {
            u8* p = px.data() + (static_cast<size_t>(y) * static_cast<size_t>(img.w()) + static_cast<size_t>(x0)) * 4;
            for (int x = x0; x < x1; ++x, p += 4) {
                p[0] = static_cast<u8>(std::min(255.0f, r + p[0] * keep + 0.5f));
                p[1] = static_cast<u8>(std::min(255.0f, g + p[1] * keep + 0.5f));
                p[2] = static_cast<u8>(std::min(255.0f, b + p[2] * keep + 0.5f));
                p[3] = static_cast<u8>(std::min(255.0f, a * 255 + p[3] * keep + 0.5f));
            }
        }
    }
    // A busy band: hatched, stripes that go on across tiles.
    void busy(int x0, int y0, int x1, int y1, const float* rgba) {
        rect(x0, y0, x1, y1, rgba, 0.15f);
        const int period = 4 * lw;
        for (int y = std::max(y0, 0); y < std::min(y1, img.h()); ++y)
            for (int x = std::max(x0, 0); x < std::min(x1, img.w()); ++x)
                if ((gx0 + x + y) % period < lw) rect(x, y, x + 1, y + 1, rgba, 0.55f);
        rect(x0, y0, x1, y0 + lw, rgba);
        rect(x0, y1 - lw, x1, y1, rgba);
    }
    // A dashed line across [x0, x1) at y.
    void dashed(int x0, int x1, int y, const float* rgba) {
        const int on = 3 * lw, period = 5 * lw;
        for (int x = std::max(x0, 0); x < std::min(x1, img.w());) {
            const int phase = (gx0 + x) % period;
            const int len = phase < on ? on - phase : period - phase;
            if (phase < on) rect(x, y, std::min(x + len, x1), y + lw, rgba);
            x += len;
        }
    }
};

// One bit's state as a level: -1 none, 0 low, 1 high, 2 z, 3 x.
int level(const Lane& lane, u64 i, u32 bit) { return i == ~0ull ? -1 : static_cast<int>(lane.bit(i, bit)); }

}  // namespace

void wave_paint_tile(const WaveTileJob& j, const DiagramFont* font, RasterImage* out, u64* upto, WaveTileStats* st, bool* again) {
    WaveTileStats dummy;
    if (!st) st = &dummy;
    bool again_dummy = false;
    if (!again) again = &again_dummy;
    *again = false;
    const Wave& w = *j.wave;
    const WaveLooks& lk = *j.looks;
    *upto = w.done ? ~0ull : w.reached;
    // A time's place in this tile as a label is placed by it: t / tpp of
    // the whole run, less the tile's start, the same in every tile (the
    // columns' own place, zoomed out, is within a pixel of it).
    const double tile_x0 = static_cast<double>(j.k) * kWaveTile;
    auto label_x = [&](u64 t) { return static_cast<double>(t) / j.tpp - tile_x0; };
    // Nothing past where the dump has been read, or past its end.
    const u64 limit_t = w.done ? w.end : w.reached;
    // (Read whole: the column its last time is in, with any change there, too.)
    const double limit_x = std::floor(wave_tile_x(j.tpp, j.k, j.t0, j.t1, limit_t)) + (w.done ? 1 : 0);
    const int limit = static_cast<int>(std::clamp(limit_x, -1.0, static_cast<double>(kWaveTile)));
    Painter p{*out, j, lk};
    p.lw = std::max(1, static_cast<int>(std::lround(lk.scale)));
    // (Where the tile starts, modulo 120, a multiple of every pattern's period: 4 and 5 line widths of 1 to 3 pixels.)
    p.gx0 = static_cast<int>(j.k % 15 * kWaveTile % 120);
    thread_local std::vector<Col> cols;
    thread_local std::vector<LaneColumn> scratch;
    std::string text;
    for (size_t r = 0; r < j.rows.size(); ++r) {
        const WaveTileRow& row = j.rows[r];
        if (row.lane >= w.lanes.size() || limit < 0) continue;
        // (Its colour, if it was given one: its lines and ticks in it.)
        const float* line = row.colour && row.colour <= kWaveColours ? lk.colours[row.colour] : lk.line;
        const float* event = row.colour && row.colour <= kWaveColours ? lk.colours[row.colour] : lk.event;
        const Lane& lane = w.lanes[row.lane];
        const int y0 = static_cast<int>(r) * j.row_h;
        const int pad = std::max(2, static_cast<int>(std::lround(j.row_h * 0.14)));
        const int hi = y0 + pad, lo = y0 + j.row_h - pad, mid = (hi + lo) / 2;
        columns_of(lane, j, &cols, &scratch);
        if (cols.empty()) continue;
        const u64 entry = cols[0].c.first == 0 ? ~0ull : cols[0].c.first - 1;  // (the value at the tile's start)
        const bool digital = lane.kind() == LaneKind::kBits && (row.fmt.bit >= 0 || lane.width() == 1);
        const u32 bit = row.fmt.bit >= 0 ? static_cast<u32>(row.fmt.bit) : 0;

        if (lane.kind() == LaneKind::kEvent) {
            for (size_t c = 0; c < cols.size() && cols[c].x0 < limit; ++c) {
                if (!cols[c].c.count) continue;
                if (cols[c].c.count > 1) {
                    size_t e = c;
                    while (e + 1 < cols.size() && cols[e + 1].c.count > 1 && cols[e + 1].x0 < limit) ++e;
                    p.busy(cols[c].x0, hi, std::min(cols[e].x1, limit), lo, event);
                    st->busy += static_cast<u32>(e - c + 1);
                    c = e;
                    continue;
                }
                p.rect(cols[c].x0, hi, cols[c].x0 + p.lw, lo, event);
                p.rect(cols[c].x0 - p.lw, hi, cols[c].x0 + 2 * p.lw, hi + p.lw, event);
                ++st->edges;
            }
            continue;
        }

        if (digital) {
            u64 cur = entry;
            int run = 0;
            auto flush = [&](int x0, int x1) {
                x1 = std::min(x1, limit);
                if (x1 <= x0) return;
                switch (level(lane, cur, bit)) {
                    case 0: p.rect(x0, lo - p.lw, x1, lo, line); break;
                    case 1: p.rect(x0, hi, x1, hi + p.lw, line); break;
                    case 2: p.dashed(x0, x1, mid, lk.z); break;
                    case 3:
                        p.rect(x0, hi, x1, lo, lk.x, 0.3f);
                        p.rect(x0, hi, x1, hi + p.lw, lk.x);
                        p.rect(x0, lo - p.lw, x1, lo, lk.x);
                        break;
                    default: break;
                }
            };
            for (size_t c = 0; c < cols.size() && cols[c].x0 < limit; ++c) {
                const Col& col = cols[c];
                if (!col.c.count) continue;
                flush(run, col.x0);
                if (col.c.count == 1) {
                    const int was = level(lane, cur, bit), now = level(lane, col.c.first, bit);
                    if (was >= 0) p.rect(col.x0, hi, col.x0 + p.lw, lo, now == 3 ? lk.x : now == 2 ? lk.z : line);
                    cur = col.c.first;
                    run = col.x0;
                    ++st->edges;
                    continue;
                }
                size_t e = c;
                while (e + 1 < cols.size() && cols[e + 1].c.count > 1 && cols[e + 1].x0 < limit) ++e;
                p.busy(col.x0, hi, std::min(cols[e].x1, limit), lo, line);
                st->busy += static_cast<u32>(e - c + 1);
                cur = cols[e].c.first + cols[e].c.count - 1;
                run = cols[e].x1;
                c = e;
            }
            flush(run, kWaveTile);
            continue;
        }

        // A bus (or a real, or a string): runs of one value each, joined
        // where it changes by a cross that meets in the middle.
        struct Run {
            int x0, x1;
            u64 v;        // the value (~0: none yet)
            bool busy;
            bool edge0;   // it starts with a change (not at the tile's edge, nor after a busy band)
        };
        thread_local std::vector<Run> runs;
        runs.clear();
        {
            u64 cur = entry;
            int run = 0;
            bool edge = false;
            for (size_t c = 0; c < cols.size() && cols[c].x0 < limit; ++c) {
                const Col& col = cols[c];
                if (!col.c.count) continue;
                if (col.x0 > run) runs.push_back({run, col.x0, cur, false, edge});
                if (col.c.count == 1) {
                    cur = col.c.first;
                    run = col.x0;
                    edge = true;
                    ++st->edges;
                    continue;
                }
                size_t e = c;
                while (e + 1 < cols.size() && cols[e + 1].c.count > 1 && cols[e + 1].x0 < limit) ++e;
                runs.push_back({col.x0, cols[e].x1, 0, true, false});
                st->busy += static_cast<u32>(e - c + 1);
                cur = cols[e].c.first + cols[e].c.count - 1;
                run = cols[e].x1;
                edge = false;
                c = e;
            }
            if (std::min(kWaveTile, limit) > run) runs.push_back({run, std::min(kWaveTile, limit), cur, false, edge});
        }
        const int slant = std::max(1, static_cast<int>(std::lround(3 * lk.scale)));
        RasterLines strokes;  // the crosses where values change
        RasterLines xfill;    // the hexagons of x
        RasterPath labels;
        const float size = std::min(lk.font_px * 0.8f, font && font->ok() ? (lo - hi - 2) / std::max(0.5f, font->ascent(1) + font->descent(1)) : lk.font_px);
        const float base = font && font->ok() ? mid + (font->ascent(size) - font->descent(size)) / 2 : 0;
        auto shown = [&](const Run& u) { return !u.busy && u.v != ~0ull; };
        auto z_run = [&](const Run& u) {
            if (lane.kind() != LaneKind::kBits || !lane.has_xz()) return false;
            for (u32 b = 0; b < lane.width(); ++b)
                if (lane.bit(u.v, b) != Bit::kZ) return false;
            return true;
        };
        auto x_run = [&](const Run& u) {
            if (lane.kind() != LaneKind::kBits || !lane.has_xz()) return false;
            for (u32 b = 0; b < lane.width(); b += 64)
                if (lane.xz(u.v, b, std::min<u32>(64, lane.width() - b))) return true;
            return false;
        };
        for (size_t k = 0; k < runs.size(); ++k) {
            const Run& u = runs[k];
            if (u.busy) {
                p.busy(u.x0, hi, u.x1, lo, line);
                continue;
            }
            if (!shown(u)) continue;
            // How far in from each end the slants reach: half the run at most.
            const int half = std::max(0, (u.x1 - u.x0) / 2);
            const int s0 = u.edge0 ? std::min(slant, half) : 0;
            const bool edge1 = k + 1 < runs.size() && runs[k + 1].edge0;
            const int s1 = edge1 ? std::min(slant, half) : 0;
            const float fx0 = static_cast<float>(u.x0), fx1 = static_cast<float>(u.x1);
            const float fhi = static_cast<float>(hi) + p.lw / 2.0f, flo = static_cast<float>(lo) - p.lw / 2.0f, fmid = (fhi + flo) / 2;
            if (z_run(u)) {
                p.dashed(u.x0 + s0, u.x1 - s1, mid, lk.z);
                if (s0) strokes.begin(false), strokes.add(fx0, fmid), strokes.add(fx0 + s0, fmid);
                if (s1) strokes.begin(false), strokes.add(fx1 - s1, fmid), strokes.add(fx1, fmid);
                continue;
            }
            const bool x = x_run(u);
            const float* ink = x ? lk.x : line;
            p.rect(u.x0 + s0, hi, u.x1 - s1, hi + p.lw, ink);
            p.rect(u.x0 + s0, lo - p.lw, u.x1 - s1, lo, ink);
            if (s0) {
                strokes.begin(false), strokes.add(fx0, fmid), strokes.add(fx0 + s0, fhi);
                strokes.begin(false), strokes.add(fx0, fmid), strokes.add(fx0 + s0, flo);
            }
            if (s1) {
                strokes.begin(false), strokes.add(fx1 - s1, fhi), strokes.add(fx1, fmid);
                strokes.begin(false), strokes.add(fx1 - s1, flo), strokes.add(fx1, fmid);
            }
            if (x) {
                xfill.begin(true);
                xfill.add(fx0, fmid), xfill.add(fx0 + s0, fhi), xfill.add(fx1 - s1, fhi), xfill.add(fx1, fmid), xfill.add(fx1 - s1, flo), xfill.add(fx0 + s0, flo);
            }
            // Its value, where it has room: once in the run, whatever tiles
            // it crosses (each drawing its part), placed by the run alone so
            // that tiles painted apart and kept while panning agree: centred
            // in a run narrower than kWaveLabelEvery points, and in a wider
            // one at the middle of each whole stretch of that from its start.
            // A run in this tile alone is placed by its columns; one that
            // goes on past an edge by its times. The last run of a dump still
            // being read has no end yet: labelled only in its stretches read
            // whole, and the tile painted again as more comes while its
            // labels may still change.
            if (!font || !font->ok()) continue;
            const double every = kWaveLabelEvery * std::max(1.0f, lk.scale);
            const bool inside = u.x0 > 0 && u.x1 < std::min(kWaveTile, limit);
            double xs = u.x0, xe = u.x1, sa = s0, sb = s1;  // (the run, and how far its slants reach in)
            bool open = false;
            if (!inside) {
                xs = label_x(lane.time(u.v));
                open = u.v + 1 >= lane.size() && !w.done;
                xe = label_x(u.v + 1 < lane.size() ? lane.time(u.v + 1) : limit_t);
                sa = sb = slant;
                if (open && xe < kWaveTile + every) *again = true;
            }
            const double run_w = xe - xs, margin = 4.0 * lk.scale;
            if (run_w < every && (open || run_w - sa - sb - margin < size * 0.6)) continue;
            wave_value(lane, u.v, row.fmt, &text);
            if (text.empty()) continue;
            const double tw = font->width(text, size);
            auto put = [&](double cx) {
                if (cx + tw / 2 <= 0 || cx - tw / 2 >= kWaveTile) return;  // (another tile's)
                font->outline(text, static_cast<float>(cx - tw / 2), base, size, false, &labels);
                ++st->labels;
            };
            if (run_w < every) {
                if (tw <= run_w - sa - sb - margin) put((xs + sa + xe - sb) / 2);
                continue;
            }
            if (tw > every - 2.0 * slant - margin) continue;
            const double whole = std::floor(run_w / every);  // (the stretches of it read whole)
            const double first = std::max(0.0, std::floor((-tw - xs) / every));
            const double last = std::min(whole - 1, std::floor((kWaveTile + tw - xs) / every));
            for (double n = first; n <= last; ++n) put(xs + (n + 0.5) * every);
        }
        if (!xfill.pts.empty()) {
            float a[4] = {lk.x[0], lk.x[1], lk.x[2], lk.x[3] * 0.3f};
            out->fill(xfill, a);
        }
        if (!strokes.pts.empty()) {
            RasterStroke style;
            style.width = static_cast<float>(p.lw);
            style.cap = RasterStroke::kButt;
            out->fill(raster_stroke(strokes, style), line);
        }
        if (!labels.empty()) out->fill(raster_flatten(labels, 1, 0, 0), lk.text);
    }
}

WavePainter::WavePainter(std::function<void()> wake, unsigned threads) : wake_(std::move(wake)) {
    const unsigned n = threads ? threads : std::clamp(cores() / 2, 2u, 4u);
    for (unsigned k = 0; k < n; ++k) threads_.emplace_back([this](std::stop_token st) { run(st); });
}

WavePainter::~WavePainter() {
    for (std::jthread& t : threads_) t.request_stop();
    cv_.notify_all();
    for (std::jthread& t : threads_)
        if (t.joinable()) t.join();
}

void WavePainter::want(std::vector<WaveTileJob> jobs) {
    {
        std::lock_guard l(mu_);
        std::erase_if(jobs, [&](const WaveTileJob& j) {
            return std::ranges::find(painting_, j.key) != painting_.end() || std::ranges::any_of(done_, [&](const Done& d) { return d.job.key == j.key; });
        });
        jobs_.assign(std::make_move_iterator(jobs.begin()), std::make_move_iterator(jobs.end()));
    }
    cv_.notify_all();
}

std::vector<WavePainter::Done> WavePainter::take() {
    std::lock_guard l(mu_);
    std::vector<Done> out;
    out.swap(done_);
    return out;
}

bool WavePainter::busy() const {
    std::lock_guard l(mu_);
    return !jobs_.empty() || !painting_.empty() || !done_.empty();
}

void WavePainter::run(std::stop_token st) {
    thread_soon(Soon::kWaitedFor);
    DiagramFont font;
    for (;;) {
        WaveTileJob job;
        {
            std::unique_lock l(mu_);
            if (!cv_.wait(l, st, [&] { return !jobs_.empty(); })) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            painting_.push_back(job.key);
        }
        const u64 t0 = now_ns();
        const WaveLooks& lk = *job.looks;
        font.open(lk.font_path, lk.font_index, lk.fallbacks);
        RasterImage img(kWaveTile, std::max(1, static_cast<int>(job.rows.size()) * job.row_h));
        Done d;
        {
            std::shared_lock lock(job.wave->mu);
            wave_paint_tile(job, &font, &img, &d.upto, &d.stats, &d.again);
        }
        d.w = img.w(), d.h = img.h();
        d.rgba = std::move(img.pixels());
        d.ns = now_ns() - t0;
        const u64 key = job.key;
        d.job = std::move(job);
        if (st.stop_requested()) return;
        {
            std::lock_guard l(mu_);
            std::erase(painting_, key);
            done_.push_back(std::move(d));
        }
        if (wake_) wake_();
    }
}

}  // namespace blit
