// Class diagrams and entity relationship diagrams: boxes of members (a
// class's name, attributes and methods; an entity's attributes, as a
// table) joined by relations with ends of their own (a hollow triangle for
// a class's parent, diamonds for its parts, crow's feet for how many),
// laid out in layers as flowcharts are (diagram_layered) and drawn here.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

#include "diagram.hpp"

namespace blit {

namespace {

constexpr size_t kMostBoxes = 1000, kMostRelations = 2000, kMostMembers = 20000;

// ---------------------------------------------------------------- shared

// Colours a style says (fill:#f9f,stroke:#333,color:#fff,stroke-width:2px).
struct Paint {
    DiagramColour fill, stroke, text;
    float width = -1;  // Mermaid's pixels (at 16 pixel text)
    void over(const Paint& o) {
        if (!o.fill.none()) fill = o.fill;
        if (!o.stroke.none()) stroke = o.stroke;
        if (!o.text.none()) text = o.text;
        if (o.width >= 0) width = o.width;
    }
};

Paint parse_paint(std::string_view css) {
    Paint p;
    size_t at = 0;
    int depth = 0;
    for (size_t i = 0; i <= css.size(); ++i) {
        if (i < css.size() && css[i] == '(') ++depth;
        if (i < css.size() && css[i] == ')') --depth;
        if (i < css.size() && !((css[i] == ',' || css[i] == ';') && depth <= 0)) continue;
        const std::string_view part = css.substr(at, i - at);
        at = i + 1;
        const size_t colon = part.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string key = diagram_lower(diagram_trim(part.substr(0, colon)));
        std::string_view value = diagram_trim(part.substr(colon + 1));
        if (value.ends_with("!important")) value = diagram_trim(value.substr(0, value.size() - 10));
        if (key == "fill" || key == "background" || key == "background-color") diagram_colour(value, &p.fill);
        else if (key == "stroke" || key == "border-color") diagram_colour(value, &p.stroke);
        else if (key == "color") diagram_colour(value, &p.text);
        else if (key == "stroke-width" || key == "border-width") {
            const float v = std::strtof(std::string(value).c_str(), nullptr);
            if (v >= 0 && v < 100) p.width = v;
        }
    }
    return p;
}

// The words of a statement: split at spaces, a "quoted" part one word
// (its quotes kept).
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        const size_t from = i;
        bool quoted = false;
        while (i < s.size() && (quoted || (s[i] != ' ' && s[i] != '\t'))) {
            if (s[i] == '"') quoted = !quoted;
            ++i;
        }
        out.emplace_back(s.substr(from, i - from));
    }
    return out;
}

std::string unquote(std::string_view s) {
    s = diagram_trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '`' && s.back() == '`'))) s = s.substr(1, s.size() - 2);
    return std::string(s);
}

// Where c first is in s outside quotes (npos: nowhere).
size_t find_bare(std::string_view s, char c, size_t from = 0) {
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') quoted = !quoted;
        else if (!quoted && i >= from && s[i] == c) return i;
    }
    return std::string_view::npos;
}

// Whether a statement (in lower case) starts with a keyword: the word
// itself, then its end (a class named Notebook is not a note).
bool keyword(std::string_view l, std::string_view word) {
    if (!l.starts_with(word)) return false;
    if (l.size() == word.size()) return true;
    const char c = l[word.size()];
    return c == ' ' || c == '\t' || c == '"' || c == ':' || c == '{';
}

// The colon a label or a member follows (not one of :::'s).
size_t label_colon(std::string_view s) {
    for (size_t at = find_bare(s, ':'); at != std::string_view::npos; at = find_bare(s, ':', at + 1)) {
        if (!s.substr(at).starts_with(":::")) return at;
        at += 2;
    }
    return std::string_view::npos;
}

// The tones everything is drawn in (as flowcharts' are).
struct Ink {
    DiagramColour fill, head, stroke, rule, text, quiet, key, edge, label_fill, cluster_fill, cluster_stroke, note_fill, note_stroke, row_odd, row_even, bg;
};
Ink inks(const DiagramPalette& p) {
    const bool dark = p.dark();
    Ink k;
    k.fill = p.tone(kMdBorder, dark ? 0.17f : 0.11f);
    k.head = p.tone(kMdBorder, dark ? 0.3f : 0.22f);
    k.stroke = p.tone(kMdBorder, dark ? 0.8f : 0.75f);
    k.rule = p.tone(kMdBorder, dark ? 0.45f : 0.4f);
    k.text = p.of(kMdText);
    k.quiet = p.tone(kMdText, dark ? 0.68f : 0.72f);
    k.key = p.tone(kMdAccent, dark ? 0.95f : 1);
    k.edge = p.tone(kMdText, dark ? 0.55f : 0.6f);
    k.label_fill = p.tone(kMdText, dark ? 0.12f : 0.08f);
    k.cluster_fill = p.tone(kMdAccent, dark ? 0.06f : 0.045f);
    k.cluster_stroke = p.tone(kMdAccent, dark ? 0.4f : 0.45f);
    k.note_fill = p.tone(kMdSlice1, dark ? 0.2f : 0.16f);
    k.note_stroke = p.tone(kMdSlice1, 0.7f);
    k.row_odd = p.tone(kMdBorder, dark ? 0.1f : 0.05f);
    k.row_even = p.tone(kMdBorder, dark ? 0.15f : 0.1f);
    k.bg = {p.bg[0], p.bg[1], p.bg[2], 1};
    return k;
}

DiagramColour readable_on(DiagramColour fill, const DiagramPalette& p) {
    if (fill.none() || fill.a < 0.5f) return p.of(kMdText);
    const float lum = fill.r * 0.3f + fill.g * 0.59f + fill.b * 0.11f;
    return lum > 0.55f ? DiagramColour{0.1f, 0.1f, 0.12f, 1} : DiagramColour{0.96f, 0.96f, 0.97f, 1};
}

// A line of text, its baseline's left end at (x, y) (align 1: centred on x).
void say(Drawing& d, std::string_view s, float x, float y, float size, DiagramColour c, const DiagramFont& font, int align, bool bold, bool italic = false) {
    const size_t n = d.items.size();
    d.text(s, x, y, size, c, font, align, bold);
    if (italic && d.items.size() > n) d.items.back().italic = true;
}

// The ends of relations.
enum End : u8 {
    kEndNone,
    kEndArrow,          // association, dependency: <, >
    kEndTriangle,       // inheritance, realization: <|, |>
    kEndDiamond,        // composition: *
    kEndHollowDiamond,  // aggregation: o
    kEndLollipop,       // an interface: ()
    kEndOne,            // ||
    kEndZeroOrOne,      // |o
    kEndOneOrMore,      // }|
    kEndZeroOrMore,     // }o
};

// How far from its tip an end reaches along its line (points).
float end_length(u8 end, float u) {
    switch (end) {
        case kEndArrow: return 11 * u;
        case kEndTriangle: return 16 * u;
        case kEndDiamond:
        case kEndHollowDiamond: return 20 * u;
        case kEndLollipop: return 11 * u;
        case kEndOne: return 15 * u;
        case kEndZeroOrOne: return 24 * u;
        case kEndOneOrMore: return 20 * u;
        case kEndZeroOrMore: return 27 * u;
        default: return 0;
    }
}
// How wide it is, across (half).
float end_half(u8 end, float u) {
    switch (end) {
        case kEndArrow: return 5 * u;
        case kEndTriangle: return 8 * u;
        case kEndDiamond:
        case kEndHollowDiamond: return 6 * u;
        case kEndLollipop: return 5.5f * u;
        case kEndNone: return 0;
        default: return 8 * u;
    }
}

// An end drawn at `tip` (on a box's border), its line coming in along
// `dir` (a unit vector, pointing into the box); how far back from the tip
// the line should stop.
float draw_end(Drawing& d, u8 end, DiagramVec tip, DiagramVec dir, float u, DiagramColour c, DiagramColour bg, float width) {
    const DiagramVec n{-dir.y, dir.x};
    auto at = [&](float back, float across) { return RasterPoint{tip.x - dir.x * back + n.x * across, tip.y - dir.y * back + n.y * across}; };
    const float len = end_length(end, u), half = end_half(end, u);
    switch (end) {
        case kEndArrow: {
            const RasterPoint pts[] = {at(0, 0), at(len, half), at(len * 0.8f, 0), at(len, -half)};
            RasterPath p;
            p.polygon(pts, 4);
            d.shape(std::move(p), c, c, width * 0.6f);
            return len * 0.8f;
        }
        case kEndTriangle: {
            const RasterPoint pts[] = {at(0, 0), at(len, half), at(len, -half)};
            RasterPath p;
            p.polygon(pts, 3);
            d.shape(std::move(p), bg, c, width);
            return len;
        }
        case kEndDiamond:
        case kEndHollowDiamond: {
            const RasterPoint pts[] = {at(0, 0), at(len / 2, half), at(len, 0), at(len / 2, -half)};
            RasterPath p;
            p.polygon(pts, 4);
            d.shape(std::move(p), end == kEndDiamond ? c : bg, c, width);
            return len;
        }
        case kEndLollipop: {
            RasterPath p;
            const RasterPoint m = at(half, 0);
            p.ellipse(m.x, m.y, half, half);
            d.shape(std::move(p), bg, c, width);
            return 2 * half;
        }
        case kEndOne:
        case kEndZeroOrOne:
        case kEndOneOrMore:
        case kEndZeroOrMore: {
            // From the box out: crow's feet (many) or a bar (one), then a
            // bar (at least one) or a ring (maybe none).
            const bool many = end == kEndOneOrMore || end == kEndZeroOrMore, zero = end == kEndZeroOrOne || end == kEndZeroOrMore;
            const float foot = 12 * u, spread = 6.5f * u, r = 4.5f * u;
            RasterPath p;
            float next;
            if (many) {
                const RasterPoint a = at(0, spread), b = at(foot, 0), e = at(0, -spread);
                p.move_to(a.x, a.y), p.line_to(b.x, b.y), p.line_to(e.x, e.y);
                next = foot + 4 * u;
            } else {
                const RasterPoint a = at(6 * u, half * 0.85f), b = at(6 * u, -half * 0.85f);
                p.move_to(a.x, a.y), p.line_to(b.x, b.y);
                next = 11 * u;
            }
            if (!zero) {
                const RasterPoint a = at(next, half * 0.85f), b = at(next, -half * 0.85f);
                p.move_to(a.x, a.y), p.line_to(b.x, b.y);
            }
            d.shape(std::move(p), {}, c, width, {}, RasterStroke::kRound);
            if (zero) {
                RasterPath ring;
                const RasterPoint m = at(next + r + (many ? 2 * u : 4 * u), 0);
                ring.ellipse(m.x, m.y, r, r);
                d.shape(std::move(ring), bg, c, width);
            }
            return 0;
        }
        default: return 0;
    }
}

// What a relation is drawn as.
struct Look {
    const DiagramStyle* style = nullptr;
    Ink ink;
    float u = 1, size = 13, line = 16, stroke_w = 1.3f, edge_w = 1.5f;
};

// A text's lines centred on (cx, cy).
void text_block(Drawing& d, const Look& k, const std::vector<std::string>& lines, float cx, float cy, DiagramColour c, bool bold) {
    const DiagramFont& font = *k.style->font;
    const float th = static_cast<float>(lines.size()) * k.line;
    float y = cy - th / 2 + (k.line - font.ascent(k.size) - font.descent(k.size)) / 2 + font.ascent(k.size);
    for (const std::string& l : lines) {
        say(d, l, cx, y, k.size, c, font, 1, bold);
        y += k.line;
    }
}

// A relation as laid out: its line (a curve, straight where its ends are),
// its ends, its label on a pill, and a few words at either end (how many).
struct Relation {
    int a = 0, b = 0;
    u8 end_a = kEndNone, end_b = kEndNone;
    bool dashed = false;
    std::string card_a, card_b;
    std::vector<std::string> label;
};

// Which side of its line the words at each end go (spread_ends says): 0,
// to the right of it or below it; -1 or 1, toward less or more x (or y,
// for a line into a box's side).
using WordSides = std::array<signed char, 2>;

// How many, at an end: its size, and how far across it is (from the line).
float card_size(const Look& k) { return k.size * 0.9f; }
float card_half(u8 end, float u) { return std::max(end_half(end, u), 2 * u); }

void draw_relation(Drawing& d, const Look& k, const LayeredGraph& lg, const LayeredGraph::Edge& e, const Relation& r, WordSides words, DiagramGeometry* geo, std::vector<DiagramGeometry::Box>* labels) {
    const float u = k.u, width = k.edge_w;
    const DiagramColour c = k.ink.edge;
    std::vector<float> dash;
    if (r.dashed) dash = {3.2f * width, 2.6f * width};
    const std::string& name_a = lg.nodes[static_cast<size_t>(r.a)].id;
    const std::string& name_b = lg.nodes[static_cast<size_t>(r.b)].id;
    auto unit = [](DiagramVec v) {
        const float l = std::hypot(v.x, v.y);
        return l > 0 ? DiagramVec{v.x / l, v.y / l} : DiagramVec{0, 1};
    };
    if (e.pts.size() < 2) return;
    const DiagramFont& font = *k.style->font;
    const float cs = card_size(k), ch = font.line(cs);
    std::vector<DiagramVec> pts = e.pts;
    DiagramVec in_a, in_b;  // the way each end goes into its box
    if (e.loop) {
        // (straight into the side it leaves and comes back to, for as long
        // as its ends are: diagram_layered was told how long)
        if (pts.size() != 6) return;
        in_a = in_b = unit({-e.out.x, -e.out.y});
    } else {
        in_a = {-e.axis.x, -e.axis.y}, in_b = e.axis;
        // Straight where the ends are, as long as they are, and the words
        // beside them (when there is room).
        auto straight = [&](u8 end, const std::string& card) {
            float l = end ? end_length(end, u) : 0;
            if (!card.empty()) l = std::max(l, 4 * u + (std::fabs(e.axis.y) > 0.5f ? ch : font.width(card, cs)));
            return l > 0 ? l + 6 * u : 0;
        };
        const float la = straight(r.end_a, r.card_a), lb = straight(r.end_b, r.card_b);
        const DiagramVec p0 = pts.front(), p1 = pts.back();
        const float along = (p1.x - p0.x) * e.axis.x + (p1.y - p0.y) * e.axis.y;
        if (la > 0 && along > la + lb + 8 * u) {
            const float first = (pts[1].x - p0.x) * e.axis.x + (pts[1].y - p0.y) * e.axis.y;
            if (first > la + 4 * u) pts.insert(pts.begin() + 1, DiagramVec{p0.x + e.axis.x * la, p0.y + e.axis.y * la});
        }
        if (lb > 0 && along > la + lb + 8 * u) {
            const DiagramVec q = pts[pts.size() - 2];
            const float last = (p1.x - q.x) * e.axis.x + (p1.y - q.y) * e.axis.y;
            if (last > lb + 4 * u) pts.insert(pts.end() - 1, DiagramVec{p1.x - e.axis.x * lb, p1.y - e.axis.y * lb});
        }
    }
    const DiagramVec tip_a = pts.front(), tip_b = pts.back();
    // (the line first, the ends over it)
    Drawing ends;
    const float back_a = draw_end(ends, r.end_a, tip_a, in_a, u, c, k.ink.bg, width);
    const float back_b = draw_end(ends, r.end_b, tip_b, in_b, u, c, k.ink.bg, width);
    pts.front() = {tip_a.x - in_a.x * back_a, tip_a.y - in_a.y * back_a};
    pts.back() = {tip_b.x - in_b.x * back_b, tip_b.y - in_b.y * back_b};
    RasterPath path;
    if (e.loop) {
        path.move_to(pts[0].x, pts[0].y);
        path.line_to(pts[1].x, pts[1].y);
        path.cubic_to(pts[2].x, pts[2].y, pts[3].x, pts[3].y, pts[4].x, pts[4].y);
        path.line_to(pts[5].x, pts[5].y);
    } else {
        diagram_curve(pts, e.axis, e.axis, &path);
    }
    if (geo) {
        DiagramGeometry::Line gl{name_a, name_b, diagram_sample(path, 16)};
        gl.pts.front() = {tip_a.x, tip_a.y}, gl.pts.back() = {tip_b.x, tip_b.y};
        geo->lines.push_back(std::move(gl));
    }
    // (where the line is, for the words beside it)
    const std::vector<RasterPoint> drawn = r.card_a.empty() && r.card_b.empty() ? std::vector<RasterPoint>{} : diagram_sample(path, 64);
    d.shape(std::move(path), {}, c, width, dash, dash.empty() ? RasterStroke::kButt : RasterStroke::kRound);
    for (DiagramItem& it : ends.items) d.items.push_back(std::move(it));
    // How many, at each end: beside the line, just off the box (and further
    // off it, if the line bends toward them there: when the boxes are too
    // near for it to run straight for as long as they are).
    for (const bool first : {true, false}) {
        const std::string& card = first ? r.card_a : r.card_b;
        if (card.empty()) continue;
        const DiagramVec tip = first ? tip_a : tip_b, in = first ? in_a : in_b;
        const float tw = font.width(card, cs);
        const float half = card_half(first ? r.end_a : r.end_b, u);
        // (to the right of the line as it leaves the box, or below it; a
        // loop's away from its other end; where spread_ends said, away from
        // the ends beside it)
        DiagramVec side{-in.y, in.x};
        if (side.x < -0.5f || side.y < -0.5f) side = {-side.x, -side.y};
        if (e.loop && first) side = {-side.x, -side.y};
        if (const signed char w = words[first ? 0 : 1]; !e.loop && w != 0) side = {std::fabs(side.x) * w, std::fabs(side.y) * w};
        const float wide = std::fabs(side.x) > 0.5f ? tw : ch, deep = std::fabs(in.y) > 0.5f ? ch : tw;
        float bend = 0;
        for (const RasterPoint& p : drawn) {
            const float out = -((p.x - tip.x) * in.x + (p.y - tip.y) * in.y);
            if (out >= -1 && out <= 6 * u + deep) bend = std::max(bend, (p.x - tip.x) * side.x + (p.y - tip.y) * side.y);
        }
        const float off_across = std::max(half, bend) + 5 * u + wide / 2;
        const float off_along = 4 * u + deep / 2;
        const DiagramVec m{tip.x - in.x * off_along + side.x * off_across, tip.y - in.y * off_along + side.y * off_across};
        const float y = m.y - ch / 2 + (ch - font.ascent(cs) - font.descent(cs)) / 2 + font.ascent(cs);
        say(d, card, m.x, y, cs, k.ink.quiet, font, 1, false);
        if (labels) labels->push_back({"label", (first ? name_a : name_b) + ":" + card, m.x - tw / 2, m.y - ch / 2, tw, ch});
    }
    if (!e.label.empty() && e.label_w > 0) {
        RasterPath pill;
        const float lx = e.label_at.x - e.label_w / 2, ly = e.label_at.y - e.label_h / 2;
        pill.rect(lx, ly, e.label_w, e.label_h, std::min(e.label_h / 2, 6 * u));
        d.shape(std::move(pill), k.ink.label_fill);
        text_block(d, k, e.label, e.label_at.x, e.label_at.y, k.ink.text, false);
        if (labels) labels->push_back({"label", name_a + "->" + name_b, lx, ly, e.label_w, e.label_h});
    }
}

// A relation from a box to itself: how long its ends are (it runs straight
// out of the box for so long) and how wide, and how much room the words
// beside them take across (the ranks going `dir`), for diagram_layered.
void loop_ends(const Look& k, const Relation& r, char dir, LayeredGraph::Edge* e) {
    const float u = k.u;
    const DiagramFont& font = *k.style->font;
    const float cs = k.size * 0.9f;
    e->end_reach = std::max(end_length(r.end_a, u), end_length(r.end_b, u)) + 6 * u;
    e->end_half = std::max({end_half(r.end_a, u), end_half(r.end_b, u), 2 * u});
    e->end_word = 0;
    for (const std::string* card : {&r.card_a, &r.card_b})
        if (!card->empty()) e->end_word = std::max(e->end_word, 5 * u + (dir == 'D' || dir == 'U' ? font.line(cs) : font.width(*card, cs)));
}

// How far out of their boxes along the ranks (going `dir`) relations' ends
// and the words beside them reach, for each box.
void end_rooms(const Look& k, const std::vector<Relation>& rels, char dir, LayeredGraph* g) {
    const float u = k.u;
    const DiagramFont& font = *k.style->font;
    const float cs = card_size(k), ch = font.line(cs);
    const bool vert = dir == 'D' || dir == 'U';
    for (const Relation& r : rels) {
        if (r.a == r.b) continue;
        for (const bool first : {true, false}) {
            const u8 end = first ? r.end_a : r.end_b;
            const std::string& card = first ? r.card_a : r.card_b;
            float room = end_length(end, u);
            if (!card.empty()) room = std::max(room, 4 * u + (vert ? ch : font.width(card, cs)));
            if (room > 0) room += 6 * u;  // (and a little, as draw_relation runs straight)
            float& at = g->nodes[static_cast<size_t>(first ? r.a : r.b)].end_room;
            at = std::max(at, room);
        }
    }
}

// A cluster's box and its title at the top (as a flowchart's subgraph:
// where the layout has it, the edges that come in at its top beside it).
void draw_cluster(Drawing& d, const Look& k, const LayeredGraph::Cluster& cl, DiagramGeometry* geo) {
    const float u = k.u;
    RasterPath box;
    box.rect(cl.x, cl.y, cl.w, cl.h, 5 * u);
    d.shape(std::move(box), k.ink.cluster_fill, k.ink.cluster_stroke, k.stroke_w);
    if (cl.title.empty()) return;
    const DiagramFont& font = *k.style->font;
    float tw = 0;
    for (const std::string& t : cl.title) tw = std::max(tw, font.width(t, k.size, true));
    const float th = (k.line + 6 * u) * static_cast<float>(cl.title.size());
    const float top = cl.y + 7 * u;
    RasterPath back;
    back.rect(cl.title_x - tw / 2 - 4 * u, top, tw + 8 * u, th - 2 * u, 3 * u);
    d.shape(std::move(back), k.ink.cluster_fill);
    text_block(d, k, cl.title, cl.title_x, top + (th - 2 * u) / 2, k.ink.text, true);
    if (geo) geo->boxes.push_back({"title", cl.id, cl.title_x - tw / 2 - 4 * u, top, tw + 8 * u, th - 2 * u});
}

// A note (its box and a dashed line to what it is beside).
void draw_note(Drawing& d, const Look& k, const LayeredGraph& lg, const LayeredGraph::Node& n, const std::vector<std::string>& lines) {
    const float u = k.u;
    const float x = n.x - n.w / 2, y = n.y - n.h / 2;
    if (n.note_of >= 0) {
        const LayeredGraph::Node& t = lg.nodes[static_cast<size_t>(n.note_of)];
        const DiagramVec tc{t.x, t.y}, nc{n.x, n.y};
        const DiagramVec a = diagram_shape_hit(kShapeNote, x, y, n.w, n.h, u, tc, nc);
        const DiagramVec b = diagram_shape_hit(kShapeRect, t.x - t.w / 2, t.y - t.h / 2, t.w, t.h, u, nc, tc);
        RasterPath path;
        path.move_to(a.x, a.y), path.line_to(b.x, b.y);
        d.shape(std::move(path), {}, k.ink.note_stroke, k.stroke_w, {3 * u, 3 * u});
    }
    RasterPath box;
    diagram_shape(kShapeNote, x, y, n.w, n.h, u, &box);
    d.shape(std::move(box), k.ink.note_fill, k.ink.note_stroke, k.stroke_w);
    text_block(d, k, lines, n.x, n.y, k.ink.text, false);
}

// A box with its top part (its name) in a stronger tone: the box, the
// band, then the outline over both.
void draw_box(Drawing& d, float x, float y, float w, float h, float head, float r, DiagramColour fill, DiagramColour band, DiagramColour stroke, float width) {
    RasterPath body;
    body.rect(x, y, w, h, r);
    d.shape(std::move(body), fill);
    if (head > 0 && !band.none()) {
        RasterPath top;
        top.move_to(x, y + head);
        top.arc(x + r, y + r, r, r, 3.14159265f, 4.71238898f);
        top.arc(x + w - r, y + r, r, r, 4.71238898f, 6.28318531f);
        top.line_to(x + w, y + head);
        top.close();
        d.shape(std::move(top), band);
    }
    RasterPath outline;
    outline.rect(x, y, w, h, r);
    d.shape(std::move(outline), {}, stroke, width);
}

// Ends that share a side of a box, kept apart: by `gap` at least, and by
// what is between them (their ends' halves, and the words of how many of
// either that are on that side of it). The words of the first go before
// it and of the last after it (away from the rest), of each between to the
// side nearer its end of the row. Moved along the side, in the order they
// were, as little as keeps them apart (squeezed, if the side is too short
// for them); which side each end's words go, in `words`; how wide and how
// tall each box would have to be for none to be squeezed, in `least`.
void spread_ends(LayeredGraph* lg, const std::vector<Relation>& rels, const Look& k, float gap, std::vector<WordSides>* words, std::vector<DiagramVec>* least = nullptr) {
    struct At {
        size_t edge;
        bool first;
        float across, toward;
        float half = 0, word = 0;  // (its end's half, its words' room across)
        signed char side = 0;
    };
    const float u = k.u;
    const DiagramFont& font = *k.style->font;
    const float cs = card_size(k), ch = font.line(cs);
    words->assign(lg->edges.size(), WordSides{0, 0});
    if (least) least->assign(lg->nodes.size(), DiagramVec{});
    std::map<std::pair<int, int>, std::vector<At>> sides;  // (a node, the way its ends go into it)
    for (size_t q = 0; q < lg->edges.size(); ++q) {
        const LayeredGraph::Edge& e = lg->edges[q];
        if (e.loop || e.pts.size() < 2) continue;
        const DiagramVec across{std::fabs(e.axis.y), std::fabs(e.axis.x)};
        auto along = [&](DiagramVec p) { return p.x * across.x + p.y * across.y; };
        const int way = e.axis.x > 0.5f ? 0 : e.axis.x < -0.5f ? 1 : e.axis.y > 0.5f ? 2 : 3;
        // (the first end goes into its box against the axis, the last along it)
        sides[{e.from, way ^ 1}].push_back({q, true, along(e.pts.front()), along(e.pts[1])});
        sides[{e.to, way}].push_back({q, false, along(e.pts.back()), along(e.pts[e.pts.size() - 2])});
    }
    for (auto& [side, list] : sides) {
        if (list.size() < 2) continue;
        const LayeredGraph::Node& n = lg->nodes[static_cast<size_t>(side.first)];
        const bool across_x = side.second >= 2;  // (ends into its top or bottom: spread across x)
        const float mid = across_x ? n.x : n.y, length = across_x ? n.w : n.h;
        std::stable_sort(list.begin(), list.end(), [](const At& a, const At& b) { return a.across != b.across ? a.across < b.across : a.toward < b.toward; });
        for (size_t i = 0; i < list.size(); ++i) {
            At& a = list[i];
            const Relation& r = rels[a.edge];
            const std::string& card = a.first ? r.card_a : r.card_b;
            a.half = card_half(a.first ? r.end_a : r.end_b, u);
            if (!card.empty()) a.word = 5 * u + (across_x ? font.width(card, cs) : ch);
            a.side = i == 0 ? -1 : i + 1 == list.size() ? 1 : static_cast<signed char>(2 * i + 1 < list.size() ? -1 : 1);
        }
        // How far apart each two must be, and are.
        std::vector<float> need(list.size() - 1);
        bool tight = false;
        float total = 0;
        for (size_t i = 0; i + 1 < list.size(); ++i) {
            need[i] = std::max(gap, list[i].half + list[i + 1].half + 6 * u + (list[i].side > 0 ? list[i].word : 0) + (list[i + 1].side < 0 ? list[i + 1].word : 0));
            tight = tight || list[i + 1].across - list[i].across < need[i] - 0.01f;
            total += need[i];
        }
        if (least) {
            float& want = across_x ? (*least)[static_cast<size_t>(side.first)].x : (*least)[static_cast<size_t>(side.first)].y;
            want = std::max(want, total + 8 * u);
        }
        if (tight) {
            // (along the side, within its corners: squeezed if it must be)
            const float span = std::max(length * 0.8f, std::min(total, length - 8 * u));
            const float squeeze = total > span ? span / total : 1;
            float centre = 0;
            for (const At& a : list) centre += a.across;
            centre /= static_cast<float>(list.size());
            const float half = total * squeeze / 2;
            centre = std::clamp(centre, mid - span / 2 + half, mid + span / 2 - half);
            float at = centre - half;
            for (size_t i = 0; i < list.size(); ++i) {
                LayeredGraph::Edge& e = lg->edges[list[i].edge];
                DiagramVec& p = list[i].first ? e.pts.front() : e.pts.back();
                (across_x ? p.x : p.y) = at;
                if (i + 1 < list.size()) at += need[i] * squeeze;
            }
        }
        for (const At& a : list) (*words)[a.edge][a.first ? 0 : 1] = a.side;
    }
}

// The parts of a graph that nothing joins (a namespace's boxes are one
// part with all in it, a note goes with what it is beside): each part's
// nodes, edges and clusters, the parts in the order of their first nodes.
struct Part {
    std::vector<int> nodes, edges, clusters;
};
std::vector<Part> parts_of(const LayeredGraph& g) {
    const size_t n = g.nodes.size(), all = n + g.clusters.size();
    std::vector<size_t> up(all);
    for (size_t i = 0; i < all; ++i) up[i] = i;
    auto find = [&](size_t i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    auto unite = [&](size_t a, size_t b) {
        a = find(a), b = find(b);
        if (a != b) up[std::max(a, b)] = std::min(a, b);
    };
    for (size_t i = 0; i < n; ++i) {
        if (g.nodes[i].cluster >= 0) unite(i, n + static_cast<size_t>(g.nodes[i].cluster));
        if (g.nodes[i].note_of >= 0) unite(i, static_cast<size_t>(g.nodes[i].note_of));
    }
    for (size_t c = 0; c < g.clusters.size(); ++c)
        if (g.clusters[c].parent >= 0) unite(n + c, n + static_cast<size_t>(g.clusters[c].parent));
    for (const LayeredGraph::Edge& e : g.edges) unite(static_cast<size_t>(e.from), static_cast<size_t>(e.to));
    // (a namespace with no boxes in it goes with the first part)
    for (size_t c = 0; c < g.clusters.size() && n > 0; ++c)
        if (find(n + c) >= n) unite(n + c, 0);
    std::vector<Part> parts;
    std::map<size_t, size_t> which;  // (a part's root: the part)
    for (size_t i = 0; i < n; ++i) {
        const auto [it, made] = which.try_emplace(find(i), parts.size());
        if (made) parts.emplace_back();
        parts[it->second].nodes.push_back(static_cast<int>(i));
    }
    for (size_t k = 0; k < g.edges.size(); ++k) parts[which[find(static_cast<size_t>(g.edges[k].from))]].edges.push_back(static_cast<int>(k));
    for (size_t c = 0; c < g.clusters.size() && n > 0; ++c) parts[which[find(n + c)]].clusters.push_back(static_cast<int>(c));
    return parts;
}

// A part of a graph as a graph of its own.
LayeredGraph part_graph(const LayeredGraph& g, const Part& p) {
    LayeredGraph out;
    out.dir = g.dir, out.sep = g.sep;
    std::map<int, int> node, cluster;
    for (const int c : p.clusters) cluster[c] = static_cast<int>(cluster.size());
    for (const int i : p.nodes) node[i] = static_cast<int>(node.size());
    for (const int c : p.clusters) {
        LayeredGraph::Cluster cl = g.clusters[static_cast<size_t>(c)];
        cl.parent = cl.parent >= 0 ? cluster.at(cl.parent) : -1;
        out.clusters.push_back(std::move(cl));
    }
    for (const int i : p.nodes) {
        LayeredGraph::Node nd = g.nodes[static_cast<size_t>(i)];
        nd.cluster = nd.cluster >= 0 ? cluster.at(nd.cluster) : -1;
        nd.note_of = nd.note_of >= 0 ? node.at(nd.note_of) : -1;
        out.nodes.push_back(std::move(nd));
    }
    for (const int k : p.edges) {
        LayeredGraph::Edge e = g.edges[static_cast<size_t>(k)];
        e.from = node.at(e.from), e.to = node.at(e.to);
        out.edges.push_back(std::move(e));
    }
    return out;
}

// Laid out in the direction asked, or, when that is too wide for the room
// and the other way fits better, that way. A graph in parts that nothing
// joins: each part so, by itself, and the parts in rows as wide as the
// room, each row in the middle. (build() makes the graph for the direction
// it is given, in g->dir: how far ends reach depends on it.) False if the
// layout gave up (why in `err`: too large to draw, or asked to stop).
template <typename Build>
bool lay_out_fitting(LayeredGraph* lg, const DiagramStyle& style, char dir, Build build, std::string* err) {
    lg->dir = dir;
    build(lg);
    const std::vector<Part> parts = parts_of(*lg);
    LayeredGraph other;  // (the graph made the other way, once it is wanted)
    other.dir = dir == 'R' || dir == 'L' ? 'D' : 'R';
    bool made = false;
    auto fit = [&](LayeredGraph* g, const Part* p) {
        if (!diagram_layered(g, style, err)) return false;
        if (g->w <= style.room) return true;
        if (!made) build(&other), made = true;
        LayeredGraph o = p ? part_graph(other, *p) : other;
        std::string why;
        if (diagram_layered(&o, style, &why) && std::min(1.0f, style.room / std::max(1.0f, o.w)) > std::min(1.0f, style.room / g->w) * 1.25f) *g = std::move(o);
        return true;
    };
    if (parts.size() <= 1) return fit(lg, nullptr);
    std::vector<LayeredGraph> laid;
    for (const Part& p : parts) {
        laid.push_back(part_graph(*lg, p));
        if (!fit(&laid.back(), &p)) return false;
    }
    // In rows: as many parts in each as fit the room (one, at least).
    const float gap = 40 * style.unit() * lg->sep, row_gap = gap * 0.6f;  // (across, and between rows)
    struct Row {
        size_t from, to;
        float w, h;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < laid.size(); ++i) {
        if (rows.empty() || rows.back().w + gap + laid[i].w > style.room) rows.push_back({i, i, -gap, 0});
        Row& r = rows.back();
        r.to = i + 1, r.w += gap + laid[i].w, r.h = std::max(r.h, laid[i].h);
    }
    float w = 0, h = -row_gap;
    for (const Row& r : rows) w = std::max(w, r.w), h += row_gap + r.h;
    lg->w = w, lg->h = h;
    float y = 0;
    for (const Row& r : rows) {
        float x = (w - r.w) / 2;
        for (size_t i = r.from; i < r.to; ++i) {
            const LayeredGraph& g = laid[i];
            const Part& p = parts[i];
            for (size_t j = 0; j < p.nodes.size(); ++j) {
                LayeredGraph::Node& nd = lg->nodes[static_cast<size_t>(p.nodes[j])];
                nd.x = g.nodes[j].x + x, nd.y = g.nodes[j].y + y;
                nd.w = g.nodes[j].w, nd.h = g.nodes[j].h;
            }
            for (size_t j = 0; j < p.clusters.size(); ++j) {
                LayeredGraph::Cluster& cl = lg->clusters[static_cast<size_t>(p.clusters[j])];
                cl.x = g.clusters[j].x + x, cl.y = g.clusters[j].y + y, cl.w = g.clusters[j].w, cl.h = g.clusters[j].h;
                cl.title_x = g.clusters[j].title_x + x;
            }
            for (size_t j = 0; j < p.edges.size(); ++j) {
                LayeredGraph::Edge& e = lg->edges[static_cast<size_t>(p.edges[j])];
                const int from = e.from, to = e.to;
                e = g.edges[j];
                e.from = from, e.to = to;
                for (DiagramVec& q : e.pts) q.x += x, q.y += y;
                e.label_at.x += x, e.label_at.y += y;
            }
            x += g.w + gap;
        }
        y += r.h + row_gap;
    }
    return true;
}

// Laid out (as lay_out_fitting does) with the ends spread on their sides
// (and which side their words go, in `words`); and, if the ends on a side
// of a box would be squeezed, laid out again with that box as wide or as
// tall as they want (`grow`: each box's least size, which build() takes),
// a few times at most (laid out again, it may go the other way). False if
// the layout gave up (why in `err`).
template <typename Build>
bool lay_out_spread(LayeredGraph* lg, const DiagramStyle& style, char dir, const std::vector<Relation>& rels, const Look& k, std::vector<DiagramVec>* grow, std::vector<WordSides>* words, Build build, std::string* err) {
    const float u = k.u;
    grow->clear();
    for (int pass = 0;; ++pass) {
        *lg = LayeredGraph{};
        if (!lay_out_fitting(lg, style, dir, build, err)) return false;
        std::vector<DiagramVec> least;
        spread_ends(lg, rels, k, 22 * u, words, &least);
        if (pass == 2) return true;
        // (and the sides a box's loops leave from long enough that their
        // ends and words are within its corners: as layered_loops() puts them,
        // on two sides in turn, the first on each as near the side's middle
        // as its own ends let it (when the side is short), each next one
        // outside the ends and words before)
        std::map<std::pair<int, bool>, std::vector<const LayeredGraph::Edge*>> sides;
        std::map<int, int> count;
        for (const LayeredGraph::Edge& e : lg->edges)
            if (e.loop) sides[{e.from, count[e.from]++ % 2 == 1}].push_back(&e);
        for (const auto& [side, loops] : sides) {
            const bool vert = std::fabs(loops.front()->out.x) > 0.5f;  // (the ranks going down: loops beside it, on its left and right sides)
            float out = 0;  // (from the first's ends to the last's words, and a little)
            for (size_t i = 1; i < loops.size(); ++i) out += loops[i - 1]->end_half + loops[i - 1]->end_word + loops[i]->end_half + 4 * u;
            out += loops.back()->end_half + loops.back()->end_word + 4 * u;
            const float span = 2 * (loops.front()->end_half + 2 * u + out);
            float& want = vert ? least[static_cast<size_t>(side.first)].y : least[static_cast<size_t>(side.first)].x;
            want = std::max(want, span);
        }
        bool short_side = false;
        for (size_t i = 0; i < least.size(); ++i) short_side = short_side || least[i].x > lg->nodes[i].w + 0.01f || least[i].y > lg->nodes[i].h + 0.01f;
        if (!short_side) return true;
        grow->resize(least.size());
        for (size_t i = 0; i < least.size(); ++i) (*grow)[i] = {std::max((*grow)[i].x, least[i].x), std::max((*grow)[i].y, least[i].y)};
    }
}

// ---------------------------------------------------------------- class diagrams

// A~T~ as A<T> (and List~List~int~~ as List<List<int>>).
std::string generics(std::string_view s) {
    std::string out;
    int depth = 0;
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '~') {
            out += s[i];
            continue;
        }
        const char next = i + 1 < s.size() ? s[i + 1] : ' ';
        const bool closes = depth > 0 && (next == '~' || !(word(next) || next == '(' || next == '[' || next == '*'));
        if (closes) {
            out += '>';
            --depth;
        } else if (i > 0 && (word(s[i - 1]) || s[i - 1] == ']' || s[i - 1] == ')') && next != ' ') {
            out += '<';
            ++depth;
        } else if (depth > 0) {
            out += '>';
            --depth;
        } else {
            out += '~';
        }
    }
    while (depth-- > 0) out += '>';
    return out;
}

struct Member {
    std::string text;
    bool is_static = false, is_abstract = false;
    bool method = false;
};

Member member_of(std::string_view raw) {
    Member m;
    std::string t = std::string(diagram_trim(raw));
    // (a classifier at the end: $ static, * abstract)
    for (int guard = 0; guard < 2 && !t.empty() && (t.back() == '$' || t.back() == '*'); ++guard) {
        (t.back() == '$' ? m.is_static : m.is_abstract) = true;
        t.pop_back();
        t = std::string(diagram_trim(t));
    }
    const size_t open = t.find('('), close = t.rfind(')');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        m.method = true;
        std::string head = t.substr(0, close + 1);
        std::string ret = std::string(diagram_trim(std::string_view(t).substr(close + 1)));
        while (!ret.empty() && (ret.front() == '$' || ret.front() == '*')) {
            (ret.front() == '$' ? m.is_static : m.is_abstract) = true;
            ret = std::string(diagram_trim(std::string_view(ret).substr(1)));
        }
        m.text = generics(head) + (ret.empty() ? "" : " : " + generics(ret));
    } else {
        m.text = generics(t);
    }
    return m;
}

struct Class {
    std::string id;
    std::vector<std::string> name;  // its title (its label, or its name and generic)
    bool named = false;             // a label or a generic said
    std::vector<std::string> notes;  // <<interface>> and the like
    std::vector<Member> attrs, methods;
    int ns = -1;
    std::vector<std::string> css;
    Paint paint;
};

struct Note {
    std::vector<std::string> lines;
    int of = -1;
    bool left = false;
};

struct ClassModel {
    std::vector<Class> classes;
    std::map<std::string, int> ids;
    std::vector<Relation> rels;
    std::vector<Note> notes;
    std::vector<LayeredGraph::Cluster> spaces;  // namespaces
    std::map<std::string, int> space_ids;
    std::map<std::string, Paint> defs;
    std::vector<std::pair<std::string, Paint>> styles;
    char dir = 'D';
};

char direction_of(std::string_view d) {
    const std::string l = diagram_lower(diagram_trim(d));
    return l.starts_with("lr") ? 'R' : l.starts_with("rl") ? 'L' : l.starts_with("bt") ? 'U' : 'D';
}

// A class named in the text (A, A~T~, `A`, A:::css), made if new.
int class_of(ClassModel& m, std::string_view token, int ns) {
    std::string_view t = diagram_trim(token);
    std::string css;
    if (const size_t c = t.find(":::"); c != std::string_view::npos) css = diagram_trim(t.substr(c + 3)), t = diagram_trim(t.substr(0, c));
    if (t.size() >= 2 && t.front() == '`' && t.back() == '`') t = t.substr(1, t.size() - 2);
    std::string_view gen;
    if (const size_t g = t.find('~'); g != std::string_view::npos) {
        gen = t.substr(g);
        t = diagram_trim(t.substr(0, g));
    }
    const std::string id(t);
    int k;
    if (const auto it = m.ids.find(id); it != m.ids.end()) {
        k = it->second;
    } else {
        Class c;
        c.id = id;
        c.name = {id};
        c.ns = ns;
        m.classes.push_back(std::move(c));
        k = m.ids[id] = static_cast<int>(m.classes.size()) - 1;
    }
    Class& c = m.classes[static_cast<size_t>(k)];
    if (c.ns < 0 && ns >= 0) c.ns = ns;
    if (!gen.empty() && !c.named) c.name = {generics(id + std::string(gen))}, c.named = true;
    if (!css.empty()) c.css.push_back(css);
    return k;
}

// The ends either side of a relation's line: what is just before and
// just after `--` or `..` at [at, at + 2).
u8 left_end(std::string_view s, size_t at, size_t* from) {
    std::string_view b = s.substr(0, at);
    *from = at;
    if (b.ends_with("<|")) return *from = at - 2, kEndTriangle;
    if (b.ends_with("()")) return *from = at - 2, kEndLollipop;
    if (b.ends_with("*")) return *from = at - 1, kEndDiamond;
    if (b.ends_with("<")) return *from = at - 1, kEndArrow;
    if (b.ends_with("o") && (b.size() == 1 || b[b.size() - 2] == ' ' || b[b.size() - 2] == '"')) return *from = at - 1, kEndHollowDiamond;
    return kEndNone;
}
u8 right_end(std::string_view s, size_t at, size_t* to) {
    std::string_view a = s.substr(at);
    *to = at;
    if (a.starts_with("|>")) return *to = at + 2, kEndTriangle;
    if (a.starts_with("()")) return *to = at + 2, kEndLollipop;
    if (a.starts_with("*")) return *to = at + 1, kEndDiamond;
    if (a.starts_with(">")) return *to = at + 1, kEndArrow;
    if (a.starts_with("o") && (a.size() == 1 || a[1] == ' ' || a[1] == '"')) return *to = at + 1, kEndHollowDiamond;
    return kEndNone;
}

// A relation's line, outside quotes and before a label's colon: where
// its -- or .. is (npos: none).
size_t relation_at(std::string_view s) {
    const size_t colon = label_colon(s);
    bool quoted = false;
    for (size_t i = 0; i + 1 < s.size() && i < colon; ++i) {
        if (s[i] == '"') quoted = !quoted;
        if (!quoted && ((s[i] == '-' && s[i + 1] == '-') || (s[i] == '.' && s[i + 1] == '.'))) return i;
    }
    return std::string_view::npos;
}

bool parse_class(const std::vector<std::string>& st, ClassModel& m, std::string* err) {
    {
        const std::string l = diagram_lower(st[0]);
        if (const size_t sp = l.find(' '); sp != std::string::npos) m.dir = direction_of(l.substr(sp + 1));
    }
    std::vector<std::pair<bool, int>> open;  // what { opened: a class's body (true) or a namespace
    std::vector<std::pair<std::vector<std::string>, std::string>> css_of;
    size_t members = 0;
    auto space = [&]() {
        for (auto it = open.rbegin(); it != open.rend(); ++it)
            if (!it->first) return it->second;
        return -1;
    };
    for (size_t k = 1; k < st.size(); ++k) {
        std::string_view s = st[k];
        const std::string l = diagram_lower(s);
        auto fail = [&](const std::string& why) {
            *err = "line " + std::to_string(k + 1) + ": " + why;
            return false;
        };
        if (++members > kMostMembers || m.classes.size() > kMostBoxes || m.rels.size() > kMostRelations) return fail("too large to draw");
        // In a class's body: its members, to its }.
        if (!open.empty() && open.back().first) {
            Class& c = m.classes[static_cast<size_t>(open.back().second)];
            bool closes = false;
            if (s.ends_with('}')) closes = true, s = diagram_trim(s.substr(0, s.size() - 1));
            if (s.starts_with("<<") && s.find(">>") != std::string_view::npos) c.notes.push_back(std::string(diagram_trim(s.substr(2, s.find(">>") - 2))));
            else if (!s.empty()) {
                Member mb = member_of(s);
                (mb.method ? c.methods : c.attrs).push_back(std::move(mb));
            }
            if (closes) open.pop_back();
            continue;
        }
        if (s == "}") {
            if (open.empty()) return fail("a } with nothing to close");
            open.pop_back();
            continue;
        }
        if (l.starts_with("direction ")) {
            m.dir = direction_of(l.substr(10));
            continue;
        }
        if (l.starts_with("click ") || l.starts_with("link ") || l.starts_with("callback ") || keyword(l, "acctitle") || keyword(l, "accdescr") || l.starts_with("title ")) continue;
        if (l.starts_with("namespace ")) {
            std::string_view name = diagram_trim(s.substr(10));
            const bool block = name.ends_with('{');
            if (block) name = diagram_trim(name.substr(0, name.size() - 1));
            if (name.empty()) return fail("a namespace's name is wanted");
            int ns;
            const std::string id = unquote(name);
            if (const auto it = m.space_ids.find(id); it != m.space_ids.end()) {
                ns = it->second;
            } else {
                LayeredGraph::Cluster c;
                c.id = id;
                c.title = {id};
                c.parent = space();
                // (how deep it is: one opened again elsewhere keeps its
                // place, so what is open says nothing of it)
                int deep = 0;
                for (int p = c.parent; p >= 0 && deep < kDiagramDeepest; p = m.spaces[static_cast<size_t>(p)].parent) ++deep;
                if (deep >= kDiagramDeepest) return fail("namespaces nested too deeply");
                m.spaces.push_back(std::move(c));
                ns = m.space_ids[id] = static_cast<int>(m.spaces.size()) - 1;
            }
            if (block) open.push_back({false, ns});
            continue;
        }
        if (l.starts_with("class ")) {
            std::string_view rest = diagram_trim(s.substr(6));
            std::string_view body;
            bool block = false;
            if (const size_t brace = find_bare(rest, '{'); brace != std::string_view::npos) {
                block = true;
                body = diagram_trim(rest.substr(brace + 1));
                rest = diagram_trim(rest.substr(0, brace));
            }
            std::string label;
            if (const size_t sq = rest.find('['); sq != std::string_view::npos && rest.ends_with(']')) {
                label = unquote(rest.substr(sq + 1, rest.size() - sq - 2));
                rest = diagram_trim(rest.substr(0, sq));
            }
            if (rest.empty()) return fail("a class's name is wanted");
            const int c = class_of(m, rest, space());
            if (!label.empty()) m.classes[static_cast<size_t>(c)].name = diagram_label(label), m.classes[static_cast<size_t>(c)].named = true;
            if (block) {
                open.push_back({true, c});
                if (!body.empty()) {  // (members on the same line: class A { +x })
                    bool closes = body.ends_with('}');
                    if (closes) body = diagram_trim(body.substr(0, body.size() - 1));
                    if (!body.empty()) {
                        Member mb = member_of(body);
                        (mb.method ? m.classes[static_cast<size_t>(c)].methods : m.classes[static_cast<size_t>(c)].attrs).push_back(std::move(mb));
                    }
                    if (closes) open.pop_back();
                }
            }
            continue;
        }
        if (s.starts_with("<<")) {  // <<interface>> A
            const size_t close = s.find(">>");
            if (close == std::string_view::npos) return fail("a >> is wanted");
            const std::string_view who = diagram_trim(s.substr(close + 2));
            if (who.empty()) return fail("a class is wanted after the annotation");
            m.classes[static_cast<size_t>(class_of(m, who, space()))].notes.push_back(std::string(diagram_trim(s.substr(2, close - 2))));
            continue;
        }
        if (keyword(l, "note")) {
            std::string_view rest = diagram_trim(s.substr(4));
            Note n;
            if (diagram_lower(rest).starts_with("for ")) {
                rest = diagram_trim(rest.substr(4));
                const size_t q = rest.find('"');
                if (q == std::string_view::npos) return fail("a note's text is wanted, in quotes");
                const std::string_view who = diagram_trim(rest.substr(0, q));
                n.of = class_of(m, who, space());
                rest = rest.substr(q);
            }
            n.lines = diagram_label(rest);
            m.notes.push_back(std::move(n));
            continue;
        }
        if (l.starts_with("classdef ")) {
            const std::vector<std::string> w = words(s.substr(9));
            if (w.size() < 2) return fail("a classDef wants a name and a style");
            const Paint p = parse_paint(diagram_trim(s.substr(9 + s.substr(9).find(w[0]) + w[0].size())));
            size_t at = 0;
            const std::string& names = w[0];
            for (size_t i = 0; i <= names.size(); ++i)
                if (i == names.size() || names[i] == ',') {
                    m.defs[std::string(diagram_trim(std::string_view(names).substr(at, i - at)))] = p;
                    at = i + 1;
                }
            continue;
        }
        if (l.starts_with("cssclass ")) {
            const std::vector<std::string> w = words(s.substr(9));
            if (w.size() < 2) return fail("a cssClass wants classes and a name");
            std::vector<std::string> who;
            const std::string list = unquote(w[0]);
            size_t at = 0;
            for (size_t i = 0; i <= list.size(); ++i)
                if (i == list.size() || list[i] == ',') {
                    who.emplace_back(diagram_trim(std::string_view(list).substr(at, i - at)));
                    at = i + 1;
                }
            css_of.push_back({who, w[1]});
            continue;
        }
        if (l.starts_with("style ")) {
            const std::vector<std::string> w = words(s.substr(6));
            if (w.size() < 2) return fail("a style wants a class and a style");
            m.styles.push_back({w[0], parse_paint(diagram_trim(s.substr(6 + s.substr(6).find(w[0]) + w[0].size())))});
            continue;
        }
        // A relation: A "1" <|-- "many" B : label
        if (const size_t at = relation_at(s); at != std::string_view::npos) {
            Relation r;
            size_t from, to;
            r.end_a = left_end(s, at, &from);
            r.end_b = right_end(s, at + 2, &to);
            r.dashed = s[at] == '.';
            std::string_view left = diagram_trim(s.substr(0, from)), right = s.substr(to);
            if (const size_t colon = label_colon(right); colon != std::string_view::npos) {
                r.label = diagram_label(right.substr(colon + 1));
                if (r.label.size() == 1 && r.label[0].empty()) r.label.clear();
                right = right.substr(0, colon);
            }
            right = diagram_trim(right);
            if (left.ends_with('"')) {
                const size_t q = left.rfind('"', left.size() - 2);
                if (q == std::string_view::npos) return fail("a \" is wanted");
                r.card_a = std::string(left.substr(q + 1, left.size() - q - 2));
                left = diagram_trim(left.substr(0, q));
            }
            if (right.starts_with('"')) {
                const size_t q = right.find('"', 1);
                if (q == std::string_view::npos) return fail("a \" is wanted");
                r.card_b = std::string(right.substr(1, q - 1));
                right = diagram_trim(right.substr(q + 1));
            }
            if (left.empty() || right.empty() || left.find(' ') != std::string_view::npos || (right.find(' ') != std::string_view::npos && !right.starts_with('`')))
                return fail("a class is wanted either side of \"" + std::string(s.substr(from, to - from)) + "\"");
            r.a = class_of(m, left, space());
            r.b = class_of(m, right, space());
            m.rels.push_back(std::move(r));
            continue;
        }
        // A member: A : +name
        if (const size_t colon = label_colon(s); colon != std::string_view::npos) {
            const std::string_view who = diagram_trim(s.substr(0, colon)), what = diagram_trim(s.substr(colon + 1));
            if (who.empty() || who.find(' ') != std::string_view::npos) return fail("a class is wanted before \":\"");
            Class& c = m.classes[static_cast<size_t>(class_of(m, who, space()))];
            if (what.starts_with("<<") && what.find(">>") != std::string_view::npos) {
                c.notes.push_back(std::string(diagram_trim(what.substr(2, what.find(">>") - 2))));
            } else if (!what.empty()) {
                Member mb = member_of(what);
                (mb.method ? c.methods : c.attrs).push_back(std::move(mb));
            }
            continue;
        }
        return fail("a class, a relation or a member is wanted at \"" + std::string(s.substr(0, 24)) + "\"");
    }
    // Colours: classDef default, then each class's, then style lines.
    for (const auto& [who, name] : css_of)
        for (const std::string& id : who)
            if (const auto it = m.ids.find(id); it != m.ids.end()) m.classes[static_cast<size_t>(it->second)].css.push_back(name);
    const auto def = m.defs.find("default");
    for (Class& c : m.classes) {
        Paint p;
        if (def != m.defs.end()) p.over(def->second);
        for (const std::string& n : c.css)
            if (const auto it = m.defs.find(n); it != m.defs.end()) p.over(it->second);
        c.paint = p;
    }
    for (const auto& [id, p] : m.styles)
        if (const auto it = m.ids.find(id); it != m.ids.end()) m.classes[static_cast<size_t>(it->second)].paint.over(p);
    return true;
}

// A member's lines as drawn (wrapped, the lines after the first indented).
struct Row {
    std::string text;
    float indent = 0;
    bool is_static = false, is_abstract = false;
};

// A class's box: its rows and the heights of its parts.
struct ClassBox {
    std::vector<std::string> notes, name;
    std::vector<Row> attrs, methods;
    float w = 0, head = 0, mid = 0, h = 0;  // its width, its name's part, its attributes' part, the whole
};

bool classdiagram(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    ClassModel m;
    if (!parse_class(st, m, err)) return false;
    if (m.classes.empty()) {
        *err = "nothing in it";
        return false;
    }
    const DiagramFont& font = *style.font;
    Look k;
    k.style = &style;
    k.ink = inks(style.palette);
    k.u = style.unit(), k.size = style.size, k.line = font.line(style.size);
    k.stroke_w = 1.3f * k.u, k.edge_w = 1.4f * k.u;
    const float u = k.u, size = k.size, line = k.line;
    const float px = 12 * u, py = 7 * u;
    const float wrap = std::max(font.width("abcdefghijklmnopqrstuvwxyz", size) * 1.5f, 240 * u);
    const float name_wrap = std::max(font.width("abcdefghijklmnopqrstuvwxyz", size), 160 * u);
    const float note_size = size * 0.92f;
    // Each class's box.
    std::vector<ClassBox> boxes(m.classes.size());
    for (size_t i = 0; i < m.classes.size(); ++i) {
        const Class& c = m.classes[i];
        ClassBox& b = boxes[i];
        float tw = 0;
        for (const std::string& n : c.notes) {
            b.notes.push_back("«" + n + "»");
            tw = std::max(tw, font.width(b.notes.back(), note_size));
        }
        b.name = diagram_wrap(c.name, name_wrap, size, font);
        for (const std::string& n : b.name) tw = std::max(tw, font.width(n, size, true));
        auto rows = [&](const std::vector<Member>& list, std::vector<Row>* out_rows) {
            for (const Member& mb : list) {
                const std::vector<std::string> lines = diagram_wrap({mb.text}, wrap, size, font);
                for (size_t j = 0; j < lines.size(); ++j) {
                    Row r{lines[j], j ? size : 0, mb.is_static, mb.is_abstract};
                    tw = std::max(tw, r.indent + font.width(r.text, size));
                    out_rows->push_back(std::move(r));
                }
            }
        };
        rows(c.attrs, &b.attrs);
        rows(c.methods, &b.methods);
        b.w = std::max(tw + 2 * px, 80 * u);
        b.head = py * 2 + static_cast<float>(b.notes.size()) * font.line(note_size) + static_cast<float>(b.name.size()) * line;
        auto part = [&](size_t n) { return n ? py * 1.4f + static_cast<float>(n) * line : py * 1.6f; };
        b.mid = part(b.attrs.size());
        b.h = b.head + b.mid + part(b.methods.size());
    }
    std::vector<std::vector<std::string>> note_lines(m.notes.size());
    for (size_t i = 0; i < m.notes.size(); ++i) note_lines[i] = diagram_wrap(m.notes[i].lines, wrap * 0.8f, size, font);
    // Laid out.
    LayeredGraph lg;
    std::vector<DiagramVec> grow;
    auto build = [&](LayeredGraph* g) {
        g->nodes.clear(), g->edges.clear();
        g->clusters = m.spaces;
        g->sep = 1.5f;
        for (size_t i = 0; i < m.classes.size(); ++i) {
            LayeredGraph::Node n;
            n.id = m.classes[i].id;
            n.w = boxes[i].w, n.h = boxes[i].h;
            if (i < grow.size()) n.w = std::max(n.w, grow[i].x), n.h = std::max(n.h, grow[i].y);
            n.cluster = m.classes[i].ns;
            g->nodes.push_back(std::move(n));
        }
        for (size_t i = 0; i < m.notes.size(); ++i) {
            LayeredGraph::Node n;
            n.id = "note" + std::to_string(i + 1);
            n.shape = kShapeNote;
            float tw = 0;
            for (const std::string& l : note_lines[i]) tw = std::max(tw, font.width(l, size));
            n.w = tw + 20 * u, n.h = static_cast<float>(note_lines[i].size()) * line + 14 * u;
            n.note_of = m.notes[i].of;
            if (n.note_of >= 0) n.cluster = m.classes[static_cast<size_t>(n.note_of)].ns;
            g->nodes.push_back(std::move(n));
        }
        for (const Relation& r : m.rels) {
            LayeredGraph::Edge e;
            e.from = r.a, e.to = r.b;
            e.label = r.label;
            if (r.a == r.b) loop_ends(k, r, g->dir, &e);
            g->edges.push_back(std::move(e));
        }
        end_rooms(k, m.rels, g->dir, g);
    };
    std::vector<WordSides> words;
    if (!lay_out_spread(&lg, style, m.dir, m.rels, k, &grow, &words, build, err)) return false;
    // Drawn.
    Drawing& d = *out;
    d.w = lg.w, d.h = lg.h;
    std::vector<int> order(lg.clusters.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    auto depth = [&](int c) {
        int n = 0;
        for (int p = lg.clusters[static_cast<size_t>(c)].parent; p >= 0 && n < 64; p = lg.clusters[static_cast<size_t>(p)].parent) ++n;
        return n;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth(a) < depth(b); });
    for (const int c : order) {
        draw_cluster(d, k, lg.clusters[static_cast<size_t>(c)], geo);
        if (geo) {
            const LayeredGraph::Cluster& cl = lg.clusters[static_cast<size_t>(c)];
            geo->boxes.push_back({"cluster", cl.id, cl.x, cl.y, cl.w, cl.h});
        }
    }
    std::vector<DiagramGeometry::Box> labels;
    if (geo) geo->edges = static_cast<int>(m.rels.size());  // (each relation should have a line)
    for (size_t i = 0; i < m.rels.size(); ++i) draw_relation(d, k, lg, lg.edges[i], m.rels[i], words[i], geo, geo ? &labels : nullptr);
    for (size_t i = 0; i < m.notes.size(); ++i) {
        const LayeredGraph::Node& n = lg.nodes[m.classes.size() + i];
        draw_note(d, k, lg, n, note_lines[i]);
        if (geo) geo->boxes.push_back({"node", n.id, n.x - n.w / 2, n.y - n.h / 2, n.w, n.h});
    }
    const float note_line = font.line(note_size);
    for (size_t i = 0; i < m.classes.size(); ++i) {
        const Class& c = m.classes[i];
        const ClassBox& b = boxes[i];
        const LayeredGraph::Node& n = lg.nodes[i];
        const float x = n.x - n.w / 2, y = n.y - n.h / 2;
        if (geo) geo->boxes.push_back({"node", c.id, x, y, n.w, n.h});
        const DiagramColour fill = !c.paint.fill.none() ? c.paint.fill : k.ink.fill;
        const DiagramColour band = !c.paint.fill.none() ? DiagramColour{} : k.ink.head;
        const DiagramColour stroke = !c.paint.stroke.none() ? c.paint.stroke : k.ink.stroke;
        const DiagramColour tc = !c.paint.text.none() ? c.paint.text : !c.paint.fill.none() ? readable_on(c.paint.fill, style.palette) : k.ink.text;
        const float sw = c.paint.width >= 0 ? c.paint.width * u : k.stroke_w;
        draw_box(d, x, y, n.w, n.h, b.head, 4 * u, fill, band, stroke, sw);
        // Its name, under its annotations.
        float base = y + py + (note_line - font.ascent(note_size) - font.descent(note_size)) / 2 + font.ascent(note_size);
        for (const std::string& t : b.notes) {
            say(d, t, n.x, base, note_size, !c.paint.text.none() ? tc : k.ink.quiet, font, 1, false);
            base += note_line;
        }
        base = y + py + static_cast<float>(b.notes.size()) * note_line + (line - font.ascent(size) - font.descent(size)) / 2 + font.ascent(size);
        const bool abstract = std::any_of(c.notes.begin(), c.notes.end(), [](const std::string& s) { return diagram_lower(s) == "abstract"; });
        for (const std::string& t : b.name) {
            say(d, t, n.x, base, size, tc, font, 1, true, abstract);
            base += line;
        }
        // The rules between its parts.
        const DiagramColour rule = stroke;
        for (const float ry : {y + b.head, y + b.head + b.mid}) {
            RasterPath p;
            p.move_to(x, ry), p.line_to(x + n.w, ry);
            d.shape(std::move(p), {}, rule, sw * 0.85f);
        }
        // Its members, from the left.
        auto rows = [&](const std::vector<Row>& list, float top) {
            float yy = top + py * 0.7f + (line - font.ascent(size) - font.descent(size)) / 2 + font.ascent(size);
            for (const Row& r : list) {
                say(d, r.text, x + px + r.indent, yy, size, tc, font, 0, false, r.is_abstract);
                if (r.is_static) {
                    RasterPath ul;
                    const float uy = yy + size * 0.14f, ux = x + px + r.indent;
                    ul.move_to(ux, uy), ul.line_to(ux + font.width(r.text, size), uy);
                    d.shape(std::move(ul), {}, tc, std::max(0.6f * u, size * 0.06f));
                }
                yy += line;
            }
        };
        rows(b.attrs, y + b.head);
        rows(b.methods, y + b.head + b.mid);
    }
    if (geo)
        for (DiagramGeometry::Box& b : labels) geo->boxes.push_back(std::move(b));
    return true;
}

// ---------------------------------------------------------------- entity relationship diagrams

struct Attr {
    std::string type, name, keys;
    std::vector<std::string> comment;
};

struct Entity {
    std::string id, label;
    std::vector<Attr> attrs;
    std::vector<std::string> css;
    Paint paint;
};

struct ErModel {
    std::vector<Entity> ents;
    std::map<std::string, int> ids;
    std::vector<Relation> rels;
    std::map<std::string, Paint> defs;
    std::vector<std::pair<std::string, Paint>> styles;
    char dir = 'D';
};

// An entity named in the text (NAME, "a name", p[Person], NAME:::css), made if new.
int entity_of(ErModel& m, std::string_view token) {
    std::string_view t = diagram_trim(token);
    std::string css;
    if (const size_t c = t.find(":::"); c != std::string_view::npos) css = diagram_trim(t.substr(c + 3)), t = diagram_trim(t.substr(0, c));
    std::string label;
    if (const size_t sq = t.find('['); sq != std::string_view::npos && t.ends_with(']') && !t.starts_with('"')) {
        label = unquote(t.substr(sq + 1, t.size() - sq - 2));
        t = diagram_trim(t.substr(0, sq));
    }
    const std::string id = unquote(t);
    int k;
    if (const auto it = m.ids.find(id); it != m.ids.end()) {
        k = it->second;
    } else {
        Entity e;
        e.id = id;
        e.label = id;
        m.ents.push_back(std::move(e));
        k = m.ids[id] = static_cast<int>(m.ents.size()) - 1;
    }
    if (!label.empty()) m.ents[static_cast<size_t>(k)].label = label;
    if (!css.empty()) m.ents[static_cast<size_t>(k)].css.push_back(css);
    return k;
}

// A cardinality's two marks (|o, ||, }o, }|; o|, ||, o{, |{).
u8 marks(std::string_view s) {
    if (s == "||") return kEndOne;
    if (s == "|o" || s == "o|") return kEndZeroOrOne;
    if (s == "}o" || s == "o{") return kEndZeroOrMore;
    if (s == "}|" || s == "|{") return kEndOneOrMore;
    return kEndNone;
}
// A cardinality in words (zero or one, one or more, only one, 1+, many(0), ...).
u8 said(std::string_view s) {
    const std::string l = diagram_lower(diagram_trim(s));
    if (l == "one or zero" || l == "zero or one") return kEndZeroOrOne;
    if (l == "one or more" || l == "one or many" || l == "many(1)" || l == "1+") return kEndOneOrMore;
    if (l == "zero or more" || l == "zero or many" || l == "many(0)" || l == "0+") return kEndZeroOrMore;
    if (l == "only one" || l == "exactly one" || l == "1") return kEndOne;
    return kEndNone;
}

bool parse_er(const std::vector<std::string>& st, ErModel& m, std::string* err) {
    int open = -1;  // the entity whose attributes are being read
    std::vector<std::pair<std::vector<std::string>, std::string>> css_of;
    size_t lines = 0;
    for (size_t k = 1; k < st.size(); ++k) {
        std::string_view s = st[k];
        const std::string l = diagram_lower(s);
        auto fail = [&](const std::string& why) {
            *err = "line " + std::to_string(k + 1) + ": " + why;
            return false;
        };
        if (++lines > kMostMembers || m.ents.size() > kMostBoxes || m.rels.size() > kMostRelations) return fail("too large to draw");
        if (open >= 0) {
            bool closes = false;
            if (s.ends_with('}')) closes = true, s = diagram_trim(s.substr(0, s.size() - 1));
            if (!s.empty()) {
                const std::vector<std::string> w = words(s);
                if (w.size() < 2) return fail("an attribute's type and name are wanted");
                Attr a;
                a.type = generics(w[0]);
                a.name = w[1];
                size_t i = 2;
                std::string keys;
                for (; i < w.size() && !w[i].starts_with('"'); ++i) {
                    std::string key = w[i];
                    if (key.ends_with(',')) key.pop_back();
                    if (key.empty()) continue;
                    const std::string up = diagram_lower(key);
                    if (up != "pk" && up != "fk" && up != "uk") return fail("PK, FK or UK is wanted at \"" + key + "\"");
                    if (!keys.empty()) keys += ", ";
                    for (char& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    keys += key;
                }
                a.keys = keys;
                if (i < w.size()) a.comment = diagram_label(unquote(w[i]));
                if (a.comment.size() == 1 && a.comment[0].empty()) a.comment.clear();
                m.ents[static_cast<size_t>(open)].attrs.push_back(std::move(a));
            }
            if (closes) open = -1;
            continue;
        }
        if (l.starts_with("direction ")) {
            m.dir = direction_of(l.substr(10));
            continue;
        }
        if (keyword(l, "acctitle") || keyword(l, "accdescr") || l.starts_with("title ") || l.starts_with("click ")) continue;
        if (l.starts_with("classdef ")) {
            const std::vector<std::string> w = words(s.substr(9));
            if (w.size() < 2) return fail("a classDef wants a name and a style");
            const Paint p = parse_paint(diagram_trim(s.substr(9 + s.substr(9).find(w[0]) + w[0].size())));
            size_t at = 0;
            for (size_t i = 0; i <= w[0].size(); ++i)
                if (i == w[0].size() || w[0][i] == ',') {
                    m.defs[std::string(diagram_trim(std::string_view(w[0]).substr(at, i - at)))] = p;
                    at = i + 1;
                }
            continue;
        }
        if (l.starts_with("class ")) {
            const std::vector<std::string> w = words(s.substr(6));
            if (w.size() < 2) return fail("a class wants entities and a name");
            std::vector<std::string> who;
            size_t at = 0;
            for (size_t i = 0; i <= w[0].size(); ++i)
                if (i == w[0].size() || w[0][i] == ',') {
                    who.emplace_back(diagram_trim(std::string_view(w[0]).substr(at, i - at)));
                    at = i + 1;
                }
            css_of.push_back({who, w[1]});
            continue;
        }
        if (l.starts_with("style ")) {
            const std::vector<std::string> w = words(s.substr(6));
            if (w.size() < 2) return fail("a style wants an entity and a style");
            m.styles.push_back({unquote(w[0]), parse_paint(diagram_trim(s.substr(6 + s.substr(6).find(w[0]) + w[0].size())))});
            continue;
        }
        // An entity's attributes: NAME { ... }
        if (const size_t brace = find_bare(s, '{'); brace != std::string_view::npos && words(s.substr(0, brace)).size() == 1) {
            const std::string_view name = diagram_trim(s.substr(0, brace));
            if (name.empty()) return fail("an entity's name is wanted before {");
            open = entity_of(m, name);
            std::string_view rest = diagram_trim(s.substr(brace + 1));
            if (rest.ends_with('}')) {
                rest = diagram_trim(rest.substr(0, rest.size() - 1));
                if (!rest.empty()) return fail("attributes go on lines of their own");
                open = -1;
            }
            continue;
        }
        // A relation: A ||--o{ B : label (or in words: A one or more to zero or one B : label).
        std::string_view head = s;
        Relation r;
        if (const size_t colon = label_colon(s); colon != std::string_view::npos) {
            r.label = diagram_label(unquote(s.substr(colon + 1)));
            if (r.label.size() == 1 && r.label[0].empty()) r.label.clear();
            head = diagram_trim(s.substr(0, colon));
        }
        const std::vector<std::string> w = words(head);
        if (w.size() == 1) {  // (an entity alone)
            entity_of(m, w[0]);
            continue;
        }
        if (w.size() == 3) {
            const std::string& rel = w[1];
            if (rel.size() != 6 || !(rel.substr(2, 2) == "--" || rel.substr(2, 2) == "..")) return fail("a relation like ||--o{ is wanted at \"" + rel + "\"");
            r.end_a = marks(rel.substr(0, 2));
            r.end_b = marks(rel.substr(4, 2));
            r.dashed = rel.substr(2, 2) == "..";
            if (r.end_a == kEndNone || r.end_b == kEndNone) return fail("a relation like ||--o{ is wanted at \"" + rel + "\"");
        } else if (w.size() >= 5) {
            std::string mid;
            for (size_t i = 1; i + 1 < w.size(); ++i) mid += (i > 1 ? " " : "") + diagram_lower(w[i]);
            size_t to = mid.find(" optionally to ");
            size_t skip = 15;
            r.dashed = to != std::string::npos;
            if (to == std::string::npos) to = mid.find(" to "), skip = 4;
            if (to == std::string::npos) return fail("\"to\" is wanted between two cardinalities");
            r.end_a = said(mid.substr(0, to));
            r.end_b = said(mid.substr(to + skip));
            if (r.end_a == kEndNone || r.end_b == kEndNone) return fail("a cardinality like \"one or more\" is wanted");
        } else {
            return fail("a relation or an entity is wanted at \"" + std::string(s.substr(0, 24)) + "\"");
        }
        r.a = entity_of(m, w.front());
        r.b = entity_of(m, w.back());
        m.rels.push_back(std::move(r));
    }
    for (const auto& [who, name] : css_of)
        for (const std::string& id : who)
            if (const auto it = m.ids.find(id); it != m.ids.end()) m.ents[static_cast<size_t>(it->second)].css.push_back(name);
    const auto def = m.defs.find("default");
    for (Entity& e : m.ents) {
        Paint p;
        if (def != m.defs.end()) p.over(def->second);
        for (const std::string& n : e.css)
            if (const auto it = m.defs.find(n); it != m.defs.end()) p.over(it->second);
        e.paint = p;
    }
    for (const auto& [id, p] : m.styles)
        if (const auto it = m.ids.find(id); it != m.ids.end()) m.ents[static_cast<size_t>(it->second)].paint.over(p);
    return true;
}

// An entity's box: its columns' widths (type, name, keys, comment), each
// row's height.
struct EntityBox {
    std::vector<std::string> name;
    float col[4] = {};
    std::vector<std::vector<std::string>> comments;
    std::vector<float> row_h;
    float w = 0, head = 0, h = 0;
};

bool erdiagram(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    ErModel m;
    if (!parse_er(st, m, err)) return false;
    if (m.ents.empty()) {
        *err = "nothing in it";
        return false;
    }
    const DiagramFont& font = *style.font;
    Look k;
    k.style = &style;
    k.ink = inks(style.palette);
    k.u = style.unit(), k.size = style.size, k.line = font.line(style.size);
    k.stroke_w = 1.3f * k.u, k.edge_w = 1.4f * k.u;
    const float u = k.u, size = k.size, line = k.line;
    const float px = 14 * u, py = 9 * u, cell = 9 * u, rp = 4 * u;
    const float wrap = std::max(font.width("abcdefghijklmnopqrstuvwxyz", size) * 1.0f, 160 * u);
    std::vector<EntityBox> boxes(m.ents.size());
    for (size_t i = 0; i < m.ents.size(); ++i) {
        const Entity& e = m.ents[i];
        EntityBox& b = boxes[i];
        b.name = diagram_wrap(diagram_label(e.label), wrap, size, font);
        float name_w = 0;
        for (const std::string& n : b.name) name_w = std::max(name_w, font.width(n, size, true));
        b.head = py * 2 + static_cast<float>(b.name.size()) * line;
        bool keys = false, comments = false;
        for (const Attr& a : e.attrs) keys = keys || !a.keys.empty(), comments = comments || !a.comment.empty();
        float table = 0;
        if (!e.attrs.empty()) {
            for (const Attr& a : e.attrs) {
                b.comments.push_back(diagram_wrap(a.comment, wrap, size * 0.92f, font));
                b.col[0] = std::max(b.col[0], font.width(a.type, size));
                b.col[1] = std::max(b.col[1], font.width(a.name, size));
                b.col[2] = std::max(b.col[2], font.width(a.keys, size * 0.92f, true));
                for (const std::string& c : b.comments.back()) b.col[3] = std::max(b.col[3], font.width(c, size * 0.92f));
                b.row_h.push_back(2 * rp + static_cast<float>(std::max<size_t>(1, b.comments.back().size())) * line);
            }
            for (int c = 0; c < 4; ++c)
                if (b.col[c] > 0 || c < 2) b.col[c] += 2 * cell;
            if (!keys) b.col[2] = 0;
            if (!comments) b.col[3] = 0;
            table = b.col[0] + b.col[1] + b.col[2] + b.col[3];
        }
        b.w = std::max({table, name_w + 2 * px, 90 * u});
        if (table > 0 && table < b.w) {  // (the columns share what the name needs more)
            const float more = (b.w - table) / static_cast<float>(1 + (b.col[2] > 0) + (b.col[3] > 0) + 1);
            for (int c = 0; c < 4; ++c)
                if (b.col[c] > 0) b.col[c] += more;
        }
        b.h = b.head;
        for (const float h : b.row_h) b.h += h;
        if (e.attrs.empty()) b.h = b.head = std::max(b.head, 44 * u), b.w = std::max(b.w, 100 * u);
    }
    LayeredGraph lg;
    std::vector<DiagramVec> grow;
    auto build = [&](LayeredGraph* g) {
        g->nodes.clear(), g->edges.clear();
        g->sep = 2.1f;
        for (size_t i = 0; i < m.ents.size(); ++i) {
            LayeredGraph::Node n;
            n.id = m.ents[i].id;
            n.w = boxes[i].w, n.h = boxes[i].h;
            if (i < grow.size()) n.w = std::max(n.w, grow[i].x), n.h = std::max(n.h, grow[i].y);
            g->nodes.push_back(std::move(n));
        }
        for (const Relation& r : m.rels) {
            LayeredGraph::Edge e;
            e.from = r.a, e.to = r.b;
            e.label = r.label;
            if (r.a == r.b) loop_ends(k, r, g->dir, &e);
            g->edges.push_back(std::move(e));
        }
        end_rooms(k, m.rels, g->dir, g);
    };
    std::vector<WordSides> words;
    if (!lay_out_spread(&lg, style, m.dir, m.rels, k, &grow, &words, build, err)) return false;
    Drawing& d = *out;
    d.w = lg.w, d.h = lg.h;
    std::vector<DiagramGeometry::Box> labels;
    if (geo) geo->edges = static_cast<int>(m.rels.size());  // (each relation should have a line)
    for (size_t i = 0; i < m.rels.size(); ++i) draw_relation(d, k, lg, lg.edges[i], m.rels[i], words[i], geo, geo ? &labels : nullptr);
    const float small = size * 0.92f;
    for (size_t i = 0; i < m.ents.size(); ++i) {
        const Entity& e = m.ents[i];
        const EntityBox& b = boxes[i];
        const LayeredGraph::Node& n = lg.nodes[i];
        const float x = n.x - n.w / 2, y = n.y - n.h / 2, r = 4 * u;
        // (its columns share what it was made wider by, for the ends on it)
        float col[4] = {b.col[0], b.col[1], b.col[2], b.col[3]};
        if (n.w > b.w + 0.01f && !e.attrs.empty()) {
            const float more = (n.w - b.w) / static_cast<float>((col[0] > 0) + (col[1] > 0) + (col[2] > 0) + (col[3] > 0));
            for (float& c : col)
                if (c > 0) c += more;
        }
        if (geo) geo->boxes.push_back({"node", e.id, x, y, n.w, n.h});
        const bool own = !e.paint.fill.none();
        const DiagramColour stroke = !e.paint.stroke.none() ? e.paint.stroke : k.ink.stroke;
        const DiagramColour tc = !e.paint.text.none() ? e.paint.text : own ? readable_on(e.paint.fill, style.palette) : k.ink.text;
        const float sw = e.paint.width >= 0 ? e.paint.width * u : k.stroke_w;
        RasterPath body;
        body.rect(x, y, n.w, n.h, r);
        d.shape(std::move(body), own ? e.paint.fill : k.ink.row_odd);
        // Its rows, in turn a little stronger, under its name's band.
        float ry = y + b.head;
        for (size_t j = 0; j < e.attrs.size(); ++j) {
            const float h = b.row_h[j];
            if (j % 2 == 1 && !own) {
                RasterPath row;
                const bool last = j + 1 == e.attrs.size();
                if (last) {  // (the bottom corners rounded as the box's are)
                    row.move_to(x, ry);
                    row.line_to(x + n.w, ry);
                    row.arc(x + n.w - r, ry + h - r, r, r, 0, 1.57079633f);
                    row.arc(x + r, ry + h - r, r, r, 1.57079633f, 3.14159265f);
                    row.close();
                } else {
                    row.rect(x, ry, n.w, h);
                }
                d.shape(std::move(row), k.ink.row_even);
            }
            ry += h;
        }
        RasterPath band;
        if (e.attrs.empty()) {  // (all of it its name)
            band.rect(x, y, n.w, n.h, r);
        } else {
            band.move_to(x, y + b.head);
            band.arc(x + r, y + r, r, r, 3.14159265f, 4.71238898f);
            band.arc(x + n.w - r, y + r, r, r, 4.71238898f, 6.28318531f);
            band.line_to(x + n.w, y + b.head);
            band.close();
        }
        d.shape(std::move(band), own ? e.paint.fill : k.ink.head);
        // The columns' lines, the name's rule.
        if (!e.attrs.empty()) {
            RasterPath cols;
            float cx = x;
            for (int c = 0; c < 3; ++c) {
                cx += col[c];
                if (col[c] <= 0 || cx >= x + n.w - 1) continue;
                bool after = false;
                for (int q = c + 1; q < 4; ++q) after = after || col[q] > 0;
                if (!after) continue;
                cols.move_to(cx, y + b.head), cols.line_to(cx, y + n.h);
            }
            if (!cols.empty()) d.shape(std::move(cols), {}, k.ink.rule, sw * 0.7f);
            RasterPath rule;
            rule.move_to(x, y + b.head), rule.line_to(x + n.w, y + b.head);
            d.shape(std::move(rule), {}, stroke, sw * 0.85f);
        }
        RasterPath outline;
        outline.rect(x, y, n.w, n.h, r);
        d.shape(std::move(outline), {}, stroke, sw);
        // Its name, its attributes.
        float base = y + (b.head - static_cast<float>(b.name.size()) * line) / 2 + (line - font.ascent(size) - font.descent(size)) / 2 + font.ascent(size);
        for (const std::string& t : b.name) {
            say(d, t, n.x, base, size, tc, font, 1, true);
            base += line;
        }
        ry = y + b.head;
        for (size_t j = 0; j < e.attrs.size(); ++j) {
            const Attr& a = e.attrs[j];
            const float yy = ry + rp + (line - font.ascent(size) - font.descent(size)) / 2 + font.ascent(size);
            float cx = x + cell;
            say(d, a.type, cx, yy, size, own ? tc : k.ink.quiet, font, 0, false);
            cx += col[0];
            say(d, a.name, cx, yy, size, tc, font, 0, false);
            cx += col[1];
            if (col[2] > 0) {
                say(d, a.keys, cx, yy, small, own ? tc : k.ink.key, font, 0, true);
                cx += col[2];
            }
            float cy = yy;
            for (const std::string& c : b.comments[j]) {
                say(d, c, cx, cy, small, own ? tc : k.ink.quiet, font, 0, false, true);
                cy += line;
            }
            ry += b.row_h[j];
        }
    }
    if (geo)
        for (DiagramGeometry::Box& bx : labels) geo->boxes.push_back(std::move(bx));
    return true;
}

}  // namespace

bool classdiagram_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    return classdiagram(st, style, out, err, geo);
}

bool erdiagram_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    return erdiagram(st, style, out, err, geo);
}

}  // namespace blit
