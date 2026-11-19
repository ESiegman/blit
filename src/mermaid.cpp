#include "mermaid.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "utf8.hpp"

namespace blit {

namespace {

// ---------------------------------------------------------------- text

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

int cols_of(std::string_view s) {
    int w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        w += static_cast<int>(cp_width(d.cp));
        i += d.len ? d.len : 1;
    }
    return w;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A label as it is shown: quotes and Markdown's backticks off, <br> a new line, a few entities.
std::vector<std::string> label_lines(std::string_view s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2);
    std::string t(s);
    for (const char* br : {"<br/>", "<br />", "<br>", "<BR>", "\\n"})
        for (size_t at; (at = t.find(br)) != std::string::npos;) t.replace(at, std::string_view(br).size(), "\n");
    for (const auto& [e, r] : std::initializer_list<std::pair<const char*, const char*>>{{"&quot;", "\""}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"#quot;", "\""}})
        for (size_t at; (at = t.find(e)) != std::string::npos;) t.replace(at, std::string_view(e).size(), r);
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t nl = t.find('\n', at);
        out.emplace_back(trim(std::string_view(t).substr(at, nl == std::string::npos ? std::string::npos : nl - at)));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return out;
}

// The diagram's statements: its lines (and what ; separates), without comments.
std::vector<std::string> statements(std::string_view src, bool split_semicolons) {
    std::vector<std::string> out;
    for (size_t at = 0; at <= src.size();) {
        const size_t nl = src.find('\n', at);
        std::string_view l = src.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? src.size() + 1 : nl + 1;
        if (const size_t c = l.find("%%"); c != std::string_view::npos) l = l.substr(0, c);
        if (split_semicolons) {
            bool quoted = false;
            size_t from = 0;
            for (size_t i = 0; i < l.size(); ++i) {
                if (l[i] == '"') quoted = !quoted;
                if (l[i] == ';' && !quoted) {
                    if (!trim(l.substr(from, i - from)).empty()) out.emplace_back(trim(l.substr(from, i - from)));
                    from = i + 1;
                }
            }
            l = l.substr(from);
        }
        if (!trim(l).empty()) out.emplace_back(trim(l));
    }
    return out;
}

// ---------------------------------------------------------------- the canvas

enum : u8 { kUp = 1, kDown = 2, kLeft = 4, kRight = 8 };
enum LineKind : u8 { kLight, kHeavy, kDotted };

struct Cell {
    std::string text;  // a character of text (or of a box), drawn as it is
    u8 ink = kMdText, style = 0;
    u8 lines = 0, kind = kLight;  // or the lines through it, which make a box-drawing character
    u8 line_ink = kMdEdge;
    bool cont = false;  // the second half of a wide character
};

struct Canvas {
    int w = 0, h = 0;
    std::vector<Cell> cells;

    void fit(int x, int y) {
        if (x < w && y < h) return;
        if (x < w) {  // (rows added below: those there stay where they are, and the vector grows by more than a row at a time)
            h = y + 1;
            cells.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
            return;
        }
        const int nw = std::max(w, x + 1), nh = std::max(h, y + 1);
        std::vector<Cell> next(static_cast<size_t>(nw) * static_cast<size_t>(nh));
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c) next[static_cast<size_t>(r) * static_cast<size_t>(nw) + static_cast<size_t>(c)] = std::move(cells[static_cast<size_t>(r) * static_cast<size_t>(w) + static_cast<size_t>(c)]);
        cells = std::move(next);
        w = nw, h = nh;
    }
    Cell& at(int x, int y) {
        fit(x, y);
        return cells[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    }
    void text(int x, int y, std::string_view s, u8 ink, u8 style = 0) {
        if (x < 0 || y < 0) return;
        const u8* p = reinterpret_cast<const u8*>(s.data());
        int last = -1;  // the cell the last character went in
        for (size_t i = 0; i < s.size();) {
            const Decoded d = decode_utf8(p + i, s.size() - i);
            const size_t len = d.len ? d.len : 1;
            if (last >= 0 && cp_width(d.cp) == 0) {  // a mark: with the character it is on
                at(last, y).text.append(s.substr(i, len));
                i += len;
                continue;
            }
            last = x;
            Cell& c = at(x, y);
            c.text.assign(s.substr(i, len));
            c.ink = ink, c.style = style, c.lines = 0, c.cont = false;
            if (cp_width(d.cp) == 2) {
                Cell& c2 = at(x + 1, y);
                c2.text.clear(), c2.lines = 0, c2.cont = true;
                ++x;
            }
            ++x;
            i += len;
        }
    }
    void bits(int x, int y, u8 b, u8 kind, u8 ink) {
        if (x < 0 || y < 0) return;
        Cell& c = at(x, y);
        if (!c.text.empty() || c.cont) return;  // (text stays over lines)
        c.lines |= b;
        if (kind == kHeavy || c.kind == kLight) c.kind = kind;
        c.line_ink = ink;
    }
    // A straight line from one point to another, inclusive.
    void line(int x0, int y0, int x1, int y1, u8 kind, u8 ink) {
        if (y0 == y1) {
            const int a = std::min(x0, x1), b = std::max(x0, x1);
            for (int x = a; x <= b; ++x) bits(x, y0, static_cast<u8>((x > a ? kLeft : 0) | (x < b ? kRight : 0)), kind, ink);
        } else if (x0 == x1) {
            const int a = std::min(y0, y1), b = std::max(y0, y1);
            for (int y = a; y <= b; ++y) bits(x0, y, static_cast<u8>((y > a ? kUp : 0) | (y < b ? kDown : 0)), kind, ink);
        }
    }
    void box(int x, int y, int bw, int bh, const char* const* corners, const char* hz, const char* vt, u8 ink) {
        text(x, y, corners[0], ink), text(x + bw - 1, y, corners[1], ink);
        text(x, y + bh - 1, corners[2], ink), text(x + bw - 1, y + bh - 1, corners[3], ink);
        for (int c = x + 1; c < x + bw - 1; ++c) text(c, y, hz, ink), text(c, y + bh - 1, hz, ink);
        for (int r = y + 1; r < y + bh - 1; ++r) text(x, r, vt, ink), text(x + bw - 1, r, vt, ink);
        for (int r = y + 1; r < y + bh - 1; ++r)
            for (int c = x + 1; c < x + bw - 1; ++c) text(c, r, " ", kMdText);
    }

    static const char* glyph(u8 b, u8 kind) {
        if (kind == kHeavy) {
            switch (b) {
                case kLeft | kRight: case kLeft: case kRight: return "\xE2\x94\x81";
                case kUp | kDown: case kUp: case kDown: return "\xE2\x94\x83";
                case kDown | kRight: return "\xE2\x94\x8F";
                case kDown | kLeft: return "\xE2\x94\x93";
                case kUp | kRight: return "\xE2\x94\x97";
                case kUp | kLeft: return "\xE2\x94\x9B";
                case kUp | kDown | kRight: return "\xE2\x94\xA3";
                case kUp | kDown | kLeft: return "\xE2\x94\xAB";
                case kDown | kLeft | kRight: return "\xE2\x94\xB3";
                case kUp | kLeft | kRight: return "\xE2\x94\xBB";
                default: return "\xE2\x95\x8B";
            }
        }
        switch (b) {
            case kLeft | kRight: case kLeft: case kRight: return kind == kDotted ? "\xE2\x94\x84" : "\xE2\x94\x80";
            case kUp | kDown: case kUp: case kDown: return kind == kDotted ? "\xE2\x94\x86" : "\xE2\x94\x82";
            case kDown | kRight: return "\xE2\x95\xAD";
            case kDown | kLeft: return "\xE2\x95\xAE";
            case kUp | kRight: return "\xE2\x95\xB0";
            case kUp | kLeft: return "\xE2\x95\xAF";
            case kUp | kDown | kRight: return "\xE2\x94\x9C";
            case kUp | kDown | kLeft: return "\xE2\x94\xA4";
            case kDown | kLeft | kRight: return "\xE2\x94\xAC";
            case kUp | kLeft | kRight: return "\xE2\x94\xB4";
            default: return "\xE2\x94\xBC";
        }
    }

    std::vector<MdRow> rows(int width, int* cut) const {
        std::vector<MdRow> out;
        *cut = 0;
        for (int y = 0; y < h; ++y) {
            MdRow r;
            int x_end = w;
            while (x_end > 0) {
                const Cell& c = cells[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x_end - 1)];
                if ((c.text.empty() || c.text == " ") && !c.lines && !c.cont) --x_end;
                else break;
            }
            const int show = width > 0 && x_end > width ? width - 1 : x_end;
            if (x_end > show) *cut = std::max(*cut, x_end - show);
            for (int x = 0; x < show; ++x) {
                const Cell& c = cells[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
                if (c.cont) continue;
                std::string t;
                u8 ink = c.ink, style = c.style;
                if (!c.text.empty()) t = c.text;
                else if (c.lines) t = glyph(c.lines, c.kind), ink = c.line_ink, style = 0;
                else t = " ";
                if (!r.runs.empty() && r.runs.back().ink == ink && r.runs.back().style == style) r.runs.back().text += t;
                else r.runs.push_back({t, ink, style});
            }
            if (x_end > show) r.runs.push_back({"\xE2\x80\xA6", kMdMuted});
            out.push_back(std::move(r));
        }
        return out;
    }
};

// ---------------------------------------------------------------- graphs (flowcharts, state diagrams)

enum Shape { kRect, kRound, kDiamond, kDouble, kDot, kEndDot };
enum Head { kNoHead, kArrow, kCircle, kCross };

struct Node {
    std::string id;
    std::vector<std::string> lines;
    Shape shape = kRect;
    bool given = false;  // its shape and text were said (not just its name used)
    int w = 1, h = 1;
    int rank = 0;
    int x = 0, y = 0;
    bool dummy = false, loop = false;
};

struct Edge {
    int from = 0, to = 0;
    std::string label;
    u8 kind = kLight;
    bool hidden = false;
    Head head_from = kNoHead, head_to = kArrow;
    std::vector<int> chain;  // the nodes it goes through, rank by rank (dummies between)
    bool flipped = false;    // drawn from `to` to `from` (it went back up, in a cycle)
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::map<std::string, int> ids;
    char dir = 'D';  // D (top to bottom), U, R (left to right), L

    int node(const std::string& id) {
        if (const auto it = ids.find(id); it != ids.end()) return it->second;
        Node n;
        n.id = id;
        n.lines = {id};
        nodes.push_back(n);
        return ids[id] = static_cast<int>(nodes.size()) - 1;
    }
};

bool id_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80; }

// A node (its name, and its shape and text if they follow) at s[i].
bool parse_node(std::string_view s, size_t& i, Graph& g, int* out, std::string* err) {
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
    if (const size_t cls = id.find(":::"); cls != std::string::npos) id.resize(cls);
    const int n = g.node(id);
    struct Opener {
        const char* open;
        const char* close;
        Shape shape;
    };
    static const Opener kOpeners[] = {{"(((", ")))", kRound}, {"((", "))", kRound},   {"([", "])", kRound},  {"[[", "]]", kDouble}, {"[(", ")]", kRound},
                                      {"{{", "}}", kDiamond},  {"[/", "/]", kRect},     {"[\\", "\\]", kRect}, {"[/", "\\]", kRect},  {"[\\", "/]", kRect},
                                      {"[", "]", kRect},       {"(", ")", kRound},      {"{", "}", kDiamond},  {">", "]", kRect}};
    for (const Opener& o : kOpeners) {
        if (!s.substr(i).starts_with(o.open)) continue;
        size_t from = i + std::string_view(o.open).size();
        size_t end;
        if (from < s.size() && s[from] == '"') {
            const size_t q = s.find('"', from + 1);
            if (q == std::string_view::npos) break;
            end = s.find(o.close, q + 1);
        } else {
            end = s.find(o.close, from);
        }
        if (end == std::string_view::npos) continue;
        Node& nd = g.nodes[static_cast<size_t>(n)];
        nd.lines = label_lines(s.substr(from, end - from));
        nd.shape = o.shape;
        nd.given = true;
        i = end + std::string_view(o.close).size();
        break;
    }
    if (s.substr(i).starts_with(":::")) {  // (a class: not drawn)
        i += 3;
        while (i < s.size() && id_char(s[i])) ++i;
    }
    *out = n;
    return true;
}

// An arrow at s[i] (with what it says, if it says anything).
bool parse_edge(std::string_view s, size_t& i, Edge* e) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    if (j < s.size() && (s[j] == '<' || ((s[j] == 'o' || s[j] == 'x') && j + 1 < s.size() && (s[j + 1] == '-' || s[j + 1] == '=')))) {
        e->head_from = s[j] == '<' ? kArrow : s[j] == 'o' ? kCircle : kCross;
        ++j;
    }
    if (j + 1 >= s.size() || std::string_view("-=~").find(s[j]) == std::string_view::npos) return false;
    // "-- text -->", "-. text .->", "== text ==>": the text between.
    const std::string_view two = s.substr(j, 2);
    bool dotted_text = false;
    if ((two == "--" || two == "-." || two == "==") && j + 2 < s.size() && s[j + 2] == ' ') {
        const std::string_view ends = two == "--" ? "--" : two == "-." ? ".-" : "==";
        const size_t close = s.find(ends, j + 3);
        if (close != std::string_view::npos && close > j + 3) {
            e->label = std::string(trim(s.substr(j + 2, close - j - 2)));
            j = close;
            if (two == "-.") ++j, dotted_text = true;  // (".-": the dot)
        }
    }
    const size_t body = j;
    while (j < s.size() && std::string_view("-=.~").find(s[j]) != std::string_view::npos) ++j;
    const std::string_view run = s.substr(body, j - body);
    if (run.size() < 2 && !(run == "-" && e->label.size())) return false;
    e->kind = run.find('=') != std::string_view::npos ? kHeavy : (dotted_text || run.find('.') != std::string_view::npos) ? kDotted : kLight;
    e->hidden = run.find('~') != std::string_view::npos;
    e->head_to = kNoHead;
    if (j < s.size() && (s[j] == '>' || ((s[j] == 'o' || s[j] == 'x') && (j + 1 >= s.size() || !id_char(s[j + 1]))))) {
        e->head_to = s[j] == '>' ? kArrow : s[j] == 'o' ? kCircle : kCross;
        ++j;
    }
    while (j < s.size() && s[j] == ' ') ++j;
    if (j < s.size() && s[j] == '|') {
        const size_t close = s.find('|', j + 1);
        if (close != std::string_view::npos) {
            e->label = std::string(trim(s.substr(j + 1, close - j - 1)));
            j = close + 1;
        }
    }
    if (e->label.size() >= 2 && e->label.front() == '"' && e->label.back() == '"') e->label = e->label.substr(1, e->label.size() - 2);
    i = j;
    return true;
}

bool parse_flowchart(const std::vector<std::string>& st, Graph& g, std::string* err) {
    {
        std::string head = lower(st[0]);
        const size_t sp = head.find(' ');
        const std::string d = sp == std::string::npos ? "td" : std::string(trim(std::string_view(head).substr(sp + 1)));
        g.dir = d.starts_with("lr") ? 'R' : d.starts_with("rl") ? 'L' : d.starts_with("bt") ? 'U' : 'D';
    }
    for (size_t k = 1; k < st.size(); ++k) {
        const std::string_view s = st[k];
        const std::string l = lower(s);
        if (l == "end" || l.starts_with("classdef ") || l.starts_with("class ") || l.starts_with("style ") || l.starts_with("linkstyle ") || l.starts_with("click ") ||
            l.starts_with("acctitle") || l.starts_with("accdescr"))
            continue;
        if (l.starts_with("direction ")) {
            const std::string d(trim(std::string_view(l).substr(10)));
            g.dir = d == "lr" ? 'R' : d == "rl" ? 'L' : d == "bt" ? 'U' : 'D';
            continue;
        }
        if (l.starts_with("subgraph")) continue;  // (its nodes are drawn; the frame round them is not)
        size_t i = 0;
        std::vector<int> left;
        auto group = [&](std::vector<int>& out) {
            out.clear();
            for (;;) {
                int n = 0;
                if (!parse_node(s, i, g, &n, err)) return false;
                out.push_back(n);
                while (i < s.size() && s[i] == ' ') ++i;
                if (i < s.size() && s[i] == '&') {
                    ++i;
                    continue;
                }
                return true;
            }
        };
        if (!group(left)) {
            *err = "line " + std::to_string(k + 1) + ": " + *err;
            return false;
        }
        for (;;) {
            while (i < s.size() && s[i] == ' ') ++i;
            if (i >= s.size()) break;
            Edge e;
            if (!parse_edge(s, i, &e)) {
                *err = "line " + std::to_string(k + 1) + ": an arrow is wanted at \"" + std::string(s.substr(i, 12)) + "\"";
                return false;
            }
            std::vector<int> right;
            if (!group(right)) {
                *err = "line " + std::to_string(k + 1) + ": " + *err;
                return false;
            }
            for (const int a : left)
                for (const int b : right) {
                    Edge ed = e;
                    ed.from = a, ed.to = b;
                    g.edges.push_back(ed);
                }
            left = right;
        }
    }
    return true;
}

bool parse_state(const std::vector<std::string>& st, Graph& g, std::string* err) {
    (void)err;
    int start = -1, end = -1;
    auto state = [&](std::string_view name, bool as_source) {
        name = trim(name);
        if (name == "[*]") {
            int& which = as_source ? start : end;
            if (which < 0) {
                which = g.node(as_source ? "[*]start" : "[*]end");
                Node& n = g.nodes[static_cast<size_t>(which)];
                n.shape = as_source ? kDot : kEndDot;
                n.lines = {as_source ? "\xE2\x97\x8F" : "\xE2\x97\x89"};
                n.given = true;
            }
            return which;
        }
        const int n = g.node(std::string(name));
        if (!g.nodes[static_cast<size_t>(n)].given) g.nodes[static_cast<size_t>(n)].shape = kRound;
        return n;
    };
    bool in_note = false;
    for (size_t k = 1; k < st.size(); ++k) {
        std::string_view s = st[k];
        const std::string l = lower(s);
        if (in_note) {
            in_note = l != "end note";
            continue;
        }
        if (l.starts_with("note ")) {
            in_note = s.find(':') == std::string_view::npos;
            continue;
        }
        if (l == "}" || l == "--" || l.starts_with("classdef ") || l.starts_with("class ") || l.starts_with("acctitle") || l.starts_with("accdescr")) continue;
        if (l.starts_with("direction ")) {
            const std::string d(trim(std::string_view(l).substr(10)));
            g.dir = d == "lr" ? 'R' : d == "rl" ? 'L' : d == "bt" ? 'U' : 'D';
            continue;
        }
        if (l.starts_with("state ")) {
            std::string_view rest = trim(s.substr(6));
            if (rest.ends_with('{')) rest = trim(rest.substr(0, rest.size() - 1));
            std::string desc;
            if (rest.starts_with('"')) {
                const size_t q = rest.find('"', 1);
                if (q != std::string_view::npos) {
                    desc = std::string(rest.substr(1, q - 1));
                    rest = trim(rest.substr(q + 1));
                    if (lower(rest).starts_with("as ")) rest = trim(rest.substr(3));
                }
            }
            std::string_view name = rest;
            Shape shape = kRound;
            if (const size_t lt = rest.find("<<"); lt != std::string_view::npos) {
                const std::string kind = lower(rest.substr(lt));
                name = trim(rest.substr(0, lt));
                shape = kind.find("choice") != std::string::npos ? kDiamond : kRect;
            }
            const int n = state(name, false);
            Node& nd = g.nodes[static_cast<size_t>(n)];
            nd.shape = shape;
            if (!desc.empty()) nd.lines = label_lines(desc);
            else if (shape == kRect) nd.lines = {""};
            nd.given = true;
            continue;
        }
        const size_t arrow = s.find("-->");
        if (arrow != std::string_view::npos) {
            std::string_view rest = s.substr(arrow + 3);
            std::string label;
            if (const size_t colon = rest.find(':'); colon != std::string_view::npos) {
                label = std::string(trim(rest.substr(colon + 1)));
                rest = rest.substr(0, colon);
            }
            Edge e;
            e.from = state(s.substr(0, arrow), true);
            e.to = state(rest, false);
            e.label = label;
            g.edges.push_back(e);
            continue;
        }
        if (const size_t colon = s.find(':'); colon != std::string_view::npos) {  // a state's description
            const int n = state(s.substr(0, colon), false);
            Node& nd = g.nodes[static_cast<size_t>(n)];
            const std::vector<std::string> more = label_lines(s.substr(colon + 1));
            if (!nd.given) nd.lines.clear();
            nd.lines.insert(nd.lines.end(), more.begin(), more.end());
            nd.given = true;
            continue;
        }
        state(s.ends_with('{') ? trim(s.substr(0, s.size() - 1)) : s, false);
    }
    return true;
}

// Lays the graph out in ranks (top to bottom, or left to right), routes
// its arrows, and draws it.
Canvas draw_graph(Graph g) {
    const bool vertical = g.dir == 'D' || g.dir == 'U';
    // Sizes.
    for (Node& n : g.nodes) {
        if (n.shape == kDot || n.shape == kEndDot) {
            n.w = n.h = 1;
            continue;
        }
        int lw = 1;
        for (const std::string& l : n.lines) lw = std::max(lw, cols_of(l));
        n.w = lw + 4;
        n.h = static_cast<int>(n.lines.size()) + 2;
    }
    const int real = static_cast<int>(g.nodes.size());
    // Cycles broken: an arrow back to a node on the way there is drawn the other way round.
    std::vector<std::vector<int>> out(static_cast<size_t>(real));
    for (size_t k = 0; k < g.edges.size(); ++k) {
        Edge& e = g.edges[k];
        if (e.from == e.to) {
            g.nodes[static_cast<size_t>(e.from)].loop = true;
            continue;
        }
        out[static_cast<size_t>(e.from)].push_back(static_cast<int>(k));
    }
    {
        std::vector<u8> state(static_cast<size_t>(real), 0);  // 0 unseen, 1 on the way, 2 done
        std::vector<std::pair<int, size_t>> stack;
        for (int root = 0; root < real; ++root) {
            if (state[static_cast<size_t>(root)]) continue;
            stack.push_back({root, 0});
            state[static_cast<size_t>(root)] = 1;
            while (!stack.empty()) {
                auto& [v, next] = stack.back();
                if (next < out[static_cast<size_t>(v)].size()) {
                    Edge& e = g.edges[static_cast<size_t>(out[static_cast<size_t>(v)][next++])];
                    if (state[static_cast<size_t>(e.to)] == 1) e.flipped = true;
                    else if (state[static_cast<size_t>(e.to)] == 0) {
                        state[static_cast<size_t>(e.to)] = 1;
                        stack.push_back({e.to, 0});
                    }
                } else {
                    state[static_cast<size_t>(v)] = 2;
                    stack.pop_back();
                }
            }
        }
    }
    // Ranks: each node one past the furthest of those before it.
    for (int pass = 0; pass < real + 1; ++pass) {
        bool moved = false;
        for (const Edge& e : g.edges) {
            if (e.from == e.to) continue;
            const int a = e.flipped ? e.to : e.from, b = e.flipped ? e.from : e.to;
            if (g.nodes[static_cast<size_t>(b)].rank < g.nodes[static_cast<size_t>(a)].rank + 1) {
                g.nodes[static_cast<size_t>(b)].rank = g.nodes[static_cast<size_t>(a)].rank + 1;
                moved = true;
            }
        }
        if (!moved) break;
    }
    int ranks = 1;
    for (const Node& n : g.nodes) ranks = std::max(ranks, n.rank + 1);
    // Arrows across more than one rank go through a point on each between.
    for (Edge& e : g.edges) {
        if (e.from == e.to) continue;
        const int a = e.flipped ? e.to : e.from, b = e.flipped ? e.from : e.to;
        e.chain = {a};
        for (int r = g.nodes[static_cast<size_t>(a)].rank + 1; r < g.nodes[static_cast<size_t>(b)].rank; ++r) {
            Node d;
            d.dummy = true;
            d.rank = r;
            d.w = d.h = 1;
            g.nodes.push_back(d);
            e.chain.push_back(static_cast<int>(g.nodes.size()) - 1);
        }
        e.chain.push_back(b);
    }
    // Order in each rank: as first seen, then each by the middle of its neighbours', back and forth.
    std::vector<std::vector<int>> layer(static_cast<size_t>(ranks));
    for (int n = 0; n < static_cast<int>(g.nodes.size()); ++n) layer[static_cast<size_t>(g.nodes[static_cast<size_t>(n)].rank)].push_back(n);
    std::vector<std::vector<int>> up(g.nodes.size()), down(g.nodes.size());
    for (const Edge& e : g.edges)
        for (size_t k = 0; k + 1 < e.chain.size(); ++k) {
            down[static_cast<size_t>(e.chain[k])].push_back(e.chain[k + 1]);
            up[static_cast<size_t>(e.chain[k + 1])].push_back(e.chain[k]);
        }
    std::vector<double> index(g.nodes.size(), 0);
    auto renumber = [&] {
        for (const std::vector<int>& l : layer)
            for (size_t k = 0; k < l.size(); ++k) index[static_cast<size_t>(l[k])] = static_cast<double>(k);
    };
    renumber();
    for (int sweep = 0; sweep < 8; ++sweep) {
        const bool downward = sweep % 2 == 0;
        for (int step = 1; step < ranks; ++step) {
            const int r = downward ? step : ranks - 1 - step;
            std::vector<std::pair<double, int>> keyed;
            for (const int n : layer[static_cast<size_t>(r)]) {
                const std::vector<int>& nb = downward ? up[static_cast<size_t>(n)] : down[static_cast<size_t>(n)];
                double sum = 0;
                for (const int m : nb) sum += index[static_cast<size_t>(m)];
                keyed.push_back({nb.empty() ? index[static_cast<size_t>(n)] : sum / static_cast<double>(nb.size()), n});
            }
            std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t k = 0; k < keyed.size(); ++k) layer[static_cast<size_t>(r)][k] = keyed[k].second;
            renumber();
        }
    }
    // Places along each rank: packed, then each pulled towards its neighbours' middle.
    auto breadth = [&](int n) { return vertical ? g.nodes[static_cast<size_t>(n)].w : g.nodes[static_cast<size_t>(n)].h; };
    const int spacing = vertical ? 3 : 1;
    std::vector<double> at(g.nodes.size(), 0);  // the start of each, along its rank
    for (const std::vector<int>& l : layer) {
        double p = 0;
        for (const int n : l) {
            at[static_cast<size_t>(n)] = p;
            p += breadth(n) + spacing;
        }
    }
    for (int pass = 0; pass < 6; ++pass) {
        const bool downward = pass % 2 == 0;
        for (int step = 0; step < ranks; ++step) {
            const int r = downward ? step : ranks - 1 - step;
            const std::vector<int>& l = layer[static_cast<size_t>(r)];
            std::vector<double> want(l.size());
            for (size_t k = 0; k < l.size(); ++k) {
                const int n = l[k];
                const std::vector<int>& nb = downward ? up[static_cast<size_t>(n)] : down[static_cast<size_t>(n)];
                if (nb.empty()) {
                    want[k] = at[static_cast<size_t>(n)];
                    continue;
                }
                double sum = 0;
                for (const int m : nb) sum += at[static_cast<size_t>(m)] + breadth(m) / 2;  // (whole cells: a node under another lines up exactly)
                want[k] = sum / static_cast<double>(nb.size()) - breadth(n) / 2;
            }
            // In order, none closer than `spacing` to the one before; then back from the right likewise.
            for (size_t k = 0; k < l.size(); ++k) {
                double p = want[k];
                if (k > 0) p = std::max(p, at[static_cast<size_t>(l[k - 1])] + breadth(l[k - 1]) + spacing);
                at[static_cast<size_t>(l[k])] = p;
            }
            for (size_t k = l.size(); k-- > 1;) {
                const double limit = at[static_cast<size_t>(l[k])] - spacing - breadth(l[k - 1]);
                if (at[static_cast<size_t>(l[k - 1])] > limit) continue;
                at[static_cast<size_t>(l[k - 1])] = std::min(std::max(want[k - 1], at[static_cast<size_t>(l[k - 1])]), limit);
            }
        }
    }
    double least = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n) least = std::min(least, at[n]);
    // Ranks along the other way: each as thick as its thickest node, gaps between for the arrows.
    std::vector<int> thick(static_cast<size_t>(ranks), 1), gap(static_cast<size_t>(ranks), 3), start(static_cast<size_t>(ranks), 0);
    for (const Node& n : g.nodes) thick[static_cast<size_t>(n.rank)] = std::max(thick[static_cast<size_t>(n.rank)], vertical ? n.h : n.w);
    for (const Edge& e : g.edges)  // (room for what arrows say: a row more down, or the words' width across)
        if (!e.chain.empty() && !e.label.empty()) {
            const int r = g.nodes[static_cast<size_t>(e.chain[0])].rank;
            gap[static_cast<size_t>(r)] = std::max(gap[static_cast<size_t>(r)], vertical ? 4 : cols_of(e.label) + 4);
        }
    for (int r = 1; r < ranks; ++r) start[static_cast<size_t>(r)] = start[static_cast<size_t>(r - 1)] + thick[static_cast<size_t>(r - 1)] + gap[static_cast<size_t>(r - 1)];
    for (size_t n = 0; n < g.nodes.size(); ++n) {
        Node& nd = g.nodes[n];
        const int along = static_cast<int>(std::lround(at[n] - least));
        const int r0 = start[static_cast<size_t>(nd.rank)], t = thick[static_cast<size_t>(nd.rank)];
        if (vertical) {
            nd.x = along;
            nd.y = nd.dummy ? r0 : r0 + (t - nd.h) / 2;
            if (nd.dummy) nd.h = t;
        } else {
            nd.y = along;
            nd.x = nd.dummy ? r0 : r0 + (t - nd.w) / 2;
            if (nd.dummy) nd.w = t;
        }
    }
    // Where each arrow leaves and arrives: spread along the node's side, in the order of the other ends.
    struct Seg {
        int edge, a, b;
        int pa = 0, pb = 0;  // the offsets along a's far side and b's near side
    };
    std::vector<Seg> segs;
    for (size_t k = 0; k < g.edges.size(); ++k)
        for (size_t j = 0; j + 1 < g.edges[k].chain.size(); ++j) segs.push_back({static_cast<int>(k), g.edges[k].chain[j], g.edges[k].chain[j + 1]});
    auto centre = [&](int n) {
        const Node& nd = g.nodes[static_cast<size_t>(n)];
        return vertical ? nd.x + nd.w / 2.0 : nd.y + nd.h / 2.0;
    };
    for (int side = 0; side < 2; ++side) {
        std::map<int, std::vector<size_t>> by;
        for (size_t s = 0; s < segs.size(); ++s) by[side == 0 ? segs[s].a : segs[s].b].push_back(s);
        for (auto& [n, list] : by) {
            std::stable_sort(list.begin(), list.end(), [&](size_t p, size_t q) { return centre(side == 0 ? segs[p].b : segs[p].a) < centre(side == 0 ? segs[q].b : segs[q].a); });
            const int b = breadth(n);
            for (size_t k = 0; k < list.size(); ++k) {
                int off = b <= 2 ? 0 : 1 + static_cast<int>((static_cast<long>(b) - 2) * static_cast<long>(k + 1) / static_cast<long>(list.size() + 1));
                if (list.size() == 1) off = b / 2;
                (side == 0 ? segs[list[k]].pa : segs[list[k]].pb) = off;
            }
        }
    }
    // Drawn into a canvas in the layout's own direction, then turned round if it goes up or left.
    struct Route {
        std::vector<std::pair<int, int>> pts;
        int edge;
        Head head_end = kNoHead, head_start = kNoHead;
    };
    std::vector<Route> routes;
    struct Text {
        int x, y;
        std::string s;
        int line_x = -1;  // beside the way down at this column: may go to its other side, or a row up or down, if that is taken
    };
    std::vector<Text> labels;
    for (const Seg& s : segs) {
        const Node& a = g.nodes[static_cast<size_t>(s.a)];
        const Node& b = g.nodes[static_cast<size_t>(s.b)];
        const Edge& e = g.edges[static_cast<size_t>(s.edge)];
        Route rt;
        rt.edge = s.edge;
        const bool first = s.a == e.chain.front(), last = s.b == e.chain.back();
        const Head at_a = e.flipped ? e.head_to : e.head_from, at_b = e.flipped ? e.head_from : e.head_to;
        if (first) rt.head_start = at_a;
        if (last) rt.head_end = at_b;
        const int r = a.rank;
        const int g0 = start[static_cast<size_t>(r)] + thick[static_cast<size_t>(r)];  // the gap's first row (or column)
        if (vertical) {
            const int xa = a.x + s.pa, xb = b.x + s.pb;
            const int ya = a.y + a.h, yb = b.y - 1;
            const int mid = g0 + 1;
            rt.pts = {{xa, a.dummy ? a.y : ya}, {xa, mid}, {xb, mid}, {xb, b.dummy ? b.y + b.h - 1 : yb}};
            if (first && !e.label.empty()) {
                // On the run across, if it is long enough; else beside the way down, a row lower.
                const int lw = cols_of(e.label) + 2;
                if (std::abs(xb - xa) >= lw + 2) labels.push_back({std::min(xa, xb) + (std::abs(xb - xa) - lw) / 2 + 1, mid, " " + e.label + " "});
                else labels.push_back({xb + 2, mid + 1, e.label, xb});
            }
        } else {
            const int ya = a.y + s.pa, yb = b.y + s.pb;
            const int xa = a.x + a.w, xb = b.x - 1;
            const int vr = g0 + gap[static_cast<size_t>(r)] - 2;
            rt.pts = {{a.dummy ? a.x : xa, ya}, {vr, ya}, {vr, yb}, {b.dummy ? b.x + b.w - 1 : xb, yb}};
            if (first && !e.label.empty()) labels.push_back({xa + 1, ya, " " + e.label + " "});
        }
        routes.push_back(std::move(rt));
    }
    // The size, and the turn for BT and RL.
    int W = 1, H = 1;
    for (const Node& n : g.nodes) W = std::max(W, n.x + n.w + 2), H = std::max(H, n.y + n.h + 1);
    for (const Text& t : labels) W = std::max(W, t.x + cols_of(t.s) + 1);
    const bool turn = g.dir == 'U' || g.dir == 'L';
    auto flip = [&](int& x, int& y, int w_, int h_) {
        if (!turn) return;
        if (vertical) y = H - y - h_;
        else x = W - x - w_;
    };
    Canvas cv;
    cv.fit(W - 1, H - 1);
    // Arrows first (their lines), then the boxes over them, then the heads and what arrows say.
    for (Route& rt : routes) {
        for (auto& [x, y] : rt.pts) flip(x, y, 1, 1);
        const Edge& e = g.edges[static_cast<size_t>(rt.edge)];
        if (e.hidden) continue;
        for (size_t k = 0; k + 1 < rt.pts.size(); ++k) cv.line(rt.pts[k].first, rt.pts[k].second, rt.pts[k + 1].first, rt.pts[k + 1].second, e.kind, kMdEdge);
        // (each end reaching to the node it touches)
        const auto [x0, y0] = rt.pts.front();
        const auto [x1, y1] = rt.pts.back();
        const bool down_ = vertical ? !turn : false, right_ = vertical ? false : !turn;
        if (vertical) {
            cv.bits(x0, y0, down_ ? kUp : kDown, e.kind, kMdEdge);
            cv.bits(x1, y1, down_ ? kDown : kUp, e.kind, kMdEdge);
        } else {
            cv.bits(x0, y0, right_ ? kLeft : kRight, e.kind, kMdEdge);
            cv.bits(x1, y1, right_ ? kRight : kLeft, e.kind, kMdEdge);
        }
    }
    for (Node& n : g.nodes) {
        flip(n.x, n.y, n.w, n.h);
        if (n.dummy) continue;
        static const char* const kSquare[] = {"\xE2\x94\x8C", "\xE2\x94\x90", "\xE2\x94\x94", "\xE2\x94\x98"};
        static const char* const kRounded[] = {"\xE2\x95\xAD", "\xE2\x95\xAE", "\xE2\x95\xB0", "\xE2\x95\xAF"};
        static const char* const kSlant[] = {"\xE2\x95\xB1", "\xE2\x95\xB2", "\xE2\x95\xB2", "\xE2\x95\xB1"};
        static const char* const kDoubled[] = {"\xE2\x95\x94", "\xE2\x95\x97", "\xE2\x95\x9A", "\xE2\x95\x9D"};
        if (n.shape == kDot || n.shape == kEndDot) {
            cv.text(n.x, n.y, n.lines[0], kMdAccent);
            continue;
        }
        const char* const* corners = n.shape == kRound ? kRounded : n.shape == kDiamond ? kSlant : n.shape == kDouble ? kDoubled : kSquare;
        const char* hz = n.shape == kDouble ? "\xE2\x95\x90" : "\xE2\x94\x80";
        const char* vt = n.shape == kDouble ? "\xE2\x95\x91" : "\xE2\x94\x82";
        cv.box(n.x, n.y, n.w, n.h, corners, hz, vt, kMdBorder);
        for (size_t k = 0; k < n.lines.size(); ++k) {
            const int lw = cols_of(n.lines[k]);
            cv.text(n.x + (n.w - lw) / 2, n.y + 1 + static_cast<int>(k), n.lines[k], kMdText);
        }
        if (n.loop) cv.text(n.x + n.w, n.y + n.h / 2, "\xE2\x86\xBA", kMdEdge);
    }
    // (a place for one is free when nothing is drawn in any of its cells, or the cell either side)
    auto free = [&](int x, int y, int n) {
        if (x < 0 || y < 0) return false;
        for (int c = x - 1; c <= x + n; ++c) {
            if (c < 0 || c >= cv.w || y >= cv.h) continue;
            const Cell& cell = cv.cells[static_cast<size_t>(y) * static_cast<size_t>(cv.w) + static_cast<size_t>(c)];
            if ((!cell.text.empty() && cell.text != " ") || cell.lines || cell.cont) return false;
        }
        return true;
    };
    for (Text& t : labels) {
        int x = t.x, y = t.y;
        if (turn) {
            if (vertical) y = H - y - 1;
            else x = W - x - cols_of(t.s);
        }
        const int n = cols_of(t.s);
        if (t.line_x >= 0) {  // beside a line: its right, its left, then a row either way
            const int lx = t.line_x;
            const std::pair<int, int> tries[] = {{lx + 2, y}, {lx - 1 - n, y}, {lx + 2, y - 1}, {lx - 1 - n, y - 1}, {lx + 2, y + 1}, {lx - 1 - n, y + 1}};
            for (const auto& [tx, ty] : tries)
                if (free(tx, ty, n)) {
                    x = tx, y = ty;
                    break;
                }
        }
        cv.text(x, y, t.s, kMdLabel, kMdItalic);
    }
    // Where an arrow leaves a box, its side shows it: ┬ ┴ ├ ┤.
    for (const Route& rt : routes) {
        const Edge& e = g.edges[static_cast<size_t>(rt.edge)];
        if (e.hidden) continue;
        auto tee = [&](std::pair<int, int> p, bool start_end) {
            const auto [x, y] = p;
            int bx = x, by = y;
            const char* t = nullptr;
            const bool forward = !turn;
            if (vertical) {
                const bool below = start_end ? forward : !forward;  // (the box is above this end)
                by = below ? y - 1 : y + 1;
                t = below ? "\xE2\x94\xAC" : "\xE2\x94\xB4";
            } else {
                const bool right = start_end ? forward : !forward;
                bx = right ? x - 1 : x + 1;
                t = right ? "\xE2\x94\x9C" : "\xE2\x94\xA4";
            }
            if (bx < 0 || by < 0 || bx >= cv.w || by >= cv.h) return;
            Cell& c = cv.at(bx, by);
            if (c.text == "\xE2\x94\x80" || c.text == "\xE2\x94\x82") c.text = t;
        };
        const bool head_s = rt.head_start != kNoHead, head_e = rt.head_end != kNoHead;
        if (!head_s) tee(rt.pts.front(), true);
        if (!head_e) tee(rt.pts.back(), false);
        // Heads: pointing into the box at that end.
        auto head = [&](std::pair<int, int> p, std::pair<int, int> from, Head h) {
            if (h == kNoHead) return;
            const auto [x, y] = p;
            const int dx = x - from.first, dy = y - from.second;
            const char* t = h == kCircle ? "\xE2\x97\x8F" : h == kCross ? "\xE2\x9C\x95"
                          : dy > 0 ? "\xE2\x96\xBC" : dy < 0 ? "\xE2\x96\xB2" : dx > 0 ? "\xE2\x96\xB6" : "\xE2\x97\x80";
            cv.text(x, y, t, kMdEdge);
        };
        // (the direction into the box: from the point before the end)
        auto into = [&](bool at_end) {
            const auto& pts = rt.pts;
            const std::pair<int, int> p = at_end ? pts.back() : pts.front();
            std::pair<int, int> q = p;
            if (vertical) q.second += (at_end ? (turn ? 1 : -1) : (turn ? -1 : 1));
            else q.first += (at_end ? (turn ? 1 : -1) : (turn ? -1 : 1));
            return std::pair{p, q};
        };
        if (head_e) {
            const auto [p, q] = into(true);
            head(p, q, rt.head_end);
        }
        if (head_s) {
            const auto [p, q] = into(false);
            head(p, q, rt.head_start);
        }
    }
    return cv;
}

// ---------------------------------------------------------------- sequence diagrams

bool draw_sequence(const std::vector<std::string>& st, Canvas* out, std::string* err) {
    struct Part {
        std::string id, name;
        int w = 0, x = 0;
    };
    std::vector<Part> parts;
    auto part = [&](std::string_view id) {
        id = trim(id);
        for (size_t k = 0; k < parts.size(); ++k)
            if (parts[k].id == id) return static_cast<int>(k);
        parts.push_back({std::string(id), std::string(id)});
        return static_cast<int>(parts.size()) - 1;
    };
    struct Event {
        enum Kind { kMessage, kNote, kOpen, kElse, kEnd } kind = kMessage;
        int a = 0, b = 0;
        std::string text;
        u8 line = kLight;
        Head head = kArrow;
        int side = 0;  // a note: -1 left of, 1 right of, 0 over
    };
    std::vector<Event> events;
    bool numbered = false;
    int number = 0;
    for (size_t k = 1; k < st.size(); ++k) {
        const std::string_view s = st[k];
        const std::string l = lower(s);
        if (l.starts_with("participant ") || l.starts_with("actor ")) {
            std::string_view rest = trim(s.substr(l.starts_with("actor ") ? 6 : 12));
            std::string name;
            const size_t as = lower(rest).find(" as ");
            if (as != std::string::npos) {
                name = std::string(trim(rest.substr(as + 4)));
                rest = trim(rest.substr(0, as));
            }
            const int p = part(rest);
            if (!name.empty()) parts[static_cast<size_t>(p)].name = name;
            continue;
        }
        if (l == "autonumber" || l.starts_with("autonumber ")) {
            numbered = true;
            continue;
        }
        if (l.starts_with("activate ") || l.starts_with("deactivate ") || l.starts_with("create ") || l.starts_with("destroy ") || l.starts_with("box") ||
            l.starts_with("links ") || l.starts_with("link ") || l.starts_with("title"))
            continue;
        if (l.starts_with("note ")) {
            const size_t colon = s.find(':');
            if (colon == std::string_view::npos) continue;
            std::string_view where = trim(s.substr(5, colon - 5));
            Event e;
            e.kind = Event::kNote;
            e.text = std::string(trim(s.substr(colon + 1)));
            const std::string wl = lower(where);
            std::string_view who;
            if (wl.starts_with("left of ")) e.side = -1, who = where.substr(8);
            else if (wl.starts_with("right of ")) e.side = 1, who = where.substr(9);
            else if (wl.starts_with("over ")) e.side = 0, who = where.substr(5);
            else continue;
            const size_t comma = who.find(',');
            e.a = part(who.substr(0, comma));
            e.b = comma == std::string_view::npos ? e.a : part(who.substr(comma + 1));
            events.push_back(e);
            continue;
        }
        static const char* const kOpens[] = {"loop", "alt", "opt", "par", "critical", "break", "rect"};
        bool opened = false;
        for (const char* o : kOpens)
            if (l == o || l.starts_with(std::string(o) + " ")) {
                Event e;
                e.kind = Event::kOpen;
                e.text = std::string(trim(s));
                events.push_back(e);
                opened = true;
                break;
            }
        if (opened) continue;
        if (l == "else" || l.starts_with("else ") || l == "and" || l.starts_with("and ") || l.starts_with("option ")) {
            Event e;
            e.kind = Event::kElse;
            e.text = std::string(trim(s));
            events.push_back(e);
            continue;
        }
        if (l == "end") {
            Event e;
            e.kind = Event::kEnd;
            events.push_back(e);
            continue;
        }
        // A message: A->>B: text (and -->>, ->, -->, -x, --x, -), --)).
        static const char* const kArrows[] = {"-->>", "->>", "-->", "->", "--x", "-x", "--)", "-)"};
        size_t at = std::string_view::npos;
        const char* which = nullptr;
        for (const char* ar : kArrows) {
            const size_t p = s.find(ar);
            if (p != std::string_view::npos && (at == std::string_view::npos || p < at || (p == at && std::string_view(ar).size() > std::string_view(which).size()))) at = p, which = ar;
        }
        if (at == std::string_view::npos) {
            *err = "line " + std::to_string(k + 1) + ": not a message, a note or a block: \"" + std::string(s.substr(0, 30)) + "\"";
            return false;
        }
        const std::string_view arrow = which;
        std::string_view rest = s.substr(at + arrow.size());
        const size_t colon = rest.find(':');
        Event e;
        e.kind = Event::kMessage;
        e.a = part(s.substr(0, at));
        std::string_view to = colon == std::string_view::npos ? rest : rest.substr(0, colon);
        to = trim(to);
        if (to.starts_with('+') || to.starts_with('-')) to.remove_prefix(1);
        e.b = part(to);
        e.text = colon == std::string_view::npos ? std::string() : std::string(trim(rest.substr(colon + 1)));
        if (numbered) e.text = std::to_string(++number) + ". " + e.text;
        e.line = arrow.starts_with("--") ? kDotted : kLight;
        e.head = arrow.ends_with('x') ? kCross : (arrow == "->" || arrow == "-->") ? kNoHead : kArrow;
        events.push_back(e);
    }
    if (parts.empty()) {
        *err = "nobody in it";
        return false;
    }
    // Columns: each participant's box, and room between for what passes between them.
    for (Part& p : parts) p.w = cols_of(p.name) + 4;
    const size_t np = parts.size();
    std::vector<int> need(np, 0);  // the distance from each centre to the next
    for (size_t k = 0; k + 1 < np; ++k) need[k] = (parts[k].w + 1) / 2 + (parts[k + 1].w + 1) / 2 + 2;
    for (const Event& e : events) {
        if (e.kind == Event::kMessage) {
            const int a = std::min(e.a, e.b), b = std::max(e.a, e.b);
            const int want = cols_of(e.text) + 4;
            if (a == b) {
                if (static_cast<size_t>(a) + 1 < np) need[static_cast<size_t>(a)] = std::max(need[static_cast<size_t>(a)], want + 2);
                continue;
            }
            int have = 0;
            for (int k = a; k < b; ++k) have += need[static_cast<size_t>(k)];
            if (have < want) need[static_cast<size_t>(b - 1)] += want - have;
        } else if (e.kind == Event::kNote && e.side == 1 && static_cast<size_t>(e.a) + 1 < np) {
            need[static_cast<size_t>(e.a)] = std::max(need[static_cast<size_t>(e.a)], cols_of(e.text) + 6);
        }
    }
    int x = (parts[0].w + 1) / 2 + 2;
    for (size_t k = 0; k < np; ++k) {
        parts[k].x = x;
        if (k + 1 < np) x += need[k];
    }
    Canvas cv;
    auto boxes = [&](int y) {
        for (const Part& p : parts) {
            static const char* const kRounded[] = {"\xE2\x95\xAD", "\xE2\x95\xAE", "\xE2\x95\xB0", "\xE2\x95\xAF"};
            const int bx = p.x - p.w / 2;
            cv.box(bx, y, p.w, 3, kRounded, "\xE2\x94\x80", "\xE2\x94\x82", kMdBorder);
            cv.text(bx + 2, y + 1, p.name, kMdText, kMdBold);
        }
    };
    boxes(0);
    int y = 3;
    const int right = parts.back().x + parts.back().w / 2 + 2;
    struct Frame {
        int y;
        int depth;
    };
    std::vector<Frame> frames;
    for (const Event& e : events) {
        switch (e.kind) {
            case Event::kMessage: {
                const int xa = parts[static_cast<size_t>(e.a)].x, xb = parts[static_cast<size_t>(e.b)].x;
                if (e.a == e.b) {  // to itself: out and back
                    if (!e.text.empty()) cv.text(xa + 2, y, e.text, kMdLabel, kMdItalic), ++y;
                    cv.line(xa, y, xa + 3, y, e.line, kMdEdge);
                    cv.line(xa + 3, y, xa + 3, y + 1, e.line, kMdEdge);
                    cv.line(xa + 1, y + 1, xa + 3, y + 1, e.line, kMdEdge);
                    if (e.head != kNoHead) cv.text(xa + 1, y + 1, e.head == kCross ? "\xE2\x9C\x95" : "\xE2\x97\x80", kMdEdge);
                    y += 3;
                    break;
                }
                if (!e.text.empty()) {
                    const int lw = cols_of(e.text);
                    cv.text(std::min(xa, xb) + (std::abs(xb - xa) - lw) / 2 + 1, y, e.text, kMdLabel, kMdItalic);
                    ++y;
                }
                const int dir = xb > xa ? 1 : -1;
                cv.line(xa, y, xb - dir, y, e.line, kMdEdge);
                cv.bits(xa, y, dir > 0 ? kRight : kLeft, e.line, kMdEdge);
                if (e.head != kNoHead) cv.text(xb - dir, y, e.head == kCross ? "\xE2\x9C\x95" : dir > 0 ? "\xE2\x96\xB6" : "\xE2\x97\x80", kMdEdge);
                y += 2;
                break;
            }
            case Event::kNote: {
                const int lw = cols_of(e.text);
                int nx;
                const int pa = parts[static_cast<size_t>(std::min(e.a, e.b))].x, pb = parts[static_cast<size_t>(std::max(e.a, e.b))].x;
                int nw = lw + 4;
                if (e.side == 1) nx = pa + 2;
                else if (e.side == -1) nx = std::max(0, pa - 2 - nw);
                else {
                    nw = std::max(nw, pb - pa + 6);
                    nx = std::max(0, (pa + pb) / 2 - nw / 2);
                }
                static const char* const kSquare[] = {"\xE2\x94\x8C", "\xE2\x94\x90", "\xE2\x94\x94", "\xE2\x94\x98"};
                cv.box(nx, y, nw, 3, kSquare, "\xE2\x94\x80", "\xE2\x94\x82", kMdMuted);
                cv.text(nx + (nw - lw) / 2, y + 1, e.text, kMdQuote, kMdItalic);
                y += 4;
                break;
            }
            case Event::kOpen:
            case Event::kElse: {
                // (Each frame inside another a column in from its sides, while there is room.)
                const int depth = std::min(static_cast<int>(frames.size()) - (e.kind == Event::kElse ? 1 : 0), right / 2 - 1);
                const int x0 = std::max(0, depth), x1 = right - depth;
                if (e.kind == Event::kOpen) frames.push_back({y, depth});
                cv.line(x0, y, x1, y, kDotted, kMdMuted);
                if (e.kind == Event::kOpen) {
                    cv.text(x0, y, "\xE2\x95\xAD", kMdMuted);
                    cv.text(x1, y, "\xE2\x95\xAE", kMdMuted);
                }
                cv.text(x0 + 2, y, " " + e.text + " ", kMdAccent, kMdBold);
                y += 2;
                break;
            }
            case Event::kEnd: {
                if (frames.empty()) break;
                const Frame f = frames.back();
                frames.pop_back();
                const int x0 = std::max(0, f.depth), x1 = right - f.depth;
                for (int r = f.y + 1; r < y; ++r) {
                    if (cv.at(x0, r).text.empty() && !cv.at(x0, r).lines) cv.text(x0, r, "\xE2\x94\x86", kMdMuted);
                    if (cv.at(x1, r).text.empty() && !cv.at(x1, r).lines) cv.text(x1, r, "\xE2\x94\x86", kMdMuted);
                }
                cv.line(x0, y, x1, y, kDotted, kMdMuted);
                cv.text(x0, y, "\xE2\x95\xB0", kMdMuted);
                cv.text(x1, y, "\xE2\x95\xAF", kMdMuted);
                y += 2;
                break;
            }
        }
    }
    // The lifelines, under what crosses them.
    for (const Part& p : parts)
        for (int r = 3; r < y; ++r) cv.bits(p.x, r, kUp | kDown, kDotted, kMdMuted);
    boxes(y);
    *out = std::move(cv);
    return true;
}

// ---------------------------------------------------------------- pie charts, as bars

bool draw_pie(const std::vector<std::string>& st, u32 width, std::vector<MdRow>* rows, std::string* err) {
    std::string title;
    std::vector<std::pair<std::string, double>> slices;
    bool show = lower(st[0]).find("showdata") != std::string::npos;
    if (const size_t t = lower(st[0]).find(" title "); t != std::string::npos) title = std::string(trim(std::string_view(st[0]).substr(t + 7)));  // (pie title Pets: on the header's line)
    for (size_t k = 1; k < st.size(); ++k) {
        const std::string_view s = st[k];
        const std::string l = lower(s);
        if (l.starts_with("title ")) {
            title = std::string(trim(s.substr(6)));
            continue;
        }
        if (l == "showdata") {
            show = true;
            continue;
        }
        const size_t colon = s.rfind(':');
        if (colon == std::string_view::npos) continue;
        std::string name(trim(s.substr(0, colon)));
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
        const std::string num(trim(s.substr(colon + 1)));
        char* end = nullptr;
        const double v = std::strtod(num.c_str(), &end);
        if (!end || *end || !(v >= 0 && v < 1e300)) {  // (nan and inf too)
            *err = "line " + std::to_string(k + 1) + ": a slice is \"name\" : number";
            return false;
        }
        slices.push_back({name, v});
    }
    double total = 0;
    int name_w = 1;
    for (const auto& [n, v] : slices) total += v, name_w = std::max(name_w, cols_of(n));
    if (slices.empty() || total <= 0) {
        *err = "no slices";
        return false;
    }
    if (!title.empty()) {
        rows->emplace_back();
        rows->back().runs.push_back({title, kMdText, kMdBold});
    }
    const int bar_room = std::max(8, static_cast<int>(width) - name_w - 16);
    static const u8 kInks[] = {kMdSlice0, kMdSlice1, kMdSlice2, kMdSlice3, kMdHeading1, kMdHeading2, kMdHeading3, kMdAccent};
    for (size_t k = 0; k < slices.size(); ++k) {
        const auto& [n, v] = slices[k];
        const double share = v / total;
        const int cells = std::max(1, static_cast<int>(std::lround(share * bar_room)));
        std::string bar;
        for (int c = 0; c < cells; ++c) bar += "\xE2\x96\x88";
        char pct[48];
        if (show) std::snprintf(pct, sizeof pct, " %.1f%% (%g)", share * 100, v);
        else std::snprintf(pct, sizeof pct, " %.1f%%", share * 100);
        MdRow r;
        r.runs.push_back({n + std::string(static_cast<size_t>(name_w - cols_of(n) + 2), ' '), kMdText});
        r.runs.push_back({bar, kInks[k % std::size(kInks)]});
        r.runs.push_back({pct, kMdMuted});
        rows->push_back(std::move(r));
    }
    return true;
}

}  // namespace

bool mermaid_draw(std::string_view source, u32 width, std::vector<MdRow>* rows, std::string* note) {
    rows->clear();
    note->clear();
    // (front matter and directives: --- ... --- and %%{ }%% at the top)
    auto without_front = [](std::vector<std::string> st) {
        if (!st.empty() && st[0] == "---") {
            size_t k = 1;
            while (k < st.size() && st[k] != "---") ++k;
            st.erase(st.begin(), st.begin() + static_cast<std::ptrdiff_t>(std::min(k + 1, st.size())));
        }
        return st;
    };
    std::vector<std::string> st = without_front(statements(source, false));
    if (st.empty()) {
        *note = "empty";
        return false;
    }
    const std::string kind = lower(st[0].substr(0, st[0].find(' ')));
    std::string err;
    Canvas cv;
    if (kind == "graph" || kind == "flowchart" || kind == "flowchart-elk") {
        Graph g;
        const std::vector<std::string> parts = without_front(statements(source, true));  // (split at ;s too: the same statements, from the same header)
        if (!parse_flowchart(parts, g, &err)) {
            *note = err;
            return false;
        }
        cv = draw_graph(g);
        if (cv.w > static_cast<int>(width) && (g.dir == 'R' || g.dir == 'L')) {  // too wide across: down instead
            g.dir = 'D';
            cv = draw_graph(g);
            *note = "(drawn top to bottom: too wide across)";
        }
    } else if (kind == "statediagram" || kind == "statediagram-v2") {
        Graph g;
        if (!parse_state(st, g, &err)) {
            *note = err;
            return false;
        }
        cv = draw_graph(g);
        if (cv.w > static_cast<int>(width) && (g.dir == 'R' || g.dir == 'L')) {
            g.dir = 'D';
            cv = draw_graph(g);
            *note = "(drawn top to bottom: too wide across)";
        }
    } else if (kind == "sequencediagram") {
        if (!draw_sequence(st, &cv, &err)) {
            *note = err;
            return false;
        }
    } else if (kind == "pie") {
        if (!draw_pie(st, width, rows, &err)) {
            *note = err;
            return false;
        }
        return true;
    } else {
        *note = st[0].substr(0, st[0].find(' ')) + " is not drawn yet: shown as text";
        return false;
    }
    int cut = 0;
    *rows = cv.rows(static_cast<int>(width), &cut);
    while (!rows->empty() && md_row_text(rows->back()).empty()) rows->pop_back();
    if (cut > 0) *note = (note->empty() ? "" : *note + " ") + "(cut at the right: " + std::to_string(cut) + " columns more)";
    return true;
}

}  // namespace blit
