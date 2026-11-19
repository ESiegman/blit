#include "mermaid.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "diagram.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

// ---------------------------------------------------------------- text

std::string_view trim(std::string_view s) { return diagram_trim(s); }
std::string lower(std::string_view s) { return diagram_lower(s); }
std::vector<std::string> statements(std::string_view src, bool split_semicolons) { return diagram_statements(src, split_semicolons); }

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

// ---------------------------------------------------------------- sequence diagrams

enum Head { kNoHead, kArrow, kCross };

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
    std::vector<std::string> st = statements(source, false);  // (without front matter and directives)
    if (st.empty()) {
        *note = "empty";
        return false;
    }
    const std::string kind = lower(st[0].substr(0, st[0].find(' ')));
    std::string err;
    Canvas cv;
    if (kind == "graph" || kind == "flowchart" || kind == "flowchart-elk" || kind == "statediagram" || kind == "statediagram-v2") {
        return false;  // (drawn as pictures: diagram.hpp)
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
