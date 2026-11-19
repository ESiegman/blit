#include "raster.hpp"

#include <algorithm>
#include <cmath>

namespace blit {

namespace {

// Points are kept within this many pixels of the picture: far enough that
// nothing on it moves, near enough that no sum overflows.
constexpr float kBound = 1 << 20;
// No curve is cut into more lines than this, no figure dashed into more
// dashes, and no outline made of more points (a path of a million figures
// is as much as a picture of 4096 by 4096 pixels could show).
constexpr int kMostPieces = 512;
constexpr size_t kMostDashes = 100'000;
constexpr size_t kMostPoints = 8'000'000;
constexpr double kQuarter = 1.5707963267948966;

bool finite(float x, float y) { return std::isfinite(x) && std::isfinite(y); }

float bound(float v) { return std::clamp(v, -kBound, kBound); }

struct Vec {
    double x = 0, y = 0;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
Vec operator*(Vec a, double k) { return {a.x * k, a.y * k}; }
double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
double cross(Vec a, Vec b) { return a.x * b.y - a.y * b.x; }
double length(Vec a) { return std::sqrt(dot(a, a)); }
Vec unit(Vec a) {
    const double l = length(a);
    return l > 0 ? a * (1 / l) : Vec{1, 0};
}

}  // namespace

void raster_sincos(double a, double* s, double* c) {
    if (!std::isfinite(a) || std::fabs(a) > 1e9) {
        *s = 0, *c = 1;
        return;
    }
    // To within a quarter turn of 0 (the quarter in two parts, so that the
    // rest keeps its bits), then the series.
    const double q = std::floor(a / kQuarter + 0.5);
    const double r = (a - q * 1.5707963267341256) - q * 6.077100506506192e-11;
    const double r2 = r * r;
    double sn = 0, cs = 0, term_s = r, term_c = 1;
    for (int k = 1; k <= 10; ++k) {
        sn += term_s, cs += term_c;
        term_s *= -r2 / ((2.0 * k) * (2.0 * k + 1));
        term_c *= -r2 / ((2.0 * k - 1) * (2.0 * k));
    }
    switch (static_cast<long long>(q) & 3) {
        case 0: *s = sn, *c = cs; break;
        case 1: *s = cs, *c = -sn; break;
        case 2: *s = -sn, *c = -cs; break;
        default: *s = -cs, *c = sn; break;
    }
}

// ---------------------------------------------------------------- paths

void RasterPath::move_to(float x, float y) {
    verbs.push_back(kMove);
    pts.insert(pts.end(), {x, y});
    open_ = true;
    start_x_ = at_x_ = x, start_y_ = at_y_ = y;
}

void RasterPath::line_to(float x, float y) {
    if (!open_) return move_to(x, y);
    verbs.push_back(kLine);
    pts.insert(pts.end(), {x, y});
    at_x_ = x, at_y_ = y;
}

void RasterPath::quad_to(float cx, float cy, float x, float y) {
    if (!open_) move_to(at_x_, at_y_);
    verbs.push_back(kQuad);
    pts.insert(pts.end(), {cx, cy, x, y});
    at_x_ = x, at_y_ = y;
}

void RasterPath::cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!open_) move_to(at_x_, at_y_);
    verbs.push_back(kCubic);
    pts.insert(pts.end(), {c1x, c1y, c2x, c2y, x, y});
    at_x_ = x, at_y_ = y;
}

void RasterPath::arc(float cx, float cy, float rx, float ry, float a0, float a1) {
    if (!std::isfinite(a0) || !std::isfinite(a1)) return;
    double sweep = std::clamp(static_cast<double>(a1) - a0, -4 * 2 * kQuarter * 2, 4 * 2 * kQuarter * 2);
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / kQuarter - 1e-9)));
    const double step = sweep / n;
    double ts, tc;
    raster_sincos(step / 4, &ts, &tc);
    const double k = 4.0 / 3.0 * ts / tc;  // (the control points of a cubic that is an arc)
    double s, c;
    raster_sincos(a0, &s, &c);
    const float x0 = static_cast<float>(cx + rx * c), y0 = static_cast<float>(cy + ry * s);
    if (open_) line_to(x0, y0);
    else move_to(x0, y0);
    double a = a0;
    for (int i = 0; i < n; ++i) {
        double s1, c1;
        raster_sincos(a + step, &s1, &c1);
        cubic_to(static_cast<float>(cx + rx * (c - k * s)), static_cast<float>(cy + ry * (s + k * c)), static_cast<float>(cx + rx * (c1 + k * s1)),
                 static_cast<float>(cy + ry * (s1 - k * c1)), static_cast<float>(cx + rx * c1), static_cast<float>(cy + ry * s1));
        a += step, s = s1, c = c1;
    }
}

void RasterPath::close() {
    if (!open_) return;
    verbs.push_back(kClose);
    open_ = false;
    at_x_ = start_x_, at_y_ = start_y_;
}

void RasterPath::rect(float x, float y, float w, float h, float r) {
    if (!(w > 0) || !(h > 0)) return;
    r = std::clamp(r, 0.0f, std::min(w, h) / 2);
    if (!(r > 0)) {
        move_to(x, y), line_to(x + w, y), line_to(x + w, y + h), line_to(x, y + h), close();
        return;
    }
    const float k = r * 0.44771525f;  // (1 - 0.5523: the control points of a quarter circle, from the corner)
    move_to(x + r, y);
    line_to(x + w - r, y);
    cubic_to(x + w - k, y, x + w, y + k, x + w, y + r);
    line_to(x + w, y + h - r);
    cubic_to(x + w, y + h - k, x + w - k, y + h, x + w - r, y + h);
    line_to(x + r, y + h);
    cubic_to(x + k, y + h, x, y + h - k, x, y + h - r);
    line_to(x, y + r);
    cubic_to(x, y + k, x + k, y, x + r, y);
    close();
}

void RasterPath::ellipse(float cx, float cy, float rx, float ry) {
    const float kx = rx * 0.55228475f, ky = ry * 0.55228475f;
    move_to(cx + rx, cy);
    cubic_to(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    cubic_to(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    cubic_to(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    cubic_to(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    close();
}

void RasterPath::polygon(const RasterPoint* p, size_t n) {
    if (n == 0) return;
    move_to(p[0].x, p[0].y);
    for (size_t i = 1; i < n; ++i) line_to(p[i].x, p[i].y);
    close();
}

RasterLines raster_flatten(const RasterPath& path, float scale, float dx, float dy, float tolerance) {
    RasterLines out;
    if (!std::isfinite(scale) || !std::isfinite(dx) || !std::isfinite(dy)) return out;
    tolerance = std::isfinite(tolerance) ? std::max(tolerance, 0.01f) : 0.15f;
    bool open = false;
    float px = 0, py = 0;  // the pen, in pixels
    auto at = [&](size_t i, float* x, float* y) {
        *x = path.pts[i] * scale + dx, *y = path.pts[i + 1] * scale + dy;
        return finite(*x, *y);
    };
    auto put = [&](float x, float y) {
        if (out.pts.size() >= kMostPoints) return;
        if (!open) {
            out.begin(false);
            open = true;
        }
        out.add(bound(x), bound(y));
        px = x, py = y;
    };
    size_t i = 0;
    for (const u8 v : path.verbs) {
        switch (v) {
            case RasterPath::kMove: {
                float x, y;
                if (at(i, &x, &y)) {
                    open = false;
                    put(x, y);
                }
                i += 2;
                break;
            }
            case RasterPath::kLine: {
                float x, y;
                if (at(i, &x, &y)) put(x, y);
                i += 2;
                break;
            }
            case RasterPath::kQuad: {
                float cx, cy, x, y;
                if (at(i, &cx, &cy) && at(i + 2, &x, &y)) {
                    if (!open) put(px, py);
                    const double ddx = px - 2.0 * cx + x, ddy = py - 2.0 * cy + y;
                    const double dd = std::sqrt(ddx * ddx + ddy * ddy);
                    const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.25 * dd / tolerance))), 1, kMostPieces);
                    const float x0 = px, y0 = py;
                    for (int k = 1; k <= n; ++k) {
                        const double t = static_cast<double>(k) / n, u = 1 - t;
                        put(static_cast<float>(u * u * x0 + 2 * u * t * cx + t * t * x), static_cast<float>(u * u * y0 + 2 * u * t * cy + t * t * y));
                    }
                }
                i += 4;
                break;
            }
            case RasterPath::kCubic: {
                float c1x, c1y, c2x, c2y, x, y;
                if (at(i, &c1x, &c1y) && at(i + 2, &c2x, &c2y) && at(i + 4, &x, &y)) {
                    if (!open) put(px, py);
                    const double ax = px - 2.0 * c1x + c2x, ay = py - 2.0 * c1y + c2y;
                    const double bx = c1x - 2.0 * c2x + x, by = c1y - 2.0 * c2y + y;
                    const double dd = std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by));
                    const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75 * dd / tolerance))), 1, kMostPieces);
                    const float x0 = px, y0 = py;
                    for (int k = 1; k <= n; ++k) {
                        const double t = static_cast<double>(k) / n, u = 1 - t;
                        const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
                        put(static_cast<float>(a * x0 + b * c1x + c * c2x + d * x), static_cast<float>(a * y0 + b * c1y + c * c2y + d * y));
                    }
                }
                i += 6;
                break;
            }
            case RasterPath::kClose:
                if (open) {
                    out.closed.back() = 1;
                    const size_t first = out.starts.back();
                    px = out.pts[first].x, py = out.pts[first].y;
                }
                open = false;
                break;
            default: return out;
        }
    }
    return out;
}

// ---------------------------------------------------------------- dashes and strokes

RasterLines raster_dash(const RasterLines& lines, const float* pattern, size_t n, float offset) {
    double sum = 0;
    for (size_t k = 0; k < n; ++k) {
        if (!(pattern[k] >= 0) || !std::isfinite(pattern[k])) return lines;
        sum += pattern[k];
    }
    if (n == 0 || !(sum > 0.05)) return lines;
    // (far too many dashes to see: drawn whole)
    double all = 0;
    for (size_t f = 0; f < lines.figures(); ++f)
        for (size_t i = lines.starts[f] + 1; i < lines.end_of(f) + (lines.closed[f] ? 1 : 0); ++i) {
            const RasterPoint a = lines.pts[i - 1], b = lines.pts[i < lines.end_of(f) ? i : lines.starts[f]];
            all += length(Vec{b.x - a.x, b.y - a.y});
        }
    if (all / sum * static_cast<double>(n) > kMostDashes) return lines;
    RasterLines out;
    for (size_t f = 0; f < lines.figures(); ++f) {
        const size_t s = lines.starts[f], e = lines.end_of(f);
        if (e <= s) continue;
        size_t k = 0;
        double left = std::fmod(std::isfinite(offset) ? std::fabs(offset) : 0.0, sum);
        while (left > pattern[k]) left -= pattern[k], k = (k + 1) % n;
        left = pattern[k] - left;  // what is left of the dash (or gap) begun
        bool on = k % 2 == 0;
        if (on) out.begin(false), out.add(lines.pts[s].x, lines.pts[s].y);
        const size_t last = e + (lines.closed[f] ? 1 : 0);
        for (size_t i = s + 1; i < last; ++i) {
            const RasterPoint a = lines.pts[i - 1], b = lines.pts[i < e ? i : s];
            const double seg = length(Vec{b.x - a.x, b.y - a.y});
            double done = 0;
            while (seg - done > left) {
                done += left;
                const float t = static_cast<float>(done / seg);
                const float x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t;
                if (on) out.add(x, y);
                k = (k + 1) % n;
                on = k % 2 == 0;
                left = pattern[k];
                if (on) out.begin(false), out.add(x, y);
            }
            left -= seg - done;
            if (on) out.add(b.x, b.y);
        }
    }
    return out;
}

namespace {

struct Stroker {
    double hw;
    const RasterStroke& st;
    double tol = 0.02;
    std::vector<Vec> ring;  // the outline being made

    // Points of the circle of radius hw round v, between unit vectors a and
    // b (less than half a turn apart), neither of them.
    void arc(Vec v, Vec a, Vec b, int depth = 0) {
        const double d = dot(a, b);
        if (depth > 12 || hw * (1 - std::sqrt(std::max(0.0, (1 + d) / 2))) < tol) return;
        const Vec m = unit(a + b);
        arc(v, a, m, depth + 1);
        ring.push_back(v + m * hw);
        arc(v, m, b, depth + 1);
    }
    // A join at v on the side of the normals (na coming in, nb going out).
    void join(Vec v, Vec da, Vec db) {
        const Vec na{-da.y, da.x}, nb{-db.y, db.x};
        const double c = cross(da, db), d = dot(da, db);
        if (std::fabs(c) < 1e-9 && d > 0) {
            ring.push_back(v + na * hw);
            return;
        }
        if (c > 0) {  // the inside of the turn: through the middle (non-zero fills the rest)
            ring.push_back(v + na * hw);
            ring.push_back(v);
            ring.push_back(v + nb * hw);
            return;
        }
        ring.push_back(v + na * hw);
        if (st.join == RasterStroke::kMiter) {
            const Vec m = na + nb;
            const double mm = dot(m, m);
            if (mm > 1e-12 && 2 / std::sqrt(mm) <= st.miter) ring.push_back(v + m * (2 * hw / mm));
        } else if (st.join == RasterStroke::kRoundJoin) {
            if (d < -0.999999) {  // (back the way it came: round the front)
                arc(v, na, da);
                ring.push_back(v + da * hw);
                arc(v, da, nb);
            } else {
                arc(v, na, nb);
            }
        }
        ring.push_back(v + nb * hw);
    }
    // One side of an open line, then the cap at its end.
    void side(const std::vector<Vec>& q) {
        const size_t m = q.size();
        Vec dprev = unit(q[1] - q[0]);
        ring.push_back(q[0] + Vec{-dprev.y, dprev.x} * hw);
        for (size_t i = 1; i + 1 < m; ++i) {
            const Vec d = unit(q[i + 1] - q[i]);
            join(q[i], dprev, d);
            dprev = d;
        }
        const Vec e = q[m - 1], n{-dprev.y, dprev.x};
        ring.push_back(e + n * hw);
        if (st.cap == RasterStroke::kSquare) {
            ring.push_back(e + n * hw + dprev * hw);
            ring.push_back(e - n * hw + dprev * hw);
        } else if (st.cap == RasterStroke::kRound) {
            arc(e, n, dprev);
            ring.push_back(e + dprev * hw);
            arc(e, dprev, Vec{} - n);
        }
    }
    // One side of a closed line, all the way round.
    void loop(const std::vector<Vec>& q) {
        const size_t m = q.size();
        for (size_t i = 0; i < m; ++i) {
            const Vec a = q[(i + m - 1) % m], v = q[i], b = q[(i + 1) % m];
            join(v, unit(v - a), unit(b - v));
        }
    }
    void flush(RasterLines& out) {
        if (ring.size() >= 3 && out.pts.size() + ring.size() < kMostPoints) {
            out.begin(true);
            for (const Vec& p : ring) out.add(bound(static_cast<float>(p.x)), bound(static_cast<float>(p.y)));
        }
        ring.clear();
    }
};

}  // namespace

RasterLines raster_stroke(const RasterLines& lines, const RasterStroke& style) {
    RasterLines out;
    if (!(style.width > 0) || !std::isfinite(style.width)) return out;
    Stroker sk{std::min(static_cast<double>(style.width), 1e5) / 2, style, 0.02, {}};
    std::vector<Vec> q;
    for (size_t f = 0; f < lines.figures(); ++f) {
        q.clear();
        for (size_t i = lines.starts[f]; i < lines.end_of(f); ++i) {
            const Vec p{lines.pts[i].x, lines.pts[i].y};
            if (q.empty() || length(p - q.back()) > 1e-3) q.push_back(p);
        }
        const bool closed = lines.closed[f] != 0;
        while (closed && q.size() > 1 && length(q.back() - q.front()) <= 1e-3) q.pop_back();
        if (q.empty()) continue;
        if (q.size() == 1) {  // a dot: what its cap makes of it
            const Vec c = q[0];
            if (style.cap == RasterStroke::kRound) {
                sk.ring.push_back(c + Vec{sk.hw, 0});
                sk.arc(c, {1, 0}, {0, 1});
                sk.ring.push_back(c + Vec{0, sk.hw});
                sk.arc(c, {0, 1}, {-1, 0});
                sk.ring.push_back(c + Vec{-sk.hw, 0});
                sk.arc(c, {-1, 0}, {0, -1});
                sk.ring.push_back(c + Vec{0, -sk.hw});
                sk.arc(c, {0, -1}, {1, 0});
            } else if (style.cap == RasterStroke::kSquare) {
                sk.ring = {c + Vec{-sk.hw, -sk.hw}, c + Vec{sk.hw, -sk.hw}, c + Vec{sk.hw, sk.hw}, c + Vec{-sk.hw, sk.hw}};
            }
            sk.flush(out);
            continue;
        }
        if (closed && q.size() > 2) {
            sk.loop(q);
            sk.flush(out);
            std::reverse(q.begin(), q.end());
            sk.loop(q);
            sk.flush(out);
            continue;
        }
        sk.side(q);
        std::reverse(q.begin(), q.end());
        sk.side(q);
        sk.flush(out);
    }
    return out;
}

// ---------------------------------------------------------------- filling

RasterImage::RasterImage(int w, int h) : w_(std::clamp(w, 0, 1 << 14)), h_(std::clamp(h, 0, 1 << 14)) {
    px_.assign(static_cast<size_t>(w_) * static_cast<size_t>(h_) * 4, 0);
}

namespace {

// The signed area each line adds to the cells it crosses (and, through
// the running sum along a row, to those right of it): cell (x, y) is
// acc[y * stride + x], for x from 0 to w + 1.
struct Coverage {
    float* acc;
    int w, h;
    size_t stride;

    // A line wholly within 0 <= x <= w.
    void inside(double x0, double y0, double x1, double y1) {
        if (y0 == y1) return;
        double dir = 1;
        if (y0 > y1) std::swap(x0, x1), std::swap(y0, y1), dir = -1;
        if (y1 <= 0 || y0 >= h) return;
        const double dxdy = (x1 - x0) / (y1 - y0);
        double x = y0 < 0 ? x0 - y0 * dxdy : x0;
        const int ys = std::max(0, static_cast<int>(std::floor(y0))), ye = std::min(h, static_cast<int>(std::ceil(y1)));
        for (int y = ys; y < ye; ++y) {
            float* row = acc + static_cast<size_t>(y) * stride;
            const double dy = std::min(y + 1.0, y1) - std::max(static_cast<double>(y), y0);
            const double xn = std::clamp(x + dxdy * dy, 0.0, static_cast<double>(w));
            x = std::clamp(x, 0.0, static_cast<double>(w));
            const double d = dy * dir;
            const double xa = std::min(x, xn), xb = std::max(x, xn);
            const double xaf = std::floor(xa), xbc = std::ceil(xb);
            const int xai = static_cast<int>(xaf), xbi = static_cast<int>(xbc);
            if (xbi <= xai + 1) {
                const double xmf = 0.5 * (x + xn) - xaf;
                row[xai] += static_cast<float>(d - d * xmf);
                row[xai + 1] += static_cast<float>(d * xmf);
            } else {
                const double s = 1 / (xb - xa);
                const double x0f = xa - xaf;
                const double a0 = 0.5 * s * (1 - x0f) * (1 - x0f);
                const double x1f = xb - xbc + 1;
                const double am = 0.5 * s * x1f * x1f;
                row[xai] += static_cast<float>(d * a0);
                if (xbi == xai + 2) {
                    row[xai + 1] += static_cast<float>(d * (1 - a0 - am));
                } else {
                    const double a1 = s * (1.5 - x0f);
                    row[xai + 1] += static_cast<float>(d * (a1 - a0));
                    for (int xi = xai + 2; xi < xbi - 1; ++xi) row[xi] += static_cast<float>(d * s);
                    const double a2 = a1 + (xbi - xai - 3) * s;
                    row[xbi - 1] += static_cast<float>(d * (1 - a2 - am));
                }
                row[xbi] += static_cast<float>(d * am);
            }
            x = xn;
        }
    }
    // Any line: the parts of it left of the cells are a line down their
    // left edge (all of each row right of it is inside), the parts right
    // of them one down their right edge.
    void line(double x0, double y0, double x1, double y1) {
        if (y0 == y1) return;
        double cut[4] = {0, 0, 0, 1};
        int n = 1;
        for (const double edge : {0.0, static_cast<double>(w)})
            if ((x0 - edge) * (x1 - edge) < 0) cut[n++] = (edge - x0) / (x1 - x0);
        cut[n] = 1;
        if (n == 3 && cut[1] > cut[2]) std::swap(cut[1], cut[2]);
        for (int k = 0; k < n; ++k) {
            const double ta = cut[k], tb = cut[k + 1];
            const double xa = x0 + (x1 - x0) * ta, ya = y0 + (y1 - y0) * ta;
            const double xb = x0 + (x1 - x0) * tb, yb = y0 + (y1 - y0) * tb;
            const double xm = (xa + xb) / 2;
            const double edge = xm <= 0 ? 0 : xm >= w ? w : -1;
            if (edge >= 0) inside(edge, ya, edge, yb);
            else inside(xa, ya, xb, yb);
        }
    }
};

}  // namespace

void RasterImage::fill(const RasterLines& lines, const float rgba[4]) {
    if (!(rgba[3] > 0) || lines.pts.empty() || w_ == 0 || h_ == 0) return;
    float x0 = kBound, y0 = kBound, x1 = -kBound, y1 = -kBound;
    for (const RasterPoint& p : lines.pts) {
        if (!finite(p.x, p.y)) continue;
        x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    }
    const int bx0 = std::clamp(static_cast<int>(std::floor(x0)), 0, w_), by0 = std::clamp(static_cast<int>(std::floor(y0)), 0, h_);
    const int bx1 = std::clamp(static_cast<int>(std::ceil(x1)), 0, w_), by1 = std::clamp(static_cast<int>(std::ceil(y1)), 0, h_);
    if (bx0 >= bx1 || by0 >= by1) return;
    Coverage cv;
    cv.w = bx1 - bx0, cv.h = by1 - by0;
    cv.stride = static_cast<size_t>(cv.w) + 2;
    acc_.assign(cv.stride * static_cast<size_t>(cv.h), 0.0f);
    cv.acc = acc_.data();
    for (size_t f = 0; f < lines.figures(); ++f) {
        const size_t s = lines.starts[f], e = lines.end_of(f);
        for (size_t i = s; i < e; ++i) {
            const RasterPoint a = lines.pts[i], b = lines.pts[i + 1 < e ? i + 1 : s];
            if (!finite(a.x, a.y) || !finite(b.x, b.y)) continue;
            cv.line(static_cast<double>(a.x) - bx0, static_cast<double>(a.y) - by0, static_cast<double>(b.x) - bx0, static_cast<double>(b.y) - by0);
        }
    }
    const float a = std::min(1.0f, rgba[3]);
    const float pr = std::clamp(rgba[0], 0.0f, 1.0f) * a, pg = std::clamp(rgba[1], 0.0f, 1.0f) * a, pb = std::clamp(rgba[2], 0.0f, 1.0f) * a;
    for (int y = 0; y < cv.h; ++y) {
        const float* row = acc_.data() + static_cast<size_t>(y) * cv.stride;
        u8* px = px_.data() + (static_cast<size_t>(by0 + y) * static_cast<size_t>(w_) + static_cast<size_t>(bx0)) * 4;
        float sum = 0;
        for (int x = 0; x < cv.w; ++x, px += 4) {
            sum += row[x];
            const float c = std::min(1.0f, std::fabs(sum));
            if (c < 1.0f / 512) continue;
            const float keep = 1 - a * c;
            px[0] = static_cast<u8>(std::min(255.0f, pr * c * 255 + px[0] * keep + 0.5f));
            px[1] = static_cast<u8>(std::min(255.0f, pg * c * 255 + px[1] * keep + 0.5f));
            px[2] = static_cast<u8>(std::min(255.0f, pb * c * 255 + px[2] * keep + 0.5f));
            px[3] = static_cast<u8>(std::min(255.0f, a * c * 255 + px[3] * keep + 0.5f));
        }
    }
}

}  // namespace blit
