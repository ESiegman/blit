// Mindmaps, laid out as a balanced tree from the root, for the Markdown
// preview's pictures (diagram.hpp). The root's branches are shared between
// its two sides (the first to the right, by how tall each is, until half
// of them are there), and each grows outwards, a node's children stacked
// beside it and centred on it, joined to it by curves in the branch's
// colour. A mindmap too wide for the room keeps the root in the middle and
// hangs each branch's nodes under it instead, indented away from the root
// (as wide as its deepest label and no more); only one too wide even so
// grows to one side. Nested by indentation, as Mermaid's are.
#include <algorithm>
#include <cmath>

#include "diagram.hpp"

namespace blit {

namespace {

enum Shape : u8 { kPlain, kSquare, kRounded, kCircle, kBang, kCloud, kHexagon };

struct Node {
    std::vector<std::string> text;  // its lines (wrapped)
    std::string label;
    u8 shape = kPlain;
    int parent = -1, depth = 0, branch = 0, indent = 0;
    std::vector<int> kids;
    float w = 0, h = 0, tw = 0, th = 0;
    float x = 0, y = 0;  // its centre
    float tall = 0;      // its subtree's height
    int side = 1;        // 1: right of the root, -1: left
};

bool parse(const std::vector<DiagramLine>& lines, std::vector<Node>& nodes, std::string* err) {
    for (size_t k = 1; k < lines.size(); ++k) {
        const std::string& raw = lines[k].text;
        auto fail = [&](const std::string& why) {
            *err = "line " + std::to_string(lines[k].no) + ": " + why;
            return false;
        };
        int indent = 0;
        size_t at = 0;
        for (; at < raw.size() && (raw[at] == ' ' || raw[at] == '\t'); ++at) indent += raw[at] == '\t' ? 4 : 1;
        std::string_view s = diagram_trim(std::string_view(raw).substr(at));
        if (s.starts_with("::icon(")) continue;  // (icons are not drawn)
        if (const size_t c = s.find(":::"); c != std::string_view::npos) s = diagram_trim(s.substr(0, c));
        if (s.empty()) continue;
        Node n;
        n.indent = indent;
        // id(text), id[text], id((text)), id))text((, id)text(, id{{text}}; or text alone.
        struct Delim {
            const char *open, *close;
            u8 shape;
        };
        static const Delim kDelims[] = {{"((", "))", kCircle}, {"))", "((", kBang}, {"{{", "}}", kHexagon}, {"(", ")", kRounded}, {")", "(", kCloud}, {"[", "]", kSquare}};
        const size_t open = s.find_first_of("([{)");
        n.label = std::string(s);
        if (open != std::string_view::npos && s.substr(0, open).find_first_of("\" \t") == std::string_view::npos) {  // (an id has no spaces)
            for (const Delim& dl : kDelims) {
                const std::string_view o = dl.open, c = dl.close;
                if (s.substr(open).starts_with(o) && s.size() >= open + o.size() + c.size() && s.ends_with(c)) {
                    n.shape = dl.shape;
                    n.label = std::string(s.substr(open + o.size(), s.size() - open - o.size() - c.size()));
                    break;
                }
            }
        }
        if (nodes.size() >= 2000) return fail("too many nodes to draw");
        if (!nodes.empty()) {
            // Its parent: the nearest node above it that is less indented.
            int p = static_cast<int>(nodes.size()) - 1;
            while (p >= 0 && nodes[static_cast<size_t>(p)].indent >= indent) p = nodes[static_cast<size_t>(p)].parent;
            if (p < 0) return fail("a mindmap has only one root (\"" + std::string(s.substr(0, 24)) + "\" is not under it)");
            n.parent = p;
            n.depth = nodes[static_cast<size_t>(p)].depth + 1;
            n.branch = n.depth == 1 ? static_cast<int>(nodes[0].kids.size()) : nodes[static_cast<size_t>(p)].branch;
            nodes[static_cast<size_t>(p)].kids.push_back(static_cast<int>(nodes.size()));
        }
        nodes.push_back(std::move(n));
    }
    if (nodes.empty()) {
        *err = "nothing in it";
        return false;
    }
    return true;
}

struct Look {
    float u, size, root_size, line, root_line, most, widest, px, py;  // (most: what labels are wrapped to; widest: at the widest)
    bool hang = false;  // a branch's nodes under it, indented, not beside it
    float hgap(int depth) const { return (depth == 0 ? (hang ? 40 : 52) : 34) * u; }
    float vgap(int depth) const { return (depth == 0 ? 18 : 9) * u; }
    float indent() const { return 22 * u; }  // (how far in a node hung under another is; how far under)
    float drop() const { return 7 * u; }
    bool under(int depth) const { return hang && depth >= 1; }  // whether a node this deep has its children under it
};

void measure(std::vector<Node>& nodes, const Look& lk, const DiagramFont& font) {
    for (Node& n : nodes) {
        const bool root = n.parent < 0;
        const float size = root ? lk.root_size : lk.size;
        // (narrower, but never so narrow that a word is broken that would fit at the widest)
        const std::vector<std::string> lines = diagram_label(n.label);
        float word = 0;
        for (const std::string& l : lines)
            for (size_t at = 0; at <= l.size();) {
                const size_t sp = std::min(l.find(' ', at), l.size());
                word = std::max(word, font.width(std::string_view(l).substr(at, sp - at), size, root));
                at = sp + 1;
            }
        n.text = diagram_wrap(lines, std::max(lk.most, std::min(word, lk.widest)), size, font);
        n.tw = 0;
        for (const std::string& l : n.text) n.tw = std::max(n.tw, font.width(l, size, root));
        n.th = static_cast<float>(n.text.size()) * (root ? lk.root_line : lk.line);
        const float px = lk.px * (root ? 1.4f : 1), py = lk.py * (root ? 1.4f : 1);
        DiagramVec s;
        switch (n.shape) {
            case kCircle: {
                // (round its text's corners, and nearly as far from them as a box's sides are from its text)
                const float across = std::sqrt(n.tw * n.tw + n.th * n.th) + 1.2f * px;
                s = {across, across};
                break;
            }
            case kHexagon: s = diagram_shape_size(kShapeHexagon, n.tw, n.th, px, py); break;
            case kBang:
            case kCloud: s = {n.tw * 1.25f + 3 * px, n.th * 1.3f + 4 * py}; break;
            default: s = {n.tw + 2 * px, n.th + 2 * py}; break;
        }
        n.w = s.x, n.h = s.y;
    }
}

// Each subtree's height: the last node first (a node's children come after
// it), not by recursion (a chain of 2,000 would be as deep on the
// preview's thread).
void subtrees(std::vector<Node>& nodes, const Look& lk) {
    for (size_t i = nodes.size(); i-- > 0;) {
        Node& n = nodes[i];
        float kids = 0;
        if (lk.under(n.depth)) {
            for (const int c : n.kids) kids += lk.drop() + nodes[static_cast<size_t>(c)].tall;
            n.tall = n.h + kids;
            continue;
        }
        for (size_t k = 0; k < n.kids.size(); ++k) kids += nodes[static_cast<size_t>(n.kids[k])].tall + (k ? lk.vgap(n.depth) : 0);
        n.tall = std::max(n.h, kids);
    }
}

// A node's children beside it, on its side, stacked and centred on it; or
// under it, each further from the root than it by the indent.
void place_kids(std::vector<Node>& nodes, int i, const std::vector<int>& kids, int side, const Look& lk) {
    const Node& n = nodes[static_cast<size_t>(i)];
    if (lk.under(n.depth)) {
        float y = n.y + n.h / 2;
        const float edge = n.x - static_cast<float>(side) * (n.w / 2 - lk.indent());
        for (const int c : kids) {
            Node& kid = nodes[static_cast<size_t>(c)];
            kid.side = side;
            kid.x = edge + static_cast<float>(side) * kid.w / 2;
            kid.y = y + lk.drop() + kid.h / 2;
            y += lk.drop() + kid.tall;
        }
        return;
    }
    float total = 0;
    for (size_t k = 0; k < kids.size(); ++k) total += nodes[static_cast<size_t>(kids[k])].tall + (k ? lk.vgap(n.depth) : 0);
    float y = n.y - total / 2;
    const float edge = n.x + static_cast<float>(side) * (n.w / 2 + lk.hgap(n.depth));
    for (const int c : kids) {
        Node& kid = nodes[static_cast<size_t>(c)];
        kid.side = side;
        kid.x = edge + static_cast<float>(side) * kid.w / 2;
        kid.y = y + (lk.under(kid.depth) ? kid.h : kid.tall) / 2;  // (a branch hung under its first node: that node at its top)
        y += kid.tall + lk.vgap(n.depth);
    }
}

// Laid out with the root's branches on both sides, or to the right only
// (beside each other or hung, as the look says); how wide that is.
float place(std::vector<Node>& nodes, bool both, const Look& lk) {
    subtrees(nodes, lk);
    Node& root = nodes[0];
    root.x = root.y = 0;
    std::vector<int> right, left;
    float total = 0, so_far = 0;
    for (const int c : root.kids) total += nodes[static_cast<size_t>(c)].tall;
    for (const int c : root.kids) {
        const float t = nodes[static_cast<size_t>(c)].tall;
        if (!both || right.empty() || so_far + t / 2 <= total / 2) right.push_back(c), so_far += t;
        else left.push_back(c);
    }
    place_kids(nodes, 0, right, 1, lk);
    place_kids(nodes, 0, left, -1, lk);
    // (each node is placed before its children: in order, not by recursion)
    for (size_t i = 1; i < nodes.size(); ++i) place_kids(nodes, static_cast<int>(i), nodes[i].kids, nodes[i].side, lk);
    float lo = 1e30f, hi = -1e30f;
    for (const Node& n : nodes) lo = std::min(lo, n.x - n.w / 2), hi = std::max(hi, n.x + n.w / 2);
    return hi - lo;
}

void outline(const Node& n, float u, RasterPath* p) {
    const float x = n.x - n.w / 2, y = n.y - n.h / 2;
    switch (n.shape) {
        case kSquare: p->rect(x, y, n.w, n.h, 1.5f * u); break;
        case kRounded: p->rect(x, y, n.w, n.h, std::min(n.h / 2, 10 * u)); break;
        case kCircle: diagram_shape(kShapeCircle, x, y, n.w, n.h, u, p); break;
        case kHexagon: diagram_shape(kShapeHexagon, x, y, n.w, n.h, u, p); break;
        case kBang: {
            // Spikes about an ellipse, their tips at its sides.
            const int spikes = 14;
            std::vector<RasterPoint> pts;
            for (int k = 0; k < spikes * 2; ++k) {
                double s = 0, c = 0;
                raster_sincos(3.14159265358979 * k / spikes, &s, &c);
                const float r = k % 2 ? 0.74f : 1.0f;
                pts.push_back({n.x + static_cast<float>(c) * n.w / 2 * r, n.y + static_cast<float>(s) * n.h / 2 * r});
            }
            p->polygon(pts.data(), pts.size());
            break;
        }
        case kCloud: {
            // Bumps about an ellipse, one at each side.
            const int bumps = 10;
            auto at = [&](double a, float r) {
                double s = 0, c = 0;
                raster_sincos(a, &s, &c);
                return RasterPoint{n.x + static_cast<float>(c) * n.w / 2 * r, n.y + static_cast<float>(s) * n.h / 2 * r};
            };
            const double step = 2 * 3.14159265358979 / bumps;
            const RasterPoint first = at(-step / 2, 0.8f);
            p->move_to(first.x, first.y);
            for (int k = 0; k < bumps; ++k) {
                const RasterPoint c = at(step * k, 1.2f), e = at(step * k + step / 2, 0.8f);
                p->quad_to(c.x, c.y, e.x, e.y);
            }
            p->close();
            break;
        }
        default: p->rect(x, y, n.w, n.h, std::min(n.h / 2, 6 * u)); break;
    }
}

// Where a line to or from the node meets its side (the side towards
// `side`), `up` of the way from its middle to its top or its foot (-1 to
// 1: a node's branches leave it apart from each other, not from one
// point). A round node's side is nearer its middle away from its middle
// height.
DiagramVec side_point(const Node& n, int side, float up = 0) {
    const float in = n.shape == kCloud ? 0.03f : n.shape == kBang ? 0.01f : 0;
    if (n.shape == kHexagon || n.shape == kBang || n.shape == kCloud) up = 0;  // (these come to a point at the side, or are not smooth)
    const float t = up * 0.6f;  // (within the middle 60% of its height)
    const float across = n.shape == kCircle ? std::sqrt(1 - t * t) : 1;
    return {n.x + static_cast<float>(side) * n.w / 2 * (1 - in) * across, n.y + t * n.h / 2};
}

}  // namespace

bool mindmap_layout(const std::vector<DiagramLine>& lines, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    std::vector<Node> nodes;
    if (!parse(lines, nodes, err)) return false;
    const DiagramFont& font = *style.font;
    const DiagramPalette& p = style.palette;
    const bool dark = p.dark();
    Look lk;
    lk.u = style.unit(), lk.size = style.size, lk.root_size = style.size * 1.15f;
    lk.line = font.line(lk.size), lk.root_line = font.line(lk.root_size);
    lk.px = 13 * lk.u, lk.py = 8 * lk.u;
    // The first of these that fits the room: on both sides, each node's
    // children beside it; on both sides, a branch's nodes hung under it;
    // the same, drawn a little smaller (the root in the middle is worth
    // that); to the right only, hung. Each with long labels wrapped
    // narrower while it is still too wide. The narrowest of them if none
    // fits (it is drawn as much smaller as it must be).
    const float most = std::max(font.width("abcdefghijklmnopqrstuvwxyz", lk.size) * 0.85f, 150 * lk.u);
    {
        struct Way {
            bool both, hang;
            float smaller;
        };
        std::vector<Node> best;
        float best_w = 1e30f;
        bool best_hang = false, fits = false;
        for (const Way way : {Way{true, false, 1}, Way{true, true, 1}, Way{true, true, 0.8f}, Way{false, true, 1}}) {
            for (const float k : {1.0f, 0.7f, 0.5f}) {
                lk.most = std::max(most * k, 80 * lk.u), lk.widest = most, lk.hang = way.hang;
                std::vector<Node> tried = nodes;
                measure(tried, lk, font);
                const float wide = place(tried, way.both, lk);
                fits = wide * way.smaller <= style.room - 28 * lk.u;
                if (fits || wide < best_w) best = std::move(tried), best_w = wide, best_hang = way.hang;
                if (fits) break;
            }
            if (fits) break;
        }
        nodes = std::move(best), lk.hang = best_hang;
    }

    const float u = lk.u, pad = 14 * u;
    float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
    for (const Node& n : nodes) lx = std::min(lx, n.x - n.w / 2), ly = std::min(ly, n.y - n.h / 2), hx = std::max(hx, n.x + n.w / 2), hy = std::max(hy, n.y + n.h / 2);
    for (Node& n : nodes) n.x += pad - lx, n.y += pad - ly;
    out->w = hx - lx + 2 * pad, out->h = hy - ly + 2 * pad;

    const std::vector<u8> series = diagram_series(p);
    auto ink_of = [&](const Node& n) { return n.parent < 0 ? static_cast<u8>(kMdAccent) : series[static_cast<size_t>(n.branch) % series.size()]; };

    // Branches first (under the nodes), thinner the further out.
    for (size_t i = 1; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        const Node& par = nodes[static_cast<size_t>(n.parent)];
        const DiagramVec b = side_point(n, -n.side);
        RasterPath curve;
        if (lk.under(par.depth)) {
            // Hung under its parent: down from the parent's foot, and round
            // a corner to its side.
            const float side = static_cast<float>(n.side), x = b.x - side * lk.indent() * 0.55f, r = std::min(lk.indent() * 0.55f, b.y - (par.y + par.h / 2));
            curve.move_to(x, par.y + par.h / 2);
            curve.line_to(x, b.y - r);
            curve.quad_to(x, b.y, x + side * r, b.y);
            curve.line_to(b.x, b.y);
        } else {
            // Beside it: from the parent's side, as far up or down it as
            // the node is among those on that side.
            float reach = 0;
            for (const int c : par.kids)
                if (nodes[static_cast<size_t>(c)].side == n.side) reach = std::max(reach, std::fabs(nodes[static_cast<size_t>(c)].y - par.y));
            const DiagramVec a = side_point(par, n.side, reach > 0 ? (n.y - par.y) / reach : 0);
            const float k = (b.x - a.x) * 0.5f;
            curve.move_to(a.x, a.y);
            curve.cubic_to(a.x + k, a.y, b.x - k, b.y, b.x, b.y);
        }
        const float width = (n.depth == 1 ? 2.8f : n.depth == 2 ? 2 : 1.5f) * u;
        if (geo) geo->lines.push_back({"n" + std::to_string(n.parent), "n" + std::to_string(i), diagram_sample(curve, 12)});
        out->shape(std::move(curve), {}, p.tone(ink_of(n), 0.8f), width, {}, RasterStroke::kRound);
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        const u8 ink = ink_of(n);
        const bool root = n.parent < 0;
        const float k = root ? (dark ? 0.42f : 0.34f) : n.depth == 1 ? (dark ? 0.34f : 0.27f) : (dark ? 0.15f : 0.11f);
        const DiagramColour fill = p.tone(ink, k);
        const DiagramColour stroke = p.tone(ink, n.depth <= 1 ? 0.9f : 0.65f);
        RasterPath shape;
        outline(n, u, &shape);
        if (n.shape == kPlain) {
            // (a plain node: its box, and a line under it in its colour)
            out->shape(std::move(shape), fill);
            RasterPath under;
            const float r = std::min(n.h / 2, 6 * u);
            under.move_to(n.x - n.w / 2 + r * 0.6f, n.y + n.h / 2 - 0.9f * u), under.line_to(n.x + n.w / 2 - r * 0.6f, n.y + n.h / 2 - 0.9f * u);
            out->shape(std::move(under), {}, p.tone(ink, 0.9f), 1.8f * u, {}, RasterStroke::kRound);
        } else {
            out->shape(std::move(shape), fill, stroke, (root ? 1.6f : n.depth == 1 ? 1.4f : 1.2f) * u);
        }
        const float size = root ? lk.root_size : lk.size, line = root ? lk.root_line : lk.line;
        float y = n.y - n.th / 2 + (line - font.ascent(size) - font.descent(size)) / 2 + font.ascent(size);
        for (const std::string& l : n.text) {
            out->text(l, n.x, y, size, p.of(kMdText), font, 1, root);
            y += line;
        }
        if (geo) geo->boxes.push_back({"node", "n" + std::to_string(i), n.x - n.w / 2, n.y - n.h / 2, n.w, n.h});
    }
    return true;
}

}  // namespace blit
