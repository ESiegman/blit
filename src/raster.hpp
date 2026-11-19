#pragma once
// A small anti-aliased rasterizer: shapes filled into an RGBA picture by
// the exact area of each pixel they cover (non-zero winding), and lines
// stroked by turning them into outlines (joins, caps, dashes) that are
// filled the same way. What the Markdown preview's diagrams are drawn with
// (diagram.cpp): no library, no sine or cosine but its own, so that every
// system draws the same pixels.
//
// A shape is built as a RasterPath (lines, curves, arcs), flattened into
// RasterLines (figures of points, in pixels) and filled; a line is
// flattened, dashed if it is, stroked into outlines and filled. Points
// that are not numbers are left out; points far off the picture are
// brought in to a bound, and nothing is ever written outside it.
#include <vector>

#include "common.hpp"

namespace blit {

struct RasterPoint {
    float x = 0, y = 0;
};

// An outline: figures of lines, curves and arcs, each begun by move_to (or
// by the first thing drawn), in any units.
class RasterPath {
public:
    enum Verb : u8 { kMove, kLine, kQuad, kCubic, kClose };
    void move_to(float x, float y);
    void line_to(float x, float y);
    void quad_to(float cx, float cy, float x, float y);
    void cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y);
    // Part of the ellipse centred at (cx, cy) with radii rx and ry, from
    // angle a0 to a1 (radians; 0 is to the right, and angles grow
    // clockwise on the screen, y going down): joined by a line to where
    // the figure is, or beginning one.
    void arc(float cx, float cy, float rx, float ry, float a0, float a1);
    void close();
    // A rectangle, its corners rounded by r (at most half its shorter side).
    void rect(float x, float y, float w, float h, float r = 0);
    void ellipse(float cx, float cy, float rx, float ry);
    void polygon(const RasterPoint* p, size_t n);
    bool empty() const { return verbs.empty(); }

    std::vector<u8> verbs;
    std::vector<float> pts;  // each verb's points, two floats a point

private:
    bool open_ = false;  // a figure is begun
    float start_x_ = 0, start_y_ = 0, at_x_ = 0, at_y_ = 0;
};

// Figures of straight lines: figure k is pts[starts[k], starts[k + 1]) (the
// last to the end), closed or not.
struct RasterLines {
    std::vector<RasterPoint> pts;
    std::vector<u32> starts;
    std::vector<u8> closed;
    void begin(bool is_closed) {
        starts.push_back(static_cast<u32>(pts.size()));
        closed.push_back(is_closed);
    }
    void add(float x, float y) { pts.push_back({x, y}); }
    size_t figures() const { return starts.size(); }
    size_t end_of(size_t k) const { return k + 1 < starts.size() ? starts[k + 1] : pts.size(); }
};

// A path in pixels: each of its points times `scale`, moved by (dx, dy);
// curves cut into lines no further than `tolerance` pixels from them.
RasterLines raster_flatten(const RasterPath& path, float scale, float dx, float dy, float tolerance = 0.04f);

struct RasterStroke {
    enum : u8 { kButt, kRound, kSquare };  // caps
    enum : u8 { kMiter, kRoundJoin, kBevel };  // joins
    float width = 1;
    u8 cap = kButt, join = kRoundJoin;
    float miter = 4;  // a mitre longer than this many half widths is bevelled
};

// The parts of the figures a dash pattern leaves (lengths on, off, on,
// ..., in pixels), from `offset` into it.
RasterLines raster_dash(const RasterLines& lines, const float* pattern, size_t n, float offset = 0);
// The outlines of the figures drawn as lines: to be filled.
RasterLines raster_stroke(const RasterLines& lines, const RasterStroke& style);

// The sine and cosine of an angle, the same on every system (the C
// library's differ from one to another in their last bits).
void raster_sincos(double a, double* s, double* c);

// A picture, its colours multiplied by alpha (as the GPU's pictures are),
// clear to begin with.
class RasterImage {
public:
    RasterImage(int w, int h);
    int w() const { return w_; }
    int h() const { return h_; }
    std::vector<u8>& pixels() { return px_; }
    const std::vector<u8>& pixels() const { return px_; }
    // The figures filled (each closed) in a colour, r, g, b and alpha from 0
    // to 1 (not multiplied), over what is there.
    void fill(const RasterLines& lines, const float rgba[4]);

private:
    int w_ = 0, h_ = 0;
    std::vector<u8> px_;
    std::vector<float> acc_;  // (a fill's coverage, kept between fills)
};

}  // namespace blit
