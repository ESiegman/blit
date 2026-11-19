// Flowcharts (graph, flowchart) and state diagrams, laid out in layers
// (Sugiyama's way, as Mermaid's own dagre does): nodes ranked so that
// edges go one way, ordered within their ranks to cross as little as they
// can, placed across in balance with their neighbours, and joined by
// smooth curves through points kept for them between the ranks (an
// edge's label is one of those points, with room for it). Subgraphs and
// composite states are laid out first, each as a block, and then take
// their place in the layout around them as one node; an edge that goes in
// or out of one passes through a port on its border, kept a way clear to
// the node inside.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

#include "diagram.hpp"

namespace blit {

namespace {

constexpr size_t kMostNodes = 1500, kMostEdges = 3000;
// Subgraphs (and composite states) within each other at most so deep: each
// is laid out by a call within its parent's, on the preview's thread, whose
// stack is the system's (512 KB on a Mac). Deeper than any diagram a person
// draws.
constexpr int kDeepest = 32;

// What an edge joins: a node (from 0), or a subgraph (cluster c is -(c + 2)).
int as_cluster(int c) { return -(c + 2); }
constexpr int kNowhere = -(1 << 30);  // (what item_of() says of a reference not in the level)
bool is_cluster(int ref) { return ref <= -2; }
int cluster_of(int ref) { return -ref - 2; }

enum Line : u8 { kSolid, kThick, kDotted, kInvisible };

struct Style {
    DiagramColour fill, stroke, text;
    float width = -1;  // Mermaid's pixels (at 16 pixel text)
    std::vector<float> dash;
    bool bold = false;
    void over(const Style& o) {
        if (!o.fill.none()) fill = o.fill;
        if (!o.stroke.none()) stroke = o.stroke;
        if (!o.text.none()) text = o.text;
        if (o.width >= 0) width = o.width;
        if (!o.dash.empty()) dash = o.dash;
        bold = bold || o.bold;
    }
};

// CSS as Mermaid takes it: fill:#f9f,stroke:#333,stroke-width:4px,color:#fff,stroke-dasharray: 5 5.
Style parse_css(std::string_view css) {
    Style st;
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (const char c : css) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if ((c == ',' || c == ';') && depth <= 0) {
            parts.push_back(cur), cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(cur);
    std::string last;
    for (const std::string& part : parts) {
        const size_t colon = part.find(':');
        if (colon == std::string::npos) {
            if (last == "stroke-dasharray") {  // (5,5: the comma split it)
                const float v = std::strtof(std::string(diagram_trim(part)).c_str(), nullptr);
                if (v >= 0 && v < 1000) st.dash.push_back(v);
            }
            continue;
        }
        const std::string key = diagram_lower(diagram_trim(std::string_view(part).substr(0, colon)));
        std::string_view value = diagram_trim(std::string_view(part).substr(colon + 1));
        if (value.ends_with("!important")) value = diagram_trim(value.substr(0, value.size() - 10));
        last = key;
        if (key == "fill" || key == "background" || key == "background-color") diagram_colour(value, &st.fill);
        else if (key == "stroke" || key == "border-color") diagram_colour(value, &st.stroke);
        else if (key == "color") diagram_colour(value, &st.text);
        else if (key == "stroke-width" || key == "border-width") {
            const float v = std::strtof(std::string(value).c_str(), nullptr);
            if (v >= 0 && v < 100) st.width = v;
        } else if (key == "stroke-dasharray") {
            std::string v(value);
            for (char& c : v)
                if (c == ',') c = ' ';
            const char* p = v.c_str();
            for (;;) {
                char* end = nullptr;
                const float x = std::strtof(p, &end);
                if (end == p) break;
                if (x >= 0 && x < 1000) st.dash.push_back(x);
                p = end;
            }
        } else if (key == "font-weight") {
            st.bold = value == "bold" || (value.size() >= 3 && value[0] >= '6');
        }
    }
    return st;
}

struct Node {
    std::string id;
    std::vector<std::string> lines;
    u8 shape = kShapeRect;
    bool given = false;  // its text or shape was said (not only its name used)
    bool dead = false;   // its name turned out to be a subgraph's
    int cluster = -1;    // the innermost subgraph it is in
    std::vector<std::string> classes;
    Style style;
    int note_of = -1;  // a note: what it is beside
    bool note_left = false;
    bool loop = false;  // an edge goes from it to itself
    float loop_room = 0;
    float w = 0, h = 0;  // laid out: its size, and its middle (in its level's box, then in the whole)
    float x = 0, y = 0;
};

struct Seg {
    int level = -1, depth = 0;
    std::vector<DiagramVec> pts;
};

struct Edge {
    int from = 0, to = 0;
    std::vector<std::string> label;
    u8 line = kSolid, head_from = kHeadNone, head_to = kHeadArrow;
    int minlen = 1;
    Style style;
    // Laid out: its ends in the order of the ranks (a to b), whether that
    // is backwards, the direction of the level it is in, and its parts in
    // each level it passes through (in their boxes).
    int a = 0, b = 0;
    bool reversed = false, laid = false;
    char dir = 'D';
    Seg mid;
    std::vector<Seg> out_a, in_b;
    int label_level = -1;
    DiagramVec label_at;
    float label_w = 0, label_h = 0;
};

struct Cluster {
    std::string id;
    std::vector<std::string> title;
    int parent = -1;
    char dir = 0;  // its own direction, if it says one
    Style style;
    std::vector<std::string> classes;
    bool state = false;  // a composite state (drawn with its name in a band)
    float x = 0, y = 0, w = 0, h = 0;  // its box: in its parent's box, then in the whole
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<Cluster> clusters;
    std::map<std::string, int> ids, cluster_ids;
    std::map<std::string, Style> classes;
    std::vector<std::pair<std::vector<std::string>, std::string>> class_of;  // class a,b name: applied once all are known
    std::vector<std::pair<std::string, Style>> styles;  // style id css
    std::vector<std::pair<std::vector<int>, Style>> link_styles;
    Style link_default;
    char dir = 'D';
    bool state = false;

    int node(const std::string& id, int cluster) {
        if (const auto it = ids.find(id); it != ids.end()) {
            Node& n = nodes[static_cast<size_t>(it->second)];
            if (n.cluster < 0 && cluster >= 0) n.cluster = cluster;  // (named inside a subgraph: it is in it)
            return it->second;
        }
        Node n;
        n.id = id;
        n.lines = {id};
        n.cluster = cluster;
        nodes.push_back(std::move(n));
        return ids[id] = static_cast<int>(nodes.size()) - 1;
    }
};

char direction(std::string_view d) {
    const std::string l = diagram_lower(diagram_trim(d));
    return l.starts_with("lr") ? 'R' : l.starts_with("rl") ? 'L' : l.starts_with("bt") ? 'U' : 'D';
}

// ---------------------------------------------------------------- flowcharts

bool id_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80; }

// Mermaid's newer shapes by name: A@{ shape: diam, label: "..." }.
u8 shape_named(std::string_view name) {
    static const struct {
        const char* name;
        u8 shape;
    } kNames[] = {{"rect", kShapeRect},           {"rectangle", kShapeRect},       {"proc", kShapeRect},          {"process", kShapeRect},
                  {"rounded", kShapeRound},       {"event", kShapeRound},          {"stadium", kShapeStadium},     {"pill", kShapeStadium},
                  {"terminal", kShapeStadium},    {"subproc", kShapeSubroutine},   {"subprocess", kShapeSubroutine}, {"subroutine", kShapeSubroutine},
                  {"fr-rect", kShapeSubroutine},  {"framed-rectangle", kShapeSubroutine}, {"cyl", kShapeCylinder}, {"cylinder", kShapeCylinder},
                  {"database", kShapeCylinder},   {"db", kShapeCylinder},          {"circle", kShapeCircle},      {"circ", kShapeCircle},
                  {"dbl-circ", kShapeDoubleCircle}, {"double-circle", kShapeDoubleCircle}, {"diam", kShapeDiamond}, {"diamond", kShapeDiamond},
                  {"decision", kShapeDiamond},    {"question", kShapeDiamond},     {"hex", kShapeHexagon},        {"hexagon", kShapeHexagon},
                  {"prepare", kShapeHexagon},     {"lean-r", kShapeLeanRight},     {"lean-right", kShapeLeanRight}, {"in-out", kShapeLeanRight},
                  {"lean-l", kShapeLeanLeft},     {"lean-left", kShapeLeanLeft},   {"out-in", kShapeLeanLeft},    {"trap-b", kShapeTrapezoid},
                  {"trapezoid-bottom", kShapeTrapezoid}, {"priority", kShapeTrapezoid}, {"trap-t", kShapeTrapezoidAlt}, {"trapezoid-top", kShapeTrapezoidAlt},
                  {"manual", kShapeTrapezoidAlt}, {"odd", kShapeAsymmetric},      {"flag", kShapeAsymmetric},     {"sm-circ", kShapeStart},
                  {"small-circle", kShapeStart},  {"start", kShapeStart},          {"fr-circ", kShapeEnd},        {"stop", kShapeEnd},
                  {"framed-circle", kShapeEnd},   {"fork", kShapeBar},             {"join", kShapeBar}};
    const std::string l = diagram_lower(diagram_trim(name));
    for (const auto& n : kNames)
        if (l == n.name) return n.shape;
    return kShapeRect;
}

// A node (its name, and its shape and text if they follow) at s[i].
bool parse_node(std::string_view s, size_t& i, Graph& g, int cluster, int* out, std::string* err) {
    while (i < s.size() && s[i] == ' ') ++i;
    const size_t start = i;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '-' && i + 1 < s.size() && (s[i + 1] == '-' || s[i + 1] == '.' || s[i + 1] == '>')) break;
        if (id_char(c) || (c == '-' && i > start)) ++i;
        else break;
    }
    if (i == start) {
        *err = "a node's name is wanted at \"" + std::string(s.substr(start, 12)) + "\"";
        return false;
    }
    std::string id(s.substr(start, i - start));
    std::vector<std::string> classes;
    if (const size_t cls = id.find(":::"); cls != std::string::npos) {
        classes.push_back(id.substr(cls + 3));
        id.resize(cls);
    }
    const int n = g.node(id, cluster);
    Node& nd = g.nodes[static_cast<size_t>(n)];
    struct Opener {
        const char* open;
        const char* close;
        u8 shape;
    };
    static const Opener kOpeners[] = {{"(((", ")))", kShapeDoubleCircle}, {"((", "))", kShapeCircle},  {"([", "])", kShapeStadium},  {"[[", "]]", kShapeSubroutine},
                                      {"[(", ")]", kShapeCylinder},       {"{{", "}}", kShapeHexagon}, {"[/", "/]", kShapeLeanRight}, {"[\\", "\\]", kShapeLeanLeft},
                                      {"[/", "\\]", kShapeTrapezoid},     {"[\\", "/]", kShapeTrapezoidAlt}, {"[", "]", kShapeRect}, {"(", ")", kShapeRound},
                                      {"{", "}", kShapeDiamond},          {">", "]", kShapeAsymmetric}};
    if (s.substr(i).starts_with("@{")) {  // A@{ shape: ..., label: "..." }
        const size_t close = s.find('}', i);
        if (close == std::string_view::npos) {
            *err = "a } is wanted after \"" + std::string(s.substr(i, 12)) + "\"";
            return false;
        }
        const std::string_view body = s.substr(i + 2, close - i - 2);
        size_t at = 0;
        while (at < body.size()) {
            const size_t colon = body.find(':', at);
            if (colon == std::string_view::npos) break;
            const std::string key = diagram_lower(diagram_trim(body.substr(at, colon - at)));
            size_t end = colon + 1;
            while (end < body.size() && body[end] == ' ') ++end;
            if (end < body.size() && body[end] == '"') end = body.find('"', end + 1);
            end = end == std::string_view::npos ? body.size() : body.find(',', end);
            if (end == std::string_view::npos) end = body.size();
            const std::string_view value = diagram_trim(body.substr(colon + 1, end - colon - 1));
            if (key == "shape") nd.shape = shape_named(value), nd.given = true;
            if (key == "label") nd.lines = diagram_label(value), nd.given = true;
            at = end + 1;
        }
        i = close + 1;
    }
    // (Of the shapes that open so, the one that closes first: [/a\] is a
    // trapezoid even when a /] comes later in the line.)
    const Opener* best = nullptr;
    size_t best_end = std::string_view::npos, best_from = 0;
    for (const Opener& o : kOpeners) {
        if (!s.substr(i).starts_with(o.open)) continue;
        if (best && std::string_view(o.open).size() < std::string_view(best->open).size()) break;
        const size_t from = i + std::string_view(o.open).size();
        size_t end;
        if (from < s.size() && s[from] == '"') {
            const size_t q = s.find('"', from + 1);
            if (q == std::string_view::npos) break;
            end = s.find(o.close, q + 1);
        } else {
            end = s.find(o.close, from);
        }
        if (end < best_end) best = &o, best_end = end, best_from = from;
    }
    if (best) {
        nd.lines = diagram_label(s.substr(best_from, best_end - best_from));
        nd.shape = best->shape;
        nd.given = true;
        i = best_end + std::string_view(best->close).size();
    }
    if (s.substr(i).starts_with(":::")) {
        i += 3;
        const size_t from = i;
        while (i < s.size() && (id_char(s[i]) || s[i] == '-')) ++i;
        classes.emplace_back(s.substr(from, i - from));
    }
    for (std::string& c : classes)
        if (!c.empty()) g.nodes[static_cast<size_t>(n)].classes.push_back(std::move(c));
    *out = n;
    return true;
}

// An arrow at s[i] (with what it says, if it says anything).
bool parse_edge(std::string_view s, size_t& i, Edge* e) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    // (an edge's own name, e1@-->, is passed over)
    for (size_t k = j; k < s.size() && k < j + 40; ++k) {
        if (s[k] == '@' && k > j && k + 1 < s.size() && std::string_view("-=~<ox").find(s[k + 1]) != std::string_view::npos) {
            j = k + 1;
            break;
        }
        if (!id_char(s[k]) && s[k] != '_') break;
    }
    if (j < s.size() && (s[j] == '<' || ((s[j] == 'o' || s[j] == 'x') && j + 1 < s.size() && (s[j + 1] == '-' || s[j + 1] == '=')))) {
        e->head_from = s[j] == '<' ? kHeadArrow : s[j] == 'o' ? kHeadCircle : kHeadCross;
        ++j;
    }
    if (j + 1 >= s.size() || std::string_view("-=~").find(s[j]) == std::string_view::npos) return false;
    // "-- text -->", "-. text .->", "== text ==>": the text between.
    const std::string_view two = s.substr(j, 2);
    bool dotted_text = false;
    std::string label;
    if ((two == "--" || two == "-." || two == "==") && j + 2 < s.size() && s[j + 2] == ' ') {
        const std::string_view ends = two == "--" ? "--" : two == "-." ? ".-" : "==";
        const size_t close = s.find(ends, j + 3);
        if (close != std::string_view::npos && close > j + 3) {
            label = std::string(diagram_trim(s.substr(j + 2, close - j - 2)));
            j = close;
            if (two == "-.") ++j, dotted_text = true;  // (".-": the dot)
        }
    }
    const size_t body = j;
    while (j < s.size() && std::string_view("-=.~").find(s[j]) != std::string_view::npos) ++j;
    const std::string_view run = s.substr(body, j - body);
    if (run.size() < 2 && !(run == "-" && !label.empty())) return false;
    const bool thick = run.find('=') != std::string_view::npos;
    const bool dotted = dotted_text || run.find('.') != std::string_view::npos;
    e->line = run.find('~') != std::string_view::npos ? kInvisible : thick ? kThick : dotted ? kDotted : kSolid;
    e->head_to = kHeadNone;
    if (j < s.size() && (s[j] == '>' || ((s[j] == 'o' || s[j] == 'x') && (j + 1 >= s.size() || !id_char(s[j + 1]))))) {
        e->head_to = s[j] == '>' ? kHeadArrow : s[j] == 'o' ? kHeadCircle : kHeadCross;
        ++j;
    }
    // How long it is: each dash (dot, for a dotted one) more than the
    // fewest adds a rank.
    if (dotted) e->minlen = std::max<int>(1, static_cast<int>(std::count(run.begin(), run.end(), '.')));
    else e->minlen = std::max(1, static_cast<int>(run.size()) - (e->head_to != kHeadNone ? 1 : 2));
    e->minlen = std::min(e->minlen, 8);
    while (j < s.size() && s[j] == ' ') ++j;
    if (j < s.size() && s[j] == '|') {
        const size_t close = s.find('|', j + 1);
        if (close != std::string_view::npos) {
            label = std::string(diagram_trim(s.substr(j + 1, close - j - 1)));
            j = close + 1;
        }
    }
    if (!label.empty()) e->label = diagram_label(label);
    i = j;
    return true;
}

bool parse_flowchart(const std::vector<std::string>& st, Graph& g, std::string* err) {
    {
        const std::string& head = st[0];
        const size_t sp = head.find(' ');
        g.dir = sp == std::string::npos ? 'D' : direction(std::string_view(head).substr(sp + 1));
    }
    std::vector<int> open;  // the subgraphs being read
    for (size_t k = 1; k < st.size(); ++k) {
        const std::string_view s = st[k];
        const std::string l = diagram_lower(s);
        const int cluster = open.empty() ? -1 : open.back();
        auto fail = [&](const std::string& why) {
            *err = "line " + std::to_string(k + 1) + ": " + why;
            return false;
        };
        if (l == "end") {
            if (!open.empty()) open.pop_back();
            continue;
        }
        if (l.starts_with("click ") || l.starts_with("acctitle") || l.starts_with("accdescr") || l.starts_with("title ")) continue;
        if (l.starts_with("direction ")) {
            if (cluster >= 0) g.clusters[static_cast<size_t>(cluster)].dir = direction(l.substr(10));
            else g.dir = direction(l.substr(10));
            continue;
        }
        if (l.starts_with("classdef ")) {
            std::string_view rest = diagram_trim(s.substr(9));
            const size_t sp = rest.find(' ');
            const Style css = parse_css(sp == std::string_view::npos ? std::string_view() : rest.substr(sp + 1));
            std::string_view names = rest.substr(0, sp);
            for (size_t at = 0; at <= names.size();) {
                const size_t c = std::min(names.find(',', at), names.size());
                if (c > at) g.classes[std::string(diagram_trim(names.substr(at, c - at)))].over(css);
                at = c + 1;
            }
            continue;
        }
        if (l.starts_with("class ")) {
            std::string_view rest = diagram_trim(s.substr(6));
            const size_t sp = rest.rfind(' ');
            if (sp == std::string_view::npos) continue;
            std::vector<std::string> ids;
            const std::string_view list = rest.substr(0, sp);
            for (size_t at = 0; at <= list.size();) {
                const size_t c = std::min(list.find(',', at), list.size());
                if (c > at) ids.emplace_back(diagram_trim(list.substr(at, c - at)));
                at = c + 1;
            }
            g.class_of.push_back({ids, std::string(diagram_trim(rest.substr(sp + 1)))});
            continue;
        }
        if (l.starts_with("style ")) {
            std::string_view rest = diagram_trim(s.substr(6));
            const size_t sp = rest.find(' ');
            if (sp != std::string_view::npos) g.styles.push_back({std::string(rest.substr(0, sp)), parse_css(rest.substr(sp + 1))});
            continue;
        }
        if (l.starts_with("linkstyle ")) {
            std::string_view rest = diagram_trim(s.substr(10));
            const size_t sp = rest.find(' ');
            if (sp == std::string_view::npos) continue;
            const Style css = parse_css(rest.substr(sp + 1));
            const std::string_view which = rest.substr(0, sp);
            if (diagram_lower(which) == "default") {
                g.link_default.over(css);
                continue;
            }
            std::vector<int> nums;
            for (size_t at = 0; at <= which.size();) {
                const size_t c = std::min(which.find(',', at), which.size());
                if (c > at) nums.push_back(std::atoi(std::string(which.substr(at, c - at)).c_str()));
                at = c + 1;
            }
            g.link_styles.push_back({nums, css});
            continue;
        }
        if (l == "subgraph" || l.starts_with("subgraph ")) {
            if (open.size() >= static_cast<size_t>(kDeepest)) return fail("subgraphs nested too deeply");
            // subgraph id, subgraph id [title], subgraph id["title"], subgraph "title", subgraph a title
            std::string_view rest = diagram_trim(s.substr(8));
            Cluster c;
            c.parent = cluster;
            const size_t bracket = rest.find('[');
            if (rest.starts_with('"')) {
                c.title = diagram_label(rest);
                c.id = c.title.empty() ? std::string() : c.title[0];
            } else if (bracket != std::string_view::npos && rest.ends_with(']')) {
                c.id = std::string(diagram_trim(rest.substr(0, bracket)));
                c.title = diagram_label(rest.substr(bracket + 1, rest.size() - bracket - 2));
            } else {
                c.id = std::string(rest);
                c.title = {std::string(rest)};
            }
            if (c.id.empty()) c.id = "subgraph" + std::to_string(g.clusters.size() + 1);
            if (const size_t cls = c.id.find(":::"); cls != std::string::npos) {
                c.classes.push_back(c.id.substr(cls + 3));
                c.id.resize(cls);
            }
            g.clusters.push_back(std::move(c));
            const int at = static_cast<int>(g.clusters.size()) - 1;
            g.cluster_ids.emplace(g.clusters.back().id, at);
            open.push_back(at);
            continue;
        }
        size_t i = 0;
        std::vector<int> left;
        auto group = [&](std::vector<int>& out) {
            out.clear();
            for (;;) {
                int n = 0;
                if (!parse_node(s, i, g, cluster, &n, err)) return false;
                out.push_back(n);
                while (i < s.size() && s[i] == ' ') ++i;
                if (i < s.size() && s[i] == '&') {
                    ++i;
                    continue;
                }
                return true;
            }
        };
        if (!group(left)) return fail(*err);
        for (;;) {
            while (i < s.size() && s[i] == ' ') ++i;
            if (i >= s.size()) break;
            Edge e;
            if (!parse_edge(s, i, &e)) return fail("an arrow is wanted at \"" + std::string(s.substr(i, 12)) + "\"");
            std::vector<int> right;
            if (!group(right)) return fail(*err);
            for (const int a : left)
                for (const int b : right) {
                    Edge ed = e;
                    ed.from = a, ed.to = b;
                    g.edges.push_back(std::move(ed));
                }
            left = right;
        }
        if (g.nodes.size() > kMostNodes || g.edges.size() > kMostEdges) return fail("too large to draw");
    }
    // Names that are subgraphs': an edge to one goes to its box.
    std::vector<int> as(g.nodes.size(), -1);
    for (size_t n = 0; n < g.nodes.size(); ++n)
        if (const auto it = g.cluster_ids.find(g.nodes[n].id); it != g.cluster_ids.end() && !g.nodes[n].given) {
            as[n] = as_cluster(it->second);
            g.nodes[n].dead = true;
        }
    for (Edge& e : g.edges) {
        if (e.from >= 0 && as[static_cast<size_t>(e.from)] != -1) e.from = as[static_cast<size_t>(e.from)];
        if (e.to >= 0 && as[static_cast<size_t>(e.to)] != -1) e.to = as[static_cast<size_t>(e.to)];
    }
    for (size_t k = 0; k < g.link_styles.size(); ++k)
        for (const int n : g.link_styles[k].first)
            if (n >= 0 && static_cast<size_t>(n) < g.edges.size()) g.edges[static_cast<size_t>(n)].style.over(g.link_styles[k].second);
    return true;
}

// ---------------------------------------------------------------- state diagrams

bool parse_state(const std::vector<std::string>& st, Graph& g, std::string* err) {
    g.state = true;
    // Composite states first: a transition may name one before its block.
    std::set<std::string> composites;
    for (size_t k = 1; k < st.size(); ++k) {
        std::string_view s = st[k];
        if (!diagram_lower(s).starts_with("state ") || !s.ends_with('{')) continue;
        std::string_view rest = diagram_trim(s.substr(6, s.size() - 7));
        if (rest.starts_with('"')) {
            const size_t q = rest.find('"', 1);
            rest = q == std::string_view::npos ? std::string_view() : diagram_trim(rest.substr(q + 1));
            if (diagram_lower(rest).starts_with("as ")) rest = diagram_trim(rest.substr(3));
        }
        if (!rest.empty()) composites.emplace(rest);
    }
    std::vector<int> open;
    std::map<int, int> starts, ends;  // each scope's [*] (the whole is -1)
    auto scope = [&] { return open.empty() ? -1 : open.back(); };
    // (A composite is within the scope it is first named in, which may
    // itself be within one first named elsewhere: how deep it is goes by
    // those, not by the blocks open.)
    bool deep = false;
    auto composite = [&](const std::string& name) {
        if (const auto it = g.cluster_ids.find(name); it != g.cluster_ids.end()) return it->second;
        int depth = 1;
        for (int p = scope(); p >= 0; p = g.clusters[static_cast<size_t>(p)].parent) ++depth;
        if (depth > kDeepest) deep = true;
        Cluster c;
        c.id = name;
        c.title = {name};
        c.parent = scope();
        c.state = true;
        g.clusters.push_back(std::move(c));
        return g.cluster_ids[name] = static_cast<int>(g.clusters.size()) - 1;
    };
    // A state named in the text (which may be a composite, or [*]).
    auto state = [&](std::string_view name, bool as_source) {
        name = diagram_trim(name);
        std::vector<std::string> classes;
        if (const size_t cls = name.find(":::"); cls != std::string_view::npos) {
            classes.emplace_back(diagram_trim(name.substr(cls + 3)));
            name = diagram_trim(name.substr(0, cls));
        }
        if (name == "[*]") {
            std::map<int, int>& which = as_source ? starts : ends;
            const auto it = which.find(scope());
            if (it != which.end()) return it->second;
            const int n = g.node((as_source ? "[*]start" : "[*]end") + std::to_string(scope()), scope());
            Node& nd = g.nodes[static_cast<size_t>(n)];
            nd.shape = as_source ? kShapeStart : kShapeEnd;
            nd.lines.clear();
            nd.given = true;
            return which[scope()] = n;
        }
        const std::string id(name);
        if (composites.count(id)) {
            const int c = composite(id);
            for (std::string& cl : classes) g.clusters[static_cast<size_t>(c)].classes.push_back(std::move(cl));
            return as_cluster(c);
        }
        const int n = g.node(id, scope());
        Node& nd = g.nodes[static_cast<size_t>(n)];
        if (!nd.given) nd.shape = kShapeState;
        for (std::string& cl : classes) nd.classes.push_back(std::move(cl));
        return n;
    };
    // A note: beside a state, in the scope that state is in.
    auto note = [&](std::string_view head, std::vector<std::string> text) {
        const std::string l = diagram_lower(head);
        const bool left = l.find(" left of ") != std::string::npos;
        const size_t of = l.find(" of ");
        if (of == std::string::npos) return;
        const int target = state(head.substr(of + 4), false);
        Node n;
        n.id = "note" + std::to_string(g.nodes.size());
        n.lines = std::move(text);
        n.shape = kShapeNote;
        n.given = true;
        n.note_of = target;
        n.note_left = left;
        n.cluster = is_cluster(target) ? g.clusters[static_cast<size_t>(cluster_of(target))].parent : g.nodes[static_cast<size_t>(target)].cluster;
        g.nodes.push_back(std::move(n));
    };
    bool in_note = false;
    std::string note_head;
    std::vector<std::string> note_text;
    auto too_deep = [&](size_t k) {
        *err = "line " + std::to_string(k) + ": states nested too deeply";
        return false;
    };
    for (size_t k = 1; k < st.size(); ++k) {
        if (deep) return too_deep(k);  // (the statement before this one)
        std::string_view s = st[k];
        const std::string l = diagram_lower(s);
        if (in_note) {
            if (l == "end note") {
                in_note = false;
                note(note_head, std::move(note_text));
                note_text.clear();
            } else {
                note_text.emplace_back(s);
            }
            continue;
        }
        if (l.starts_with("note ")) {
            const size_t colon = s.find(':');
            if (colon == std::string_view::npos) {
                in_note = true;
                note_head = std::string(s);
            } else {
                note(s.substr(0, colon), diagram_label(s.substr(colon + 1)));
            }
            continue;
        }
        if (l == "}") {
            if (!open.empty()) open.pop_back();
            continue;
        }
        if (l == "--" || l.starts_with("acctitle") || l.starts_with("accdescr") || l.starts_with("hide empty") || l.starts_with("scale ")) continue;
        if (l.starts_with("direction ")) {
            if (scope() >= 0) g.clusters[static_cast<size_t>(scope())].dir = direction(l.substr(10));
            else g.dir = direction(l.substr(10));
            continue;
        }
        if (l.starts_with("classdef ")) {
            std::string_view rest = diagram_trim(s.substr(9));
            const size_t sp = rest.find(' ');
            if (sp != std::string_view::npos) g.classes[std::string(rest.substr(0, sp))].over(parse_css(rest.substr(sp + 1)));
            continue;
        }
        if (l.starts_with("class ")) {
            std::string_view rest = diagram_trim(s.substr(6));
            const size_t sp = rest.rfind(' ');
            if (sp == std::string_view::npos) continue;
            std::vector<std::string> ids;
            const std::string_view list = rest.substr(0, sp);
            for (size_t at = 0; at <= list.size();) {
                const size_t c = std::min(list.find(',', at), list.size());
                if (c > at) ids.emplace_back(diagram_trim(list.substr(at, c - at)));
                at = c + 1;
            }
            g.class_of.push_back({ids, std::string(diagram_trim(rest.substr(sp + 1)))});
            continue;
        }
        if (l.starts_with("style ")) {
            std::string_view rest = diagram_trim(s.substr(6));
            const size_t sp = rest.find(' ');
            if (sp != std::string_view::npos) g.styles.push_back({std::string(rest.substr(0, sp)), parse_css(rest.substr(sp + 1))});
            continue;
        }
        if (l.starts_with("state ")) {
            std::string_view rest = diagram_trim(s.substr(6));
            const bool block = rest.ends_with('{');
            if (block) rest = diagram_trim(rest.substr(0, rest.size() - 1));
            std::vector<std::string> desc;
            if (rest.starts_with('"')) {
                const size_t q = rest.find('"', 1);
                if (q != std::string_view::npos) {
                    desc = diagram_label(rest.substr(0, q + 1));
                    rest = diagram_trim(rest.substr(q + 1));
                    if (diagram_lower(rest).starts_with("as ")) rest = diagram_trim(rest.substr(3));
                }
            }
            std::string_view name = rest;
            u8 shape = kShapeState;
            if (const size_t lt = rest.find("<<"); lt != std::string_view::npos) {
                const std::string kind = diagram_lower(rest.substr(lt));
                name = diagram_trim(rest.substr(0, lt));
                shape = kind.find("choice") != std::string::npos ? kShapeDiamond : (kind.find("fork") != std::string::npos || kind.find("join") != std::string::npos) ? kShapeBar : kShapeState;
            }
            if (const size_t colon = name.find(':'); colon != std::string_view::npos && !name.substr(colon).starts_with(":::")) {
                desc = diagram_label(name.substr(colon + 1));
                name = diagram_trim(name.substr(0, colon));
            }
            const int r = state(name, false);
            if (is_cluster(r)) {
                Cluster& c = g.clusters[static_cast<size_t>(cluster_of(r))];
                if (!desc.empty()) c.title = desc;
                if (block) open.push_back(cluster_of(r));
                continue;
            }
            Node& nd = g.nodes[static_cast<size_t>(r)];
            nd.shape = shape;
            if (!desc.empty()) nd.lines = desc;
            if (shape == kShapeDiamond || shape == kShapeBar) nd.lines.clear();
            nd.given = true;
            continue;
        }
        const size_t arrow = s.find("-->");
        if (arrow != std::string_view::npos) {
            std::string_view rest = s.substr(arrow + 3);
            Edge e;
            if (const size_t colon = rest.find(':'); colon != std::string_view::npos && !rest.substr(colon).starts_with(":::")) {
                e.label = diagram_label(rest.substr(colon + 1));
                rest = rest.substr(0, colon);
            }
            e.from = state(s.substr(0, arrow), true);
            e.to = state(rest, false);
            g.edges.push_back(std::move(e));
            if (g.nodes.size() > kMostNodes || g.edges.size() > kMostEdges) {
                *err = "too large to draw";
                return false;
            }
            continue;
        }
        if (const size_t colon = s.find(':'); colon != std::string_view::npos && !s.substr(colon).starts_with(":::")) {  // a state's description
            const int r = state(s.substr(0, colon), false);
            const std::vector<std::string> more = diagram_label(s.substr(colon + 1));
            if (is_cluster(r)) {
                g.clusters[static_cast<size_t>(cluster_of(r))].title = more;
                continue;
            }
            Node& nd = g.nodes[static_cast<size_t>(r)];
            if (!nd.given) nd.lines.clear();
            nd.lines.insert(nd.lines.end(), more.begin(), more.end());
            nd.given = true;
            continue;
        }
        const int r = state(s.ends_with('{') ? diagram_trim(s.substr(0, s.size() - 1)) : s, false);
        if (s.ends_with('{') && is_cluster(r)) open.push_back(cluster_of(r));
    }
    if (deep) return too_deep(st.size());
    return true;
}

// ---------------------------------------------------------------- laying out

struct Adj {
    int other = 0;
    float w = 1;
    float off_self = 0, off_other = 0;  // where on each it joins, across (a port of a subgraph's)
};

struct LNode {
    enum Kind : u8 { kItem, kDummy, kLabel, kPortIn, kPortOut };
    u8 kind = kItem;
    int ref = -1;    // an item's node or subgraph
    int edge = -1;   // a dummy's, a label's or a port's edge
    float across = 0, along = 0;
    int rank = 0, order = 0;
    double pos = 0;  // across, its middle
    float hint = 0;  // a port's place in the level around (0 to 1)
    std::vector<Adj> up, down;
};

struct Port {
    int edge = 0;
    bool out = false;
    float hint = 0;
};

struct LevelSize {
    float w = 0, h = 0;
    std::map<int, float> port_off;  // across, from the box's middle, by edge
};

struct Layout {
    Graph& g;
    const DiagramStyle& style;
    const DiagramFont& font;
    float u, size, line;
    float nodesep, ranksep, edgesep, pad, title_h, px, py, wrap;
    std::vector<std::vector<Port>> ports;  // each subgraph's
    std::vector<int> depth;                // each subgraph's

    Layout(Graph& graph, const DiagramStyle& s, const DiagramFont& f) : g(graph), style(s), font(f) {
        u = s.unit();
        size = s.size;
        line = f.line(size);
        nodesep = 40 * u, ranksep = 46 * u, edgesep = 16 * u;
        pad = 14 * u;
        title_h = line + 6 * u;
        px = 15 * u, py = 9 * u;
        wrap = std::max(f.width("abcdefghijklmnopqrstuvwxyz", size) * 1.0f, 160 * u);
        ports.assign(g.clusters.size(), {});
        depth.assign(g.clusters.size(), 0);
        for (size_t c = 0; c < g.clusters.size(); ++c) {
            int d = 0;
            for (int p = g.clusters[c].parent; p >= 0 && d < 64; p = g.clusters[static_cast<size_t>(p)].parent) ++d;
            depth[c] = d;
        }
    }

    static bool vertical(char dir) { return dir == 'D' || dir == 'U'; }

    // What, at level c, a reference is in: an item of that level (a node, or
    // a subgraph holding it); -1 if it is not in c at all, or is c.
    int item_of(int ref, int c) const {
        int r = ref;
        for (int guard = 0; guard < 128; ++guard) {
            const int parent = is_cluster(r) ? g.clusters[static_cast<size_t>(cluster_of(r))].parent : g.nodes[static_cast<size_t>(r)].cluster;
            if (parent == c) return r;
            if (parent == -1) return kNowhere;
            r = as_cluster(parent);
        }
        return kNowhere;
    }
    // Where the text first names a node, or anything in a subgraph.
    size_t first_named(int ref) const {
        if (!is_cluster(ref)) return static_cast<size_t>(ref);
        size_t best = g.nodes.size() + static_cast<size_t>(cluster_of(ref));
        for (size_t n = 0; n < g.nodes.size() && n < best; ++n)
            if (!g.nodes[n].dead && inside(static_cast<int>(n), cluster_of(ref))) best = n;
        return best;
    }
    bool inside(int ref, int c) const { return item_of(ref, c) != kNowhere && !(is_cluster(ref) && cluster_of(ref) == c); }

    // The size of a node's shape round its text.
    void measure(Node& n, char dir) {
        const float tiny = 14 * u;
        switch (n.shape) {
            case kShapeStart: n.w = n.h = tiny; return;
            case kShapeEnd: n.w = n.h = tiny * 1.2f; return;
            case kShapeBar:
                n.w = vertical(dir) ? 72 * u : 8 * u;
                n.h = vertical(dir) ? 8 * u : 72 * u;
                return;
            case kShapeDiamond:
                if (g.state && n.lines.empty()) {
                    n.w = n.h = 26 * u;
                    return;
                }
                break;
            default: break;
        }
        n.lines = diagram_wrap(n.lines, n.shape == kShapeDiamond ? wrap * 0.6f : wrap, size, font);
        float tw = 0;
        for (const std::string& l : n.lines) tw = std::max(tw, font.width(l, size, n.style.bold));
        const float th = static_cast<float>(std::max<size_t>(1, n.lines.size())) * line;
        const bool note = n.shape == kShapeNote;
        const DiagramVec s = diagram_shape_size(n.shape, tw, th, note ? 10 * u : px, note ? 7 * u : py);
        n.w = std::max(s.x, g.state ? 40 * u : 30 * u), n.h = s.y;
    }

    // ------------------------------------------------ one level

    LevelSize level(int c, char dir, int lvl_depth) {
        const bool vert = vertical(dir);
        std::vector<LNode> ln;
        std::map<int, int> item;  // reference -> its LNode
        for (size_t n = 0; n < g.nodes.size(); ++n)
            if (!g.nodes[n].dead && g.nodes[n].cluster == c) {
                measure(g.nodes[n], dir);
                LNode x;
                x.ref = static_cast<int>(n);
                item[x.ref] = static_cast<int>(ln.size());
                ln.push_back(x);
            }
        for (size_t k = 0; k < g.clusters.size(); ++k)
            if (g.clusters[k].parent == c) {
                LNode x;
                x.ref = as_cluster(static_cast<int>(k));
                item[x.ref] = static_cast<int>(ln.size());
                ln.push_back(x);
            }
        const size_t items = ln.size();
        // This level's edges (between two of its items), and its ports' (each
        // from or to one).
        struct LEdge {
            int u, v;  // LNodes, in the order of the ranks once cycles are broken
            int edge;
            int len;
            bool port = false;
        };
        std::vector<LEdge> le;
        for (size_t e = 0; e < g.edges.size(); ++e) {
            Edge& ed = g.edges[e];
            if (ed.from == ed.to) continue;
            const int a = item_of(ed.from, c), b = item_of(ed.to, c);
            const auto ia = item.find(a), ib = item.find(b);
            if (ia == item.end() || ib == item.end() || a == b) continue;
            le.push_back({ia->second, ib->second, static_cast<int>(e), ed.minlen * 2});
        }
        std::vector<int> port_ln;
        if (c >= 0)
            for (const Port& p : ports[static_cast<size_t>(c)]) {
                const Edge& ed = g.edges[static_cast<size_t>(p.edge)];
                // (the end inside: the far end of an edge going out, the near one of one coming in)
                const int end_ref = p.out ? ed.a : ed.b;
                const auto it = item.find(item_of(end_ref, c));
                if (it == item.end()) continue;
                LNode x;
                x.kind = p.out ? LNode::kPortOut : LNode::kPortIn;
                x.edge = p.edge;
                x.hint = p.hint;
                port_ln.push_back(static_cast<int>(ln.size()));
                ln.push_back(x);
                if (p.out) le.push_back({it->second, static_cast<int>(ln.size()) - 1, p.edge, 2, true});
                else le.push_back({static_cast<int>(ln.size()) - 1, it->second, p.edge, 2, true});
            }

        // Cycles broken: a depth-first walk, from the ports in, the sources,
        // then the rest, each in the order the text first names them (as
        // Mermaid's does); an edge back to a node on the way is turned round.
        {
            std::vector<std::vector<int>> out(ln.size());
            std::vector<int> indeg(ln.size(), 0);
            for (size_t k = 0; k < le.size(); ++k) out[static_cast<size_t>(le[k].u)].push_back(static_cast<int>(k)), ++indeg[static_cast<size_t>(le[k].v)];
            std::vector<u8> state(ln.size(), 0);
            std::vector<std::pair<int, size_t>> stack;
            auto walk = [&](int from) {
                if (state[static_cast<size_t>(from)]) return;
                state[static_cast<size_t>(from)] = 1;
                stack.push_back({from, 0});
                while (!stack.empty()) {
                    auto& [v, next] = stack.back();
                    if (next < out[static_cast<size_t>(v)].size()) {
                        LEdge& e = le[static_cast<size_t>(out[static_cast<size_t>(v)][next++])];
                        if (e.u != v) continue;
                        if (state[static_cast<size_t>(e.v)] == 1) {
                            std::swap(e.u, e.v);
                            if (!e.port) g.edges[static_cast<size_t>(e.edge)].reversed = !g.edges[static_cast<size_t>(e.edge)].reversed;
                        } else if (state[static_cast<size_t>(e.v)] == 0) {
                            state[static_cast<size_t>(e.v)] = 1;
                            stack.push_back({e.v, 0});
                        }
                    } else {
                        state[static_cast<size_t>(v)] = 2;
                        stack.pop_back();
                    }
                }
            };
            std::vector<int> by_text(items);
            std::vector<size_t> named(items);
            for (size_t v = 0; v < items; ++v) by_text[v] = static_cast<int>(v), named[v] = first_named(ln[v].ref);
            std::stable_sort(by_text.begin(), by_text.end(), [&](int a, int b) { return named[static_cast<size_t>(a)] < named[static_cast<size_t>(b)]; });
            for (const int p : port_ln)
                if (ln[static_cast<size_t>(p)].kind == LNode::kPortIn) walk(p);
            for (const int v : by_text)
                if (indeg[static_cast<size_t>(v)] == 0) walk(v);
            for (const int v : by_text) walk(v);
            for (size_t v = 0; v < ln.size(); ++v) walk(static_cast<int>(v));
        }
        for (const LEdge& e : le)
            if (!e.port) {
                Edge& ed = g.edges[static_cast<size_t>(e.edge)];
                ed.a = ed.reversed ? ed.to : ed.from;
                ed.b = ed.reversed ? ed.from : ed.to;
                ed.dir = dir;
                ed.laid = true;
            }

        // Ranks: as early as each can be, then each moved to where its edges
        // are shortest (the median of where they would have it).
        {
            const size_t n = ln.size();
            std::vector<std::vector<int>> out(n), in(n);
            for (size_t k = 0; k < le.size(); ++k) out[static_cast<size_t>(le[k].u)].push_back(static_cast<int>(k)), in[static_cast<size_t>(le[k].v)].push_back(static_cast<int>(k));
            std::vector<int> indeg(n), queue;
            for (size_t v = 0; v < n; ++v) {
                indeg[v] = static_cast<int>(in[v].size());
                if (!indeg[v]) queue.push_back(static_cast<int>(v));
            }
            std::vector<int> topo;
            for (size_t q = 0; q < queue.size(); ++q) {
                const int v = queue[q];
                topo.push_back(v);
                for (const int k : out[static_cast<size_t>(v)]) {
                    LEdge& e = le[static_cast<size_t>(k)];
                    ln[static_cast<size_t>(e.v)].rank = std::max(ln[static_cast<size_t>(e.v)].rank, ln[static_cast<size_t>(v)].rank + e.len);
                    if (--indeg[static_cast<size_t>(e.v)] == 0) queue.push_back(e.v);
                }
            }
            const int passes = n > 400 ? 2 : 8;
            for (int pass = 0; pass < passes; ++pass) {
                bool moved = false;
                for (int t = static_cast<int>(topo.size()) - 1; t >= 0; --t) {
                    const int v = topo[static_cast<size_t>(t)];
                    if (ln[static_cast<size_t>(v)].kind != LNode::kItem) continue;
                    std::vector<int> want;
                    int lo = -1 << 28, hi = 1 << 28;
                    for (const int k : in[static_cast<size_t>(v)]) {
                        const LEdge& e = le[static_cast<size_t>(k)];
                        want.push_back(ln[static_cast<size_t>(e.u)].rank + e.len);
                        lo = std::max(lo, want.back());
                    }
                    for (const int k : out[static_cast<size_t>(v)]) {
                        const LEdge& e = le[static_cast<size_t>(k)];
                        want.push_back(ln[static_cast<size_t>(e.v)].rank - e.len);
                        hi = std::min(hi, want.back());
                    }
                    if (want.empty() || lo > hi) continue;
                    std::sort(want.begin(), want.end());
                    int r = want[(want.size() - 1) / 2];
                    r = std::clamp(r, lo, hi);
                    if ((r - ln[static_cast<size_t>(v)].rank) % 2) continue;  // (kept on the ranks of nodes, not between them)
                    if (r != ln[static_cast<size_t>(v)].rank) ln[static_cast<size_t>(v)].rank = r, moved = true;
                }
                if (!moved) break;
            }
            // Notes: in the rank of what they are beside.
            for (size_t v = 0; v < items; ++v)
                if (const int ref = ln[v].ref; ref >= 0 && g.nodes[static_cast<size_t>(ref)].note_of != -1)
                    if (const auto it = item.find(g.nodes[static_cast<size_t>(ref)].note_of); it != item.end()) ln[v].rank = ln[static_cast<size_t>(it->second)].rank;
            // Ports at the edge of the box: before the first rank, after the last.
            int lo = 1 << 28, hi = -(1 << 28);
            for (size_t v = 0; v < items; ++v) lo = std::min(lo, ln[v].rank), hi = std::max(hi, ln[v].rank);
            if (items == 0) lo = hi = 0;
            for (const int p : port_ln) ln[static_cast<size_t>(p)].rank = ln[static_cast<size_t>(p)].kind == LNode::kPortIn ? lo - 1 : hi + 1;
            int least = 1 << 28;
            for (const LNode& x : ln) least = std::min(least, x.rank);
            for (LNode& x : ln) x.rank -= least;
        }

        // The subgraphs' ports: where an edge between items here goes into one.
        for (const LEdge& e : le) {
            const Edge& ed = g.edges[static_cast<size_t>(e.edge)];
            const LNode& lu = ln[static_cast<size_t>(e.u)];
            const LNode& lv = ln[static_cast<size_t>(e.v)];
            if (lu.kind == LNode::kItem && is_cluster(lu.ref) && inside(ed.a, cluster_of(lu.ref)))
                ports[static_cast<size_t>(cluster_of(lu.ref))].push_back({e.edge, true, 0});
            if (lv.kind == LNode::kItem && is_cluster(lv.ref) && inside(ed.b, cluster_of(lv.ref)))
                ports[static_cast<size_t>(cluster_of(lv.ref))].push_back({e.edge, false, 0});
        }

        // Chains: an edge across more than one rank goes through a point at
        // each between (its label one of them, in the middle).
        std::vector<std::vector<int>> chain(le.size());
        for (size_t k = 0; k < le.size(); ++k) {
            const LEdge& e = le[k];
            Edge& ed = g.edges[static_cast<size_t>(e.edge)];
            const int r0 = ln[static_cast<size_t>(e.u)].rank, r1 = ln[static_cast<size_t>(e.v)].rank;
            const int mid = r0 + (r1 - r0) / 2;
            chain[k].push_back(e.u);
            for (int r = r0 + 1; r < r1; ++r) {
                LNode d;
                d.kind = LNode::kDummy;
                d.edge = e.edge;
                d.rank = r;
                if (!e.port && r == mid && !ed.label.empty() && ed.line != kInvisible) {
                    d.kind = LNode::kLabel;
                    ed.label = diagram_wrap(ed.label, wrap, size, font);
                    float tw = 0;
                    for (const std::string& l : ed.label) tw = std::max(tw, font.width(l, size));
                    ed.label_w = tw + 12 * u;
                    ed.label_h = static_cast<float>(ed.label.size()) * line + 6 * u;
                    d.across = vert ? ed.label_w : ed.label_h;
                    d.along = vert ? ed.label_h : ed.label_w;
                }
                chain[k].push_back(static_cast<int>(ln.size()));
                ln.push_back(d);
            }
            chain[k].push_back(e.v);
            for (size_t i = 0; i + 1 < chain[k].size(); ++i) {
                const int a = chain[k][i], b = chain[k][i + 1];
                const bool da = ln[static_cast<size_t>(a)].kind != LNode::kItem, db = ln[static_cast<size_t>(b)].kind != LNode::kItem;
                const float w = da && db ? 8.0f : da || db ? 2.0f : 1.0f;
                ln[static_cast<size_t>(a)].down.push_back({b, w});
                ln[static_cast<size_t>(b)].up.push_back({a, w});
            }
        }

        // Order within the ranks.
        int ranks = 0;
        for (const LNode& x : ln) ranks = std::max(ranks, x.rank + 1);
        std::vector<std::vector<int>> layer(static_cast<size_t>(ranks));
        {
            // First: the order a walk down from the top meets them in.
            std::vector<u8> seen(ln.size(), 0);
            std::vector<int> by_rank(ln.size());
            for (size_t v = 0; v < ln.size(); ++v) by_rank[v] = static_cast<int>(v);
            std::stable_sort(by_rank.begin(), by_rank.end(), [&](int a, int b) { return ln[static_cast<size_t>(a)].rank < ln[static_cast<size_t>(b)].rank; });
            std::vector<int> stack;
            for (const int start : by_rank) {
                if (seen[static_cast<size_t>(start)]) continue;
                stack.push_back(start);
                while (!stack.empty()) {
                    const int v = stack.back();
                    stack.pop_back();
                    if (seen[static_cast<size_t>(v)]) continue;
                    seen[static_cast<size_t>(v)] = 1;
                    layer[static_cast<size_t>(ln[static_cast<size_t>(v)].rank)].push_back(v);
                    const auto& d = ln[static_cast<size_t>(v)].down;
                    for (auto it = d.rbegin(); it != d.rend(); ++it)
                        if (!seen[static_cast<size_t>(it->other)]) stack.push_back(it->other);
                }
            }
            for (std::vector<int>& l : layer) {
                if (l.empty() || (ln[static_cast<size_t>(l[0])].kind != LNode::kPortIn && ln[static_cast<size_t>(l[0])].kind != LNode::kPortOut)) continue;
                std::stable_sort(l.begin(), l.end(), [&](int a, int b) { return ln[static_cast<size_t>(a)].hint < ln[static_cast<size_t>(b)].hint; });
            }
        }
        auto renumber = [&] {
            for (std::vector<int>& l : layer)
                for (size_t i = 0; i < l.size(); ++i) ln[static_cast<size_t>(l[i])].order = static_cast<int>(i);
        };
        auto notes_beside = [&](std::vector<int>& l) {
            for (size_t i = 0; i < l.size(); ++i) {
                const LNode& x = ln[static_cast<size_t>(l[i])];
                if (x.kind != LNode::kItem || x.ref < 0) continue;
                const Node& nd = g.nodes[static_cast<size_t>(x.ref)];
                if (nd.note_of == -1) continue;
                const auto t = item.find(nd.note_of);
                if (t == item.end()) continue;
                const int note = l[i];
                l.erase(l.begin() + static_cast<std::ptrdiff_t>(i));
                const auto at = std::find(l.begin(), l.end(), t->second);
                if (at == l.end()) {
                    l.insert(l.begin() + static_cast<std::ptrdiff_t>(i), note);
                    continue;
                }
                l.insert(nd.note_left ? at : at + 1, note);
            }
        };
        for (std::vector<int>& l : layer) notes_beside(l);
        renumber();
        auto crossings = [&] {
            long long total = 0;
            std::vector<std::pair<int, int>> pairs;
            std::vector<int> tree;
            for (size_t r = 0; r + 1 < layer.size(); ++r) {
                pairs.clear();
                for (const int v : layer[r])
                    for (const Adj& a : ln[static_cast<size_t>(v)].down) pairs.push_back({ln[static_cast<size_t>(v)].order, ln[static_cast<size_t>(a.other)].order});
                std::sort(pairs.begin(), pairs.end());
                const size_t m = layer[r + 1].size() + 1;
                tree.assign(m + 1, 0);
                for (size_t k = 0; k < pairs.size(); ++k) {
                    // (how many before it went further right)
                    int above = 0;
                    for (int i = static_cast<int>(m); i > 0; i -= i & -i) above += tree[static_cast<size_t>(i)];
                    for (int i = pairs[k].second + 1; i > 0; i -= i & -i) above -= tree[static_cast<size_t>(i)];
                    total += above;
                    for (int i = pairs[k].second + 1; i <= static_cast<int>(m); i += i & -i) ++tree[static_cast<size_t>(i)];
                }
            }
            return total;
        };
        auto fixed = [&](const std::vector<int>& l) {
            return !l.empty() && (ln[static_cast<size_t>(l[0])].kind == LNode::kPortIn || ln[static_cast<size_t>(l[0])].kind == LNode::kPortOut);
        };
        auto sweep = [&](bool down) {
            const int n = static_cast<int>(layer.size());
            for (int k = 1; k < n; ++k) {
                const int r = down ? k : n - 1 - k;
                std::vector<int>& l = layer[static_cast<size_t>(r)];
                if (fixed(l)) continue;
                std::vector<std::pair<double, int>> movable;
                std::vector<u8> moves(l.size(), 0);
                for (size_t i = 0; i < l.size(); ++i) {
                    const LNode& x = ln[static_cast<size_t>(l[i])];
                    const std::vector<Adj>& adj = down ? x.up : x.down;
                    double sum = 0, w = 0;
                    for (const Adj& a : adj) sum += a.w * ln[static_cast<size_t>(a.other)].order, w += a.w;
                    if (w > 0) movable.push_back({sum / w, l[i]}), moves[i] = 1;
                }
                std::stable_sort(movable.begin(), movable.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                size_t next = 0;
                for (size_t i = 0; i < l.size(); ++i)
                    if (moves[i]) l[i] = movable[next++].second;
                notes_beside(l);
                for (size_t i = 0; i < l.size(); ++i) ln[static_cast<size_t>(l[i])].order = static_cast<int>(i);
            }
        };
        {
            const size_t work = ln.size() + le.size();
            const int iterations = work > 3000 ? 2 : work > 800 ? 6 : 24;
            long long best = crossings();
            std::vector<std::vector<int>> best_layer = layer;
            for (int it = 0; it < iterations && best > 0; ++it) {
                sweep(it % 2 == 0);
                const long long c2 = crossings();
                if (c2 < best) best = c2, best_layer = layer;
            }
            layer = best_layer;
            renumber();
            // Neighbours swapped where that crosses less.
            if (work <= 800 && best > 0) {
                for (int pass = 0; pass < 8; ++pass) {
                    bool better = false;
                    for (size_t r = 0; r < layer.size(); ++r) {
                        std::vector<int>& l = layer[r];
                        if (fixed(l)) continue;
                        for (size_t i = 0; i + 1 < l.size(); ++i) {
                            auto cross_pair = [&](int a, int b) {
                                long long n = 0;
                                for (const bool d : {false, true}) {
                                    const auto& aa = d ? ln[static_cast<size_t>(a)].down : ln[static_cast<size_t>(a)].up;
                                    const auto& bb = d ? ln[static_cast<size_t>(b)].down : ln[static_cast<size_t>(b)].up;
                                    for (const Adj& x : aa)
                                        for (const Adj& y : bb) n += ln[static_cast<size_t>(x.other)].order > ln[static_cast<size_t>(y.other)].order;
                                }
                                return n;
                            };
                            const int a = l[i], b = l[i + 1];
                            const bool note = (ln[static_cast<size_t>(a)].ref >= 0 && g.nodes[static_cast<size_t>(ln[static_cast<size_t>(a)].ref)].note_of != -1) ||
                                              (ln[static_cast<size_t>(b)].ref >= 0 && g.nodes[static_cast<size_t>(ln[static_cast<size_t>(b)].ref)].note_of != -1);
                            if (!note && cross_pair(b, a) < cross_pair(a, b)) {
                                std::swap(l[i], l[i + 1]);
                                ln[static_cast<size_t>(l[i])].order = static_cast<int>(i);
                                ln[static_cast<size_t>(l[i + 1])].order = static_cast<int>(i + 1);
                                better = true;
                            }
                        }
                    }
                    if (!better) break;
                }
            }
        }

        // The subgraphs here, laid out (each knowing where its ports' edges
        // come from around it), and so their sizes.
        std::map<int, LevelSize> inner;
        for (size_t v = 0; v < items; ++v) {
            if (!is_cluster(ln[v].ref)) continue;
            const int k = cluster_of(ln[v].ref);
            for (Port& p : ports[static_cast<size_t>(k)])
                for (size_t q = 0; q < le.size(); ++q) {
                    if (le[q].edge != p.edge) continue;
                    const std::vector<int>& ch = chain[q];
                    const int next = p.out ? ch[1] : ch[ch.size() - 2];
                    const size_t width = layer[static_cast<size_t>(ln[static_cast<size_t>(next)].rank)].size();
                    p.hint = (ln[static_cast<size_t>(next)].order + 0.5f) / static_cast<float>(std::max<size_t>(1, width));
                }
            const Cluster& cl = g.clusters[static_cast<size_t>(k)];
            const char inner_dir = ports[static_cast<size_t>(k)].empty() && cl.dir ? cl.dir : dir;
            inner[k] = level(k, inner_dir, lvl_depth + 1);
        }
        // Sizes, across and along.
        for (size_t v = 0; v < items; ++v) {
            LNode& x = ln[v];
            float w, h;
            if (is_cluster(x.ref)) w = inner[cluster_of(x.ref)].w, h = inner[cluster_of(x.ref)].h;
            else w = g.nodes[static_cast<size_t>(x.ref)].w, h = g.nodes[static_cast<size_t>(x.ref)].h;
            x.across = vert ? w : h;
            x.along = vert ? h : w;
        }
        // An edge into a subgraph's port joins it where that port is.
        for (size_t q = 0; q < le.size(); ++q) {
            const std::vector<int>& ch = chain[q];
            for (const bool front : {true, false}) {
                const int end = front ? ch.front() : ch.back();
                const LNode& x = ln[static_cast<size_t>(end)];
                if (x.kind != LNode::kItem || !is_cluster(x.ref)) continue;
                const LevelSize& ls = inner[cluster_of(x.ref)];
                const auto it = ls.port_off.find(le[q].edge);
                if (it == ls.port_off.end()) continue;
                const int nb = front ? ch[1] : ch[ch.size() - 2];
                for (Adj& a : front ? ln[static_cast<size_t>(end)].down : ln[static_cast<size_t>(end)].up)
                    if (a.other == nb) a.off_self = it->second;
                for (Adj& a : front ? ln[static_cast<size_t>(nb)].up : ln[static_cast<size_t>(nb)].down)
                    if (a.other == end) a.off_other = it->second;
            }
        }
        // Self loops: room beside the node.
        for (Edge& ed : g.edges)
            if (ed.from == ed.to && ed.from >= 0 && g.nodes[static_cast<size_t>(ed.from)].cluster == c && !g.nodes[static_cast<size_t>(ed.from)].dead) {
                Node& n = g.nodes[static_cast<size_t>(ed.from)];
                n.loop = true;
                float lw = 0;
                ed.label = diagram_wrap(ed.label, wrap, size, font);
                for (const std::string& l : ed.label) lw = std::max(lw, font.width(l, size));
                const float bulge = 22 * u;
                n.loop_room = std::max(n.loop_room, bulge + (lw > 0 ? lw + 12 * u : 0));
                ed.dir = dir;
                ed.laid = true;
            }

        // Across: each node where the mean of its neighbours would have it,
        // nodes in a rank kept apart; over and over, down and up.
        auto gap = [&](const LNode& a, const LNode& b) {
            const bool ia = a.kind == LNode::kItem || a.kind == LNode::kLabel, ib = b.kind == LNode::kItem || b.kind == LNode::kLabel;
            float gp = ia && ib ? nodesep : ia || ib ? (nodesep + edgesep) / 2 : edgesep;
            const bool note = (a.kind == LNode::kItem && a.ref >= 0 && g.nodes[static_cast<size_t>(a.ref)].note_of != -1) ||
                              (b.kind == LNode::kItem && b.ref >= 0 && g.nodes[static_cast<size_t>(b.ref)].note_of != -1);
            if (note) gp = nodesep * 0.6f;
            if (a.kind == LNode::kItem && a.ref >= 0) gp += g.nodes[static_cast<size_t>(a.ref)].loop_room;
            return gp + (a.across + b.across) / 2;
        };
        for (std::vector<int>& l : layer) {
            double at = 0;
            for (size_t i = 0; i < l.size(); ++i) {
                if (i) at += gap(ln[static_cast<size_t>(l[i - 1])], ln[static_cast<size_t>(l[i])]);
                ln[static_cast<size_t>(l[i])].pos = at;
            }
        }
        {
            const size_t work = ln.size();
            const int passes = work > 2000 ? 6 : work > 500 ? 20 : 60;
            std::vector<double> want, weight, sep, x;
            for (int pass = 0; pass < passes; ++pass) {
                const bool down = pass % 2 == 0;
                for (size_t k = 0; k < layer.size(); ++k) {
                    std::vector<int>& l = layer[down ? k : layer.size() - 1 - k];
                    const size_t n = l.size();
                    if (!n) continue;
                    want.assign(n, 0), weight.assign(n, 0), sep.assign(n, 0), x.assign(n, 0);
                    for (size_t i = 0; i < n; ++i) {
                        const LNode& v = ln[static_cast<size_t>(l[i])];
                        double sum = 0, w = 0;
                        for (const std::vector<Adj>* adj : {&v.up, &v.down})
                            for (const Adj& a : *adj) {
                                sum += a.w * (ln[static_cast<size_t>(a.other)].pos + a.off_other - a.off_self);
                                w += a.w;
                            }
                        want[i] = w > 0 ? sum / w : v.pos;
                        weight[i] = w > 0 ? w : 0.05;
                        if (i) sep[i] = gap(ln[static_cast<size_t>(l[i - 1])], v);
                    }
                    // (pool adjacent violators: the nearest to what is wanted that keeps them apart)
                    struct Block {
                        double w, wt;
                        size_t from, n;
                    };
                    std::vector<Block> blocks;
                    double off = 0;
                    std::vector<double> offs(n);
                    for (size_t i = 0; i < n; ++i) {
                        off += sep[i];
                        offs[i] = off;
                        blocks.push_back({weight[i], weight[i] * (want[i] - off), i, 1});
                        while (blocks.size() >= 2) {
                            Block& b = blocks.back();
                            Block& a = blocks[blocks.size() - 2];
                            if (a.wt / a.w < b.wt / b.w) break;
                            a.w += b.w, a.wt += b.wt, a.n += b.n;
                            blocks.pop_back();
                        }
                    }
                    for (const Block& b : blocks)
                        for (size_t i = b.from; i < b.from + b.n; ++i) ln[static_cast<size_t>(l[i])].pos = b.wt / b.w + offs[i];
                }
            }
        }

        // Along: each rank as deep as its deepest, half the rank gap between
        // one and the next (a node's rank and the rank of the points between).
        std::vector<float> depth_of(layer.size(), 0), at(layer.size(), 0);
        const bool has_in = !layer.empty() && fixed(layer.front()) && ln[static_cast<size_t>(layer.front()[0])].kind == LNode::kPortIn;
        const bool has_out = !layer.empty() && fixed(layer.back()) && ln[static_cast<size_t>(layer.back()[0])].kind == LNode::kPortOut;
        for (size_t r = 0; r < layer.size(); ++r)
            for (const int v : layer[r]) depth_of[r] = std::max(depth_of[r], ln[static_cast<size_t>(v)].along);
        {
            float y = 0;
            bool first = true;
            for (size_t r = 0; r < layer.size(); ++r) {
                if ((r == 0 && has_in) || (r + 1 == layer.size() && has_out && layer.size() > 1)) continue;
                if (!first) y += depth_of[r - 1] / 2 + ranksep / 2;
                y += depth_of[r] / 2;
                at[r] = y;
                first = false;
            }
        }
        float lo_a = 0, hi_a = 0, lo_l = 0, hi_l = 0;
        bool any = false;
        for (size_t r = 0; r < layer.size(); ++r)
            for (const int v : layer[r]) {
                const LNode& x = ln[static_cast<size_t>(v)];
                if (x.kind == LNode::kPortIn || x.kind == LNode::kPortOut) continue;
                const float a0 = static_cast<float>(x.pos) - x.across / 2, a1 = static_cast<float>(x.pos) + x.across / 2;
                const float l0 = at[r] - x.along / 2, l1 = at[r] + x.along / 2;
                if (!any) lo_a = a0, hi_a = a1, lo_l = l0, hi_l = l1, any = true;
                lo_a = std::min(lo_a, a0), hi_a = std::max(hi_a, a1), lo_l = std::min(lo_l, l0), hi_l = std::max(hi_l, l1);
            }
        // Room for self loops' bulges.
        for (size_t v = 0; v < items; ++v)
            if (ln[v].ref >= 0 && g.nodes[static_cast<size_t>(ln[v].ref)].loop) hi_a = std::max(hi_a, static_cast<float>(ln[v].pos) + ln[v].across / 2 + g.nodes[static_cast<size_t>(ln[v].ref)].loop_room);
        const float span_a = hi_a - lo_a, span_l = hi_l - lo_l;
        // The box: the content, padded, with the title at the top.
        const bool titled = c >= 0 && !g.clusters[static_cast<size_t>(c)].title.empty() && !(g.clusters[static_cast<size_t>(c)].title.size() == 1 && g.clusters[static_cast<size_t>(c)].title[0].empty());
        float title_w = 0;
        if (titled)
            for (const std::string& t : g.clusters[static_cast<size_t>(c)].title) title_w = std::max(title_w, font.width(t, size, true));
        const float content_w = vert ? span_a : span_l, content_h = vert ? span_l : span_a;
        const float margin = c < 0 ? 8 * u : pad;
        const float top = margin + (titled ? title_h * static_cast<float>(g.clusters[static_cast<size_t>(c)].title.size()) : 0) + (titled && g.clusters[static_cast<size_t>(c)].state ? 4 * u : 0);
        LevelSize res;
        res.w = std::max(content_w + 2 * margin, titled ? title_w + 2 * pad : 0);
        res.h = content_h + top + margin;
        if (c >= 0) res.w = std::max(res.w, 60 * u), res.h = std::max(res.h, top + margin + 10 * u);
        const float ox = (res.w - content_w) / 2, oy = top;
        // Layout's (across, along) to this box's (x, y).
        auto place = [&](double a, double l) -> DiagramVec {
            const float ar = static_cast<float>(a) - lo_a, lr = static_cast<float>(l) - lo_l;
            switch (dir) {
                case 'U': return {ox + ar, oy + span_l - lr};
                case 'R': return {ox + lr, oy + ar};
                case 'L': return {ox + span_l - lr, oy + ar};
                default: return {ox + ar, oy + lr};
            }
        };
        // A port's place on this box's border, across at a.
        auto border = [&](double a, bool out) -> DiagramVec {
            const float ar = static_cast<float>(a) - lo_a;
            const bool first = out == (dir == 'U' || dir == 'L');  // (the box's top or left side)
            if (vert) return {ox + ar, first ? 0.0f : res.h};
            return {first ? 0.0f : res.w, oy + ar};
        };
        for (size_t v = 0; v < items; ++v) {
            const LNode& x = ln[v];
            const DiagramVec p = place(x.pos, at[static_cast<size_t>(x.rank)]);
            if (is_cluster(x.ref)) {
                Cluster& cl = g.clusters[static_cast<size_t>(cluster_of(x.ref))];
                const LevelSize& ls = inner[cluster_of(x.ref)];
                cl.w = ls.w, cl.h = ls.h;
                cl.x = p.x - ls.w / 2, cl.y = p.y - ls.h / 2;
            } else {
                Node& n = g.nodes[static_cast<size_t>(x.ref)];
                n.x = p.x, n.y = p.y;
            }
        }
        for (const int p : port_ln) {
            const LNode& x = ln[static_cast<size_t>(p)];
            res.port_off[x.edge] = static_cast<float>(x.pos) - lo_a - (vert ? (res.w / 2 - ox) : (res.h / 2 - oy));
        }
        // The edges' parts here.
        for (size_t q = 0; q < le.size(); ++q) {
            const LEdge& e = le[q];
            Edge& ed = g.edges[static_cast<size_t>(e.edge)];
            Seg seg;
            seg.level = c, seg.depth = lvl_depth;
            const std::vector<int>& ch = chain[q];
            for (size_t i = 0; i < ch.size(); ++i) {
                const LNode& x = ln[static_cast<size_t>(ch[i])];
                if (x.kind == LNode::kPortIn || x.kind == LNode::kPortOut) {
                    seg.pts.push_back(border(x.pos, x.kind == LNode::kPortOut));
                    continue;
                }
                if (x.kind == LNode::kItem && is_cluster(x.ref)) {
                    const LevelSize& ls = inner[cluster_of(x.ref)];
                    const auto it = ls.port_off.find(e.edge);
                    if (it != ls.port_off.end()) {  // (through its port, on its border)
                        const bool out = i == 0;
                        seg.pts.push_back(place(x.pos + it->second, at[static_cast<size_t>(x.rank)] + (out ? 0.5f : -0.5f) * x.along));
                        continue;
                    }
                }
                // (A point between passes its rank straight, as deep as the
                // rank is: it never cuts the corner of a node beside it.)
                const float deep = depth_of[static_cast<size_t>(x.rank)];
                if (x.kind == LNode::kDummy && deep > 0) {
                    seg.pts.push_back(place(x.pos, at[static_cast<size_t>(x.rank)] - deep / 2));
                    seg.pts.push_back(place(x.pos, at[static_cast<size_t>(x.rank)] + deep / 2));
                    continue;
                }
                seg.pts.push_back(place(x.pos, at[static_cast<size_t>(x.rank)]));
                if (x.kind == LNode::kLabel) {
                    ed.label_level = c;
                    ed.label_at = seg.pts.back();
                }
            }
            if (!e.port) ed.mid = std::move(seg);
            else if (ln[static_cast<size_t>(e.u)].kind == LNode::kPortIn) ed.in_b.push_back(std::move(seg));
            else ed.out_a.push_back(std::move(seg));
        }
        return res;
    }

    // ------------------------------------------------ the whole

    // Each level's box, from its own place to the whole's.
    void absolute(float* w, float* h) {
        const LevelSize root = level(-1, g.dir, 0);
        *w = root.w, *h = root.h;
        std::vector<DiagramVec> origin(g.clusters.size());
        std::vector<int> order(g.clusters.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth[static_cast<size_t>(a)] < depth[static_cast<size_t>(b)]; });
        for (const int k : order) {
            Cluster& cl = g.clusters[static_cast<size_t>(k)];
            const DiagramVec base = cl.parent >= 0 ? origin[static_cast<size_t>(cl.parent)] : DiagramVec{};
            cl.x += base.x, cl.y += base.y;
            origin[static_cast<size_t>(k)] = {cl.x, cl.y};
        }
        auto at = [&](int lvl) { return lvl >= 0 ? origin[static_cast<size_t>(lvl)] : DiagramVec{}; };
        for (Node& n : g.nodes) {
            if (n.dead) continue;
            const DiagramVec o = at(n.cluster);
            n.x += o.x, n.y += o.y;
        }
        for (Edge& e : g.edges) {
            for (Seg* s : {&e.mid}) {
                const DiagramVec o = at(s->level);
                for (DiagramVec& p : s->pts) p.x += o.x, p.y += o.y;
            }
            for (std::vector<Seg>* list : {&e.out_a, &e.in_b})
                for (Seg& s : *list) {
                    const DiagramVec o = at(s.level);
                    for (DiagramVec& p : s.pts) p.x += o.x, p.y += o.y;
                }
            if (e.label_level >= -1 && !e.label.empty()) {
                const DiagramVec o = at(e.label_level);
                e.label_at.x += o.x, e.label_at.y += o.y;
            }
        }
    }
};

// ---------------------------------------------------------------- drawing

// Each node's and subgraph's style: classDef default, its classes, its own style lines.
void resolve_styles(Graph& g) {
    for (const auto& [ids, cls] : g.class_of)
        for (const std::string& id : ids) {
            if (const auto it = g.ids.find(id); it != g.ids.end()) g.nodes[static_cast<size_t>(it->second)].classes.push_back(cls);
            if (const auto it = g.cluster_ids.find(id); it != g.cluster_ids.end()) g.clusters[static_cast<size_t>(it->second)].classes.push_back(cls);
        }
    const auto def = g.classes.find("default");
    for (Node& n : g.nodes) {
        Style s;
        if (def != g.classes.end()) s.over(def->second);
        for (const std::string& c : n.classes)
            if (const auto it = g.classes.find(c); it != g.classes.end()) s.over(it->second);
        s.over(n.style);
        n.style = s;
    }
    for (Cluster& cl : g.clusters)
        for (const std::string& c : cl.classes)
            if (const auto it = g.classes.find(c); it != g.classes.end()) cl.style.over(it->second);
    for (const auto& [id, s] : g.styles) {
        if (const auto it = g.ids.find(id); it != g.ids.end()) g.nodes[static_cast<size_t>(it->second)].style.over(s);
        if (const auto it = g.cluster_ids.find(id); it != g.cluster_ids.end()) g.clusters[static_cast<size_t>(it->second)].style.over(s);
    }
}

struct Ink {
    DiagramColour node_fill, node_stroke, text, edge, label_fill, cluster_fill, cluster_stroke, note_fill, note_stroke, dot;
};

DiagramColour readable_on(DiagramColour fill, const DiagramPalette& p) {
    if (fill.none() || fill.a < 0.5f) return p.of(kMdText);
    const float lum = fill.r * 0.3f + fill.g * 0.59f + fill.b * 0.11f;
    return lum > 0.55f ? DiagramColour{0.1f, 0.1f, 0.12f, 1} : DiagramColour{0.96f, 0.96f, 0.97f, 1};
}

DiagramVec axis_of(char dir) { return dir == 'U' ? DiagramVec{0, -1} : dir == 'R' ? DiagramVec{1, 0} : dir == 'L' ? DiagramVec{-1, 0} : DiagramVec{0, 1}; }

void draw(Graph& g, const Layout& lay, const DiagramStyle& style, float w, float h, Drawing* d, DiagramGeometry* geo) {
    const DiagramPalette& p = style.palette;
    const DiagramFont& font = *style.font;
    const float u = lay.u, size = lay.size, line = lay.line;
    const bool dark = p.dark();
    Ink ink;
    ink.node_fill = p.tone(kMdBorder, dark ? 0.17f : 0.11f);
    ink.node_stroke = p.tone(kMdBorder, dark ? 0.8f : 0.75f);
    ink.text = p.of(kMdText);
    ink.edge = p.tone(kMdText, dark ? 0.55f : 0.6f);
    ink.label_fill = p.tone(kMdText, dark ? 0.12f : 0.08f);
    ink.cluster_fill = p.tone(kMdAccent, dark ? 0.06f : 0.045f);
    ink.cluster_stroke = p.tone(kMdAccent, dark ? 0.4f : 0.45f);
    ink.note_fill = p.tone(kMdSlice1, dark ? 0.2f : 0.16f);
    ink.note_stroke = p.tone(kMdSlice1, 0.7f);
    ink.dot = p.of(kMdText);
    const float stroke_w = 1.3f * u, edge_w = 1.5f * u;
    d->w = w, d->h = h;

    auto text_block = [&](const std::vector<std::string>& lines, float cx, float cy, DiagramColour c, bool bold) {
        const float th = static_cast<float>(lines.size()) * line;
        float y = cy - th / 2 + (line - font.ascent(size) - font.descent(size)) / 2 + font.ascent(size);
        for (const std::string& l : lines) {
            d->text(l, cx, y, size, c, font, 1, bold);
            y += line;
        }
    };

    // Subgraphs, the outer first.
    std::vector<int> order(g.clusters.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return lay.depth[static_cast<size_t>(a)] < lay.depth[static_cast<size_t>(b)]; });
    for (const int k : order) {
        const Cluster& cl = g.clusters[static_cast<size_t>(k)];
        RasterPath box;
        box.rect(cl.x, cl.y, cl.w, cl.h, (cl.state ? 9 : 5) * u);
        const DiagramColour fill = !cl.style.fill.none() ? cl.style.fill : cl.state ? p.tone(kMdBorder, dark ? 0.07f : 0.05f) : ink.cluster_fill;
        const DiagramColour stroke = !cl.style.stroke.none() ? cl.style.stroke : cl.state ? ink.node_stroke : ink.cluster_stroke;
        d->shape(std::move(box), fill, stroke, cl.style.width >= 0 ? cl.style.width * u : stroke_w, cl.style.dash);
        if (cl.state && !cl.title.empty()) {
            const float band = lay.title_h * static_cast<float>(cl.title.size()) + lay.pad * 0.5f;
            RasterPath rule;
            rule.move_to(cl.x, cl.y + band), rule.line_to(cl.x + cl.w, cl.y + band);
            d->shape(std::move(rule), {}, stroke, stroke_w);
        }
        if (geo) geo->boxes.push_back({"cluster", cl.id, cl.x, cl.y, cl.w, cl.h});
    }

    // Edges.
    struct LabelAt {
        const Edge* e;
        DiagramVec at;
    };
    std::vector<LabelAt> labels;
    auto end_box = [&](int ref, float* x, float* y, float* bw, float* bh, u8* shape) {
        if (is_cluster(ref)) {
            const Cluster& cl = g.clusters[static_cast<size_t>(cluster_of(ref))];
            *x = cl.x, *y = cl.y, *bw = cl.w, *bh = cl.h, *shape = kShapeRect;
        } else {
            const Node& n = g.nodes[static_cast<size_t>(ref)];
            *x = n.x - n.w / 2, *y = n.y - n.h / 2, *bw = n.w, *bh = n.h, *shape = n.shape;
        }
    };
    // Each edge's points, from a to b: out of a's subgraphs, across, into b's.
    std::vector<std::vector<DiagramVec>> chains(g.edges.size());
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const Edge& e = g.edges[k];
        if (!e.laid || e.from == e.to) continue;
        std::vector<DiagramVec>& pts = chains[k];
        auto add = [&](const std::vector<DiagramVec>& s) {
            for (const DiagramVec& q : s)
                if (pts.empty() || std::fabs(pts.back().x - q.x) + std::fabs(pts.back().y - q.y) > 0.01f) pts.push_back(q);
        };
        std::vector<Seg> outs = e.out_a, ins = e.in_b;
        std::stable_sort(outs.begin(), outs.end(), [](const Seg& a, const Seg& b) { return a.depth > b.depth; });
        std::stable_sort(ins.begin(), ins.end(), [](const Seg& a, const Seg& b) { return a.depth < b.depth; });
        std::vector<DiagramVec> keep;  // (ports and the label: the line goes through them)
        for (const Seg& s : outs) add(s.pts), keep.push_back(pts.back());
        add(e.mid.pts);
        for (const Seg& s : ins) keep.push_back(s.pts.empty() ? DiagramVec{} : s.pts.front()), add(s.pts);
        if (!e.label.empty() && e.label_w > 0) keep.push_back(e.label_at);
        if (pts.size() < 3) continue;
        // Straightened: the points between left out wherever a straight line
        // from the last one kept clears every other node, subgraph and label
        // (with room to spare), so that a long edge is one sweep, not steps.
        std::vector<std::pair<DiagramVec, DiagramVec>> walls;  // (boxes, top left and bottom right)
        const float spare = 7 * lay.u;
        for (const Node& n : g.nodes)
            if (!n.dead && &n - g.nodes.data() != e.a && &n - g.nodes.data() != e.b)
                walls.push_back({{n.x - n.w / 2 - spare, n.y - n.h / 2 - spare}, {n.x + n.w / 2 + spare, n.y + n.h / 2 + spare}});
        for (size_t c = 0; c < g.clusters.size(); ++c) {
            const int ref = as_cluster(static_cast<int>(c));
            if (ref == e.a || ref == e.b || lay.inside(e.a, static_cast<int>(c)) || lay.inside(e.b, static_cast<int>(c))) continue;
            const Cluster& cl = g.clusters[c];
            walls.push_back({{cl.x - spare, cl.y - spare}, {cl.x + cl.w + spare, cl.y + cl.h + spare}});
        }
        for (const Edge& o : g.edges)
            if (&o != &e && !o.label.empty() && o.label_w > 0 && o.from != o.to)
                walls.push_back({{o.label_at.x - o.label_w / 2 - spare, o.label_at.y - o.label_h / 2 - spare}, {o.label_at.x + o.label_w / 2 + spare, o.label_at.y + o.label_h / 2 + spare}});
        if (walls.size() * pts.size() * pts.size() > 4'000'000) continue;
        auto clear = [&](DiagramVec a, DiagramVec b) {
            for (const auto& [lo, hi] : walls) {
                // (Liang and Barsky's clipping: does the line meet the box at all)
                float t0 = 0, t1 = 1;
                const float dx = b.x - a.x, dy = b.y - a.y;
                const float pq[4][2] = {{-dx, a.x - lo.x}, {dx, hi.x - a.x}, {-dy, a.y - lo.y}, {dy, hi.y - a.y}};
                bool out = false;
                for (const auto& [pp, q] : pq) {
                    if (pp == 0) {
                        if (q < 0) out = true;
                        continue;
                    }
                    const float r = q / pp;
                    if (pp < 0) t0 = std::max(t0, r);
                    else t1 = std::min(t1, r);
                }
                if (!out && t0 <= t1) return false;
            }
            return true;
        };
        auto kept = [&](DiagramVec q) {
            for (const DiagramVec& kp : keep)
                if (std::fabs(kp.x - q.x) + std::fabs(kp.y - q.y) < 0.05f) return true;
            return false;
        };
        std::vector<DiagramVec> out{pts.front()};
        size_t i = 0;
        while (i + 1 < pts.size()) {
            size_t j = i + 1;
            for (size_t t = i + 1; t < pts.size(); ++t) {
                if (!clear(pts[i], pts[t])) break;
                j = t;
                if (kept(pts[t])) break;
            }
            out.push_back(pts[j]);
            i = j;
        }
        pts = std::move(out);
    }
    // Where on its side of a node each end goes: the edges leaving (or
    // meeting) the same side spread along it, in the order of where they go;
    // one alone a little off the middle towards where it goes.
    std::map<std::pair<size_t, bool>, float> spread;
    {
        std::map<std::pair<int, bool>, std::vector<std::pair<float, size_t>>> sides;
        for (size_t k = 0; k < g.edges.size(); ++k) {
            const std::vector<DiagramVec>& pts = chains[k];
            if (pts.size() < 2 || g.edges[k].line == kInvisible) continue;
            const DiagramVec axis = axis_of(g.edges[k].dir), across{std::fabs(axis.y), std::fabs(axis.x)};
            auto toward = [&](DiagramVec c, DiagramVec t) { return (t.x - c.x) * across.x + (t.y - c.y) * across.y; };
            sides[{g.edges[k].a, true}].push_back({toward(pts.front(), pts[1]), k});
            sides[{g.edges[k].b, false}].push_back({toward(pts.back(), pts[pts.size() - 2]), k});
        }
        for (auto& [side, list] : sides) {
            float bx, by, bw, bh;
            u8 shape;
            end_box(side.first, &bx, &by, &bw, &bh, &shape);
            const bool vert = Layout::vertical(g.edges[list[0].second].dir);
            const float span = vert ? bw : bh;
            const bool dot = shape == kShapeStart || shape == kShapeEnd;
            const float share = dot ? 0.08f : shape == kShapeDiamond ? (list.size() > 1 ? 0.2f : 0.06f) : shape == kShapeCircle || shape == kShapeDoubleCircle ? 0.22f : 0.32f;
            const float most = span * share;
            std::stable_sort(list.begin(), list.end());
            const size_t n = list.size();
            for (size_t i = 0; i < n; ++i) {
                float off = std::clamp(list[i].first * 0.5f, -most, most);
                if (n > 1) off = -most + 2 * most * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                spread[{list[i].second, side.second}] = off;
            }
        }
    }
    for (size_t k = 0; k < g.edges.size(); ++k) {
        Edge& e = g.edges[k];
        if (!e.laid || e.line == kInvisible) continue;
        Style es = g.link_default;
        es.over(e.style);
        const DiagramColour colour = !es.stroke.none() ? es.stroke : ink.edge;
        float width = es.width >= 0 ? es.width * u : e.line == kThick ? edge_w * 2.3f : edge_w;
        std::vector<float> dash = es.dash;
        for (float& v : dash) v *= u;
        if (dash.empty() && e.line == kDotted) dash = {2.2f * edge_w, 2.4f * edge_w};
        const float head = 8 * u + width * 1.6f;
        // Its tips: at the end the original arrow points to (to), and from.
        const u8 head_a = e.reversed ? e.head_to : e.head_from, head_b = e.reversed ? e.head_from : e.head_to;
        if (e.from == e.to) {  // a loop, beside its node
            const Node& n = g.nodes[static_cast<size_t>(e.from)];
            const bool vert = Layout::vertical(e.dir);
            const DiagramVec out = vert ? DiagramVec{1, 0} : DiagramVec{0, 1}, side = vert ? DiagramVec{0, 1} : DiagramVec{1, 0};
            const float half = (vert ? n.h : n.w) * 0.22f, reach = (vert ? n.w : n.h) / 2, bulge = 22 * u;
            const DiagramVec c{n.x + out.x * reach, n.y + out.y * reach};
            const DiagramVec s{c.x - side.x * half, c.y - side.y * half}, t{c.x + side.x * half, c.y + side.y * half};
            const DiagramVec c1{s.x + out.x * bulge - side.x * half, s.y + out.y * bulge - side.y * half}, c2{t.x + out.x * bulge + side.x * half, t.y + out.y * bulge + side.y * half};
            DiagramVec dir{t.x - c2.x, t.y - c2.y};
            const float len = std::hypot(dir.x, dir.y);
            dir = {dir.x / len, dir.y / len};
            const float back = diagram_head(*d, head_b, t, dir, head, colour, width);
            RasterPath path;
            path.move_to(s.x, s.y);
            path.cubic_to(c1.x, c1.y, c2.x, c2.y, t.x - dir.x * back, t.y - dir.y * back);
            if (geo) {
                DiagramGeometry::Line gl{e.from >= 0 ? n.id : "", n.id, diagram_sample(path, 12)};
                gl.pts.back() = {t.x, t.y};
                geo->lines.push_back(std::move(gl));
            }
            d->shape(std::move(path), {}, colour, width, dash, RasterStroke::kRound);
            if (!e.label.empty()) {
                float lw = 0;
                for (const std::string& l : e.label) lw = std::max(lw, font.width(l, size));
                const DiagramVec at{c.x + out.x * (bulge + 6 * u) + (vert ? lw / 2 + 6 * u : 0), c.y + out.y * (bulge + 6 * u) + (vert ? 0 : static_cast<float>(e.label.size()) * line / 2 + 3 * u)};
                e.label_w = lw + 12 * u, e.label_h = static_cast<float>(e.label.size()) * line + 6 * u;
                labels.push_back({&e, at});
            }
            continue;
        }
        std::vector<DiagramVec>& pts = chains[k];
        if (pts.size() < 2) continue;
        const DiagramVec axis = axis_of(e.dir), across{std::fabs(axis.y), std::fabs(axis.x)};
        // Each end on its node's border: where the edge leaves (or meets) it
        // along the axis, off the middle as far as it was spread.
        auto attach = [&](int ref, DiagramVec centre, bool start) {
            float bx, by, bw, bh;
            u8 shape;
            end_box(ref, &bx, &by, &bw, &bh, &shape);
            const float off = spread[{k, start}];
            const DiagramVec base{centre.x + across.x * off, centre.y + across.y * off};
            const float far = (axis.x ? bw : bh) + 10;
            const float sgn = start ? 1.0f : -1.0f;
            const DiagramVec from{base.x + axis.x * far * sgn, base.y + axis.y * far * sgn};
            return diagram_shape_hit(shape, bx, by, bw, bh, u, from, base);
        };
        const DiagramVec start = attach(e.a, pts.front(), true);
        const DiagramVec end = attach(e.b, pts.back(), false);
        pts.front() = start, pts.back() = end;
        // Heads: the line stops short of a tip by the head's length.
        const float back_b = diagram_head(*d, head_b, end, axis, head, colour, width);
        const float back_a = diagram_head(*d, head_a, start, {-axis.x, -axis.y}, head, colour, width);
        std::vector<DiagramVec> line_pts = pts;
        line_pts.front() = {start.x + axis.x * back_a, start.y + axis.y * back_a};
        line_pts.back() = {end.x - axis.x * back_b, end.y - axis.y * back_b};
        RasterPath path;
        diagram_curve(line_pts, axis, axis, &path);
        if (geo) {
            const int from = e.reversed ? e.b : e.a, to = e.reversed ? e.a : e.b;
            auto name = [&](int ref) { return is_cluster(ref) ? g.clusters[static_cast<size_t>(cluster_of(ref))].id : g.nodes[static_cast<size_t>(ref)].id; };
            DiagramGeometry::Line gl{name(from), name(to), diagram_sample(path, 16)};
            gl.pts.front() = {start.x, start.y}, gl.pts.back() = {end.x, end.y};
            if (e.reversed) std::reverse(gl.pts.begin(), gl.pts.end());
            geo->lines.push_back(std::move(gl));
        }
        d->shape(std::move(path), {}, colour, width, dash, dash.empty() ? RasterStroke::kButt : RasterStroke::kRound);
        if (!e.label.empty() && e.label_level >= -1 && e.label_w > 0) labels.push_back({&e, e.label_at});
    }
    // Notes' lines to what they are beside.
    for (const Node& n : g.nodes) {
        if (n.dead || n.note_of == -1) continue;
        float bx, by, bw, bh;
        u8 shape;
        end_box(n.note_of, &bx, &by, &bw, &bh, &shape);
        const DiagramVec tc{bx + bw / 2, by + bh / 2}, nc{n.x, n.y};
        const DiagramVec a = diagram_shape_hit(kShapeNote, n.x - n.w / 2, n.y - n.h / 2, n.w, n.h, u, tc, nc);
        const DiagramVec b = diagram_shape_hit(shape, bx, by, bw, bh, u, nc, tc);
        RasterPath path;
        path.move_to(a.x, a.y), path.line_to(b.x, b.y);
        d->shape(std::move(path), {}, ink.note_stroke, stroke_w, {3 * u, 3 * u});
    }

    // Subgraphs' titles, over the edges that pass under them.
    for (const int k : order) {
        const Cluster& cl = g.clusters[static_cast<size_t>(k)];
        if (cl.title.empty() || (cl.title.size() == 1 && cl.title[0].empty())) continue;
        const DiagramColour fill = !cl.style.fill.none() ? cl.style.fill : cl.state ? p.tone(kMdBorder, dark ? 0.07f : 0.05f) : ink.cluster_fill;
        const DiagramColour tc = !cl.style.text.none() ? cl.style.text : !cl.style.fill.none() ? readable_on(cl.style.fill, p) : ink.text;
        float tw = 0;
        for (const std::string& t : cl.title) tw = std::max(tw, font.width(t, size, true));
        const float th = lay.title_h * static_cast<float>(cl.title.size());
        const float top = cl.y + lay.pad * 0.5f;
        RasterPath back;
        back.rect(cl.x + cl.w / 2 - tw / 2 - 4 * u, top, tw + 8 * u, th - 2 * u, 3 * u);
        d->shape(std::move(back), fill);
        text_block(cl.title, cl.x + cl.w / 2, top + (th - 2 * u) / 2, tc, true);
    }

    // Nodes.
    for (const Node& n : g.nodes) {
        if (n.dead) continue;
        const float x = n.x - n.w / 2, y = n.y - n.h / 2;
        if (geo) geo->boxes.push_back({"node", n.id, x, y, n.w, n.h});
        RasterPath outline;
        diagram_shape(n.shape, x, y, n.w, n.h, u, &outline);
        const float sw = n.style.width >= 0 ? n.style.width * u : stroke_w;
        std::vector<float> dash = n.style.dash;
        for (float& v : dash) v *= u;
        switch (n.shape) {
            case kShapeStart:
            case kShapeBar: d->shape(std::move(outline), !n.style.fill.none() ? n.style.fill : ink.dot); continue;
            case kShapeEnd: {
                d->shape(std::move(outline), {}, !n.style.stroke.none() ? n.style.stroke : ink.dot, sw * 1.2f);
                RasterPath dot;
                dot.ellipse(n.x, n.y, n.w * 0.3f, n.h * 0.3f);
                d->shape(std::move(dot), !n.style.fill.none() ? n.style.fill : ink.dot);
                continue;
            }
            default: break;
        }
        const bool note = n.shape == kShapeNote;
        const DiagramColour fill = !n.style.fill.none() ? n.style.fill : note ? ink.note_fill : ink.node_fill;
        const DiagramColour stroke = !n.style.stroke.none() ? n.style.stroke : note ? ink.note_stroke : ink.node_stroke;
        d->shape(std::move(outline), fill, stroke, sw, dash);
        RasterPath extra;
        if (diagram_shape_extra(n.shape, x, y, n.w, n.h, u, &extra)) d->shape(std::move(extra), {}, stroke, sw);
        const DiagramColour tc = !n.style.text.none() ? n.style.text : !n.style.fill.none() ? readable_on(n.style.fill, p) : ink.text;
        float cy = n.y;
        if (n.shape == kShapeCylinder) cy += std::min(n.h / 4, n.w / 2 / (2.5f + n.w / (50 * u))) / 2;
        text_block(n.lines, n.x, cy, tc, n.style.bold);
    }

    // Edges' labels, on a pill each.
    for (const LabelAt& l : labels) {
        const Edge& e = *l.e;
        RasterPath pill;
        const float lx = l.at.x - e.label_w / 2, ly = l.at.y - e.label_h / 2;
        pill.rect(lx, ly, e.label_w, e.label_h, std::min(e.label_h / 2, 6 * u));
        Style es = g.link_default;
        es.over(e.style);
        d->shape(std::move(pill), ink.label_fill);
        text_block(e.label, l.at.x, l.at.y, !es.text.none() ? es.text : ink.text, false);
        if (geo) {
            const int from = e.reversed ? e.b : e.a, to = e.reversed ? e.a : e.b;
            auto name = [&](int ref) { return is_cluster(ref) ? g.clusters[static_cast<size_t>(cluster_of(ref))].id : g.nodes[static_cast<size_t>(ref)].id; };
            geo->boxes.push_back({"label", name(e.from == e.to ? e.from : from) + "->" + name(e.from == e.to ? e.to : to), lx, ly, e.label_w, e.label_h});
        }
    }
}

bool lay_out(Graph g, const DiagramStyle& style, Drawing* out, DiagramGeometry* geo) {
    resolve_styles(g);
    Layout lay(g, style, *style.font);
    float w = 0, h = 0;
    lay.absolute(&w, &h);
    draw(g, lay, style, w, h, out, geo);
    return true;
}

}  // namespace

bool flowchart_layout(const std::vector<std::string>& st, bool state, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    Graph g;
    if (!(state ? parse_state(st, g, err) : parse_flowchart(st, g, err))) return false;
    if (g.nodes.empty() && g.clusters.empty()) {
        *err = "nothing in it";
        return false;
    }
    DiagramGeometry across_geo;
    lay_out(g, style, out, geo ? &across_geo : nullptr);
    // Too wide across for the room: down instead, if that is less too wide.
    if (out->w > style.room && (g.dir == 'R' || g.dir == 'L')) {
        Graph down = g;
        down.dir = 'D';
        Drawing d2;
        DiagramGeometry g2;
        lay_out(down, style, &d2, geo ? &g2 : nullptr);
        if (std::min(1.0f, style.room / d2.w) > std::min(1.0f, style.room / out->w) * 1.25f) {
            *out = std::move(d2);
            across_geo = std::move(g2);
        }
    }
    if (geo) *geo = std::move(across_geo);
    return true;
}

}  // namespace blit
