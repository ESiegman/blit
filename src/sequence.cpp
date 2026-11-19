// Sequence diagrams (sequenceDiagram), laid out as Mermaid lays them out:
// the participants in a row across the top (and again at the bottom), a
// lifeline down from each, and what happens down the page in order, each
// thing in a band of its own: messages (an arrow from one lifeline to
// another, what it says over it; to itself, a loop), notes, frames round
// parts of it (loop, alt with its else parts, opt, par, critical, break,
// rect), and activations as bars on the lifelines. The gaps between
// participants are made as wide as what passes between them needs.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "diagram.hpp"

namespace blit {

namespace {

enum PartKind : u8 { kParticipant, kActor, kDatabase, kCollections };

struct Part {
    std::string id, name;
    std::vector<std::string> lines;  // its name as shown
    u8 kind = kParticipant;
    int group = -1;
    int created = -1, destroyed = -1;  // the messages that make it and end it
    float w = 0, x = 0;                // its box's width; its lifeline (across)
    float head_y = 0, end_y = -1;      // its box's top; where its lifeline ends (-1: at the foot)
    float reach = 0;                   // how far its activations go either side of its lifeline
};

// A box round participants (box ... end), in a colour, with a label.
struct Group {
    std::vector<std::string> label;
    DiagramColour fill;
    float x0 = 0, x1 = 0;
};

enum EventKind : u8 { kMessage, kNote, kOpen, kElse, kEnd, kActivate, kDeactivate };

struct Event {
    u8 kind = kMessage;
    int a = -1, b = -1;              // a message's ends; a note's participants (a to b); who is activated
    std::vector<std::string> said;   // what it says
    std::vector<std::string> lines;  // as shown (wrapped), and its size
    float tw = 0, th = 0;
    bool dotted = false, both = false;
    u8 head = kHeadArrow;
    bool activate = false, deactivate = false;  // + (its target) and - (its source)
    int side = 0;                               // a note: -1 left of, 1 right of, 0 over
    std::string word;                           // a frame's kind (loop, alt, ...; and, else ...)
    DiagramColour fill;                         // a rect's
    bool numbered = false;                      // autonumber's: its number shown
    long long number = 0;                       // (64 bits: a big start, a big step and many messages go past an int)
    int frame = -1;                             // the frame it opens, divides or ends
    // Where it was put: its line (a message's), or its top; a note's box.
    float y = 0, text_y = 0, x0 = 0, x1 = 0, h = 0;
};

struct Frame {
    int open = -1, end = -1, parent = -1, depth = 0;
    std::vector<int> elses;
    float x0 = 0, x1 = 0, top = 0, bottom = 0;
    float tab_w = 0, tab_h = 0;
};

struct Bar {
    int part = 0, level = 0;
    float y0 = 0, y1 = -1;
};

// How tight it is laid out: the usual way, or narrower to fit the room.
struct Tight {
    float wrap = 1, min_w = 1, margin = 1;
};

std::string_view after_word(std::string_view s, size_t n) { return diagram_trim(s.substr(std::min(n, s.size()))); }

class Sequence {
public:
    explicit Sequence(const DiagramStyle& s) : style_(s), font_(*s.font), p_(s.palette) {
        u_ = s.unit();
        size_ = s.size;
        line_ = font_.line(size_);
    }

    bool parse(const std::vector<std::string>& st, std::string* err);
    void lay_out(const Tight& t, Drawing* d, DiagramGeometry* geo);

private:
    int part(std::string_view id);
    std::vector<std::string> wrapped(std::string_view text, float most) const {
        return diagram_wrap(diagram_label(text), most, size_, font_);
    }
    float widest(const std::vector<std::string>& lines, bool bold = false) const {
        float w = 0;
        for (const std::string& l : lines) w = std::max(w, font_.width(l, size_, bold));
        return w;
    }
    // A participant's lifeline's edge (or its top activation's) at y, on side s (-1 left, 1 right).
    float edge_at(int part, float y, int s) const;
    int level_at(int part, float y) const;
    void text_block(Drawing* d, const std::vector<std::string>& lines, float x, float top, DiagramColour c, int align, bool bold = false, float size = 0) const;

    const DiagramStyle& style_;
    const DiagramFont& font_;
    const DiagramPalette& p_;
    float u_ = 1, size_ = 13, line_ = 16;
    std::vector<Part> parts_;
    std::vector<Group> groups_;
    std::vector<Event> events_;
    std::vector<Frame> frames_;
    std::vector<Bar> bars_;
    std::string title_;
};

int Sequence::part(std::string_view id) {
    id = diagram_trim(id);
    for (size_t k = 0; k < parts_.size(); ++k)
        if (parts_[k].id == id) return static_cast<int>(k);
    Part p;
    p.id = p.name = std::string(id);
    parts_.push_back(std::move(p));
    return static_cast<int>(parts_.size()) - 1;
}

bool Sequence::parse(const std::vector<std::string>& st, std::string* err) {
    std::vector<char> open;  // what each end ends: 'b' a box, 'f' a frame
    std::vector<int> frame_stack;
    int group = -1;
    bool numbered = false;
    long long number = 1, step = 1;
    std::string create, destroy;  // (a create or destroy said, for the next message)
    auto fail = [&](size_t k, const std::string& why) {
        *err = "line " + std::to_string(k + 1) + ": " + why;
        return false;
    };
    for (size_t k = 1; k < st.size(); ++k) {
        const std::string_view s = st[k];
        const std::string l = diagram_lower(s);
        auto is = [&](const char* w) {
            const size_t n = std::strlen(w);
            return l.size() >= n && l.compare(0, n, w) == 0 && (l.size() == n || l[n] == ' ' || l[n] == '\t' || l[n] == ':');
        };
        if (is("create")) {
            std::string_view rest = after_word(s, 6);
            const std::string rl = diagram_lower(rest);
            if (!rl.starts_with("participant ") && !rl.starts_with("actor ")) return fail(k, "create what? (create participant X, or create actor X)");
            // (declared below, as any other participant; made by the next message)
            const size_t sp = rest.find(' ');
            std::string_view id = after_word(rest, sp);
            if (const size_t as = diagram_lower(id).find(" as "); as != std::string::npos) id = diagram_trim(id.substr(0, as));
            if (const size_t at = id.find("@{"); at != std::string::npos) id = diagram_trim(id.substr(0, at));
            create = std::string(id);
        }
        if (is("participant") || is("actor") || is("create")) {
            std::string_view rest = s;
            if (is("create")) rest = after_word(rest, 6);
            const bool actor = diagram_lower(rest).starts_with("actor");
            rest = after_word(rest, actor ? 5 : 11);
            std::string name;
            u8 kind = actor ? kActor : kParticipant;
            if (const size_t at = rest.find("@{"); at != std::string::npos) {  // A@{ "type": "database", "alias": "..." }
                const size_t close = rest.find('}', at);
                const std::string meta(rest.substr(at + 2, close == std::string_view::npos ? std::string_view::npos : close - at - 2));
                auto value = [&](const char* key) {
                    const size_t kp = meta.find(key);
                    if (kp == std::string::npos) return std::string();
                    size_t q = meta.find(':', kp);
                    if (q == std::string::npos) return std::string();
                    std::string_view v = diagram_trim(std::string_view(meta).substr(q + 1));
                    if (const size_t comma = v.find(','); comma != std::string_view::npos) v = diagram_trim(v.substr(0, comma));
                    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.find(v.front(), 1) - 1);
                    return std::string(v);
                };
                const std::string type = diagram_lower(value("type"));
                if (type == "actor") kind = kActor;
                else if (type == "database") kind = kDatabase;
                else if (type == "collections") kind = kCollections;
                name = value("alias");
                std::string tail = close == std::string_view::npos ? std::string() : std::string(rest.substr(close + 1));
                rest = diagram_trim(rest.substr(0, at));
                if (const size_t as = diagram_lower(tail).find("as "); as != std::string::npos) name = std::string(diagram_trim(std::string_view(tail).substr(as + 3)));
            }
            if (const size_t as = diagram_lower(rest).find(" as "); as != std::string::npos) {
                name = std::string(diagram_trim(rest.substr(as + 4)));
                rest = diagram_trim(rest.substr(0, as));
            }
            if (rest.empty()) return fail(k, "a participant needs a name");
            const int p = part(rest);
            Part& pt = parts_[static_cast<size_t>(p)];
            if (!name.empty()) pt.name = name;
            pt.kind = kind;
            if (group >= 0 && pt.group < 0) pt.group = group;
            continue;
        }
        if (is("destroy")) {
            destroy = std::string(after_word(s, 7));
            if (destroy.empty()) return fail(k, "destroy whom?");
            part(destroy);
            continue;
        }
        if (is("box")) {
            std::string_view rest = after_word(s, 3);
            Group g;
            // A colour first (a name, #hex, rgb(...)), if there is one; then the label.
            size_t end = rest.find(' ');
            if (diagram_lower(rest).starts_with("rgb") || diagram_lower(rest).starts_with("hsl")) end = rest.find(')') == std::string_view::npos ? end : rest.find(')') + 1;
            DiagramColour c;
            if (!rest.empty() && diagram_colour(rest.substr(0, end), &c)) {
                g.fill = c;
                rest = end == std::string_view::npos ? std::string_view() : after_word(rest, end);
            }
            g.label = diagram_label(rest);
            if (g.label.size() == 1 && g.label[0].empty()) g.label.clear();
            if (group >= 0) return fail(k, "a box inside a box");
            group = static_cast<int>(groups_.size());
            groups_.push_back(std::move(g));
            open.push_back('b');
            continue;
        }
        if (l == "end") {
            if (open.empty()) return fail(k, "an end with nothing to end");
            const char what = open.back();
            open.pop_back();
            if (what == 'b') {
                group = -1;
                continue;
            }
            Event e;
            e.kind = kEnd;
            e.frame = frame_stack.back();
            frame_stack.pop_back();
            frames_[static_cast<size_t>(e.frame)].end = static_cast<int>(events_.size());
            events_.push_back(std::move(e));
            continue;
        }
        static const char* const kFrames[] = {"loop", "alt", "opt", "par", "critical", "break", "rect"};
        bool framed = false;
        for (const char* w : kFrames) {
            if (!is(w)) continue;
            Event e;
            e.kind = kOpen;
            e.word = w;
            std::string_view rest = after_word(s, std::strlen(w));
            if (e.word == "rect") {
                if (!diagram_colour(rest, &e.fill)) e.fill = {};
            } else {
                e.said = diagram_label(rest);
                if (e.said.size() == 1 && e.said[0].empty()) e.said.clear();
            }
            Frame f;
            f.open = static_cast<int>(events_.size());
            f.parent = frame_stack.empty() ? -1 : frame_stack.back();
            f.depth = static_cast<int>(frame_stack.size());
            e.frame = static_cast<int>(frames_.size());
            frames_.push_back(f);
            frame_stack.push_back(e.frame);
            open.push_back('f');
            events_.push_back(std::move(e));
            framed = true;
            break;
        }
        if (framed) continue;
        if (is("else") || is("and") || is("option")) {
            if (frame_stack.empty() || open.back() != 'f') return fail(k, std::string(s.substr(0, s.find(' '))) + " outside a block");
            Event e;
            e.kind = kElse;
            e.word = diagram_lower(s.substr(0, s.find(' ')));
            e.said = diagram_label(after_word(s, e.word.size()));
            if (e.said.size() == 1 && e.said[0].empty()) e.said.clear();
            e.frame = frame_stack.back();
            frames_[static_cast<size_t>(e.frame)].elses.push_back(static_cast<int>(events_.size()));
            events_.push_back(std::move(e));
            continue;
        }
        if (is("note")) {
            const size_t colon = s.find(':');
            if (colon == std::string_view::npos) return fail(k, "a note is: Note right of A: text");
            const std::string_view where = diagram_trim(s.substr(4, colon - 4));
            const std::string wl = diagram_lower(where);
            Event e;
            e.kind = kNote;
            std::string_view who;
            if (wl.starts_with("left of ")) e.side = -1, who = where.substr(8);
            else if (wl.starts_with("right of ")) e.side = 1, who = where.substr(9);
            else if (wl.starts_with("over ")) who = where.substr(5);
            else return fail(k, "a note goes left of, right of or over someone");
            const size_t comma = who.find(',');
            if (diagram_trim(who.substr(0, comma)).empty()) return fail(k, "a note over whom?");
            if (comma != std::string_view::npos && diagram_trim(who.substr(comma + 1)).empty()) return fail(k, "a note over whom?");  // (A, and no one after it)
            e.a = part(who.substr(0, comma));
            e.b = comma == std::string_view::npos ? e.a : part(who.substr(comma + 1));
            if (e.a > e.b) std::swap(e.a, e.b);
            e.said = diagram_label(s.substr(colon + 1));
            events_.push_back(std::move(e));
            continue;
        }
        if (is("autonumber")) {
            const std::string_view rest = after_word(s, 10);
            if (diagram_lower(rest) == "off") {
                numbered = false;
                continue;
            }
            numbered = true;
            char* end = nullptr;
            const std::string r(rest);
            const long long first = std::strtoll(r.c_str(), &end, 10);
            if (end != r.c_str()) {
                number = std::clamp(first, -1000000LL, 1000000LL);
                const long long by = std::strtoll(end, nullptr, 10);
                if (by != 0) step = std::clamp(by, -1000000LL, 1000000LL);
            }
            continue;
        }
        if (is("activate") || is("deactivate")) {
            const bool on = is("activate");
            const std::string_view who = after_word(s, on ? 8 : 10);
            if (who.empty()) return fail(k, "activate whom?");
            Event e;
            e.kind = on ? kActivate : kDeactivate;
            e.a = part(who);
            events_.push_back(std::move(e));
            continue;
        }
        if (is("title")) {
            std::string_view t = after_word(s, 5);
            if (t.starts_with(':')) t = diagram_trim(t.substr(1));
            title_ = std::string(t);
            continue;
        }
        if (is("acctitle") || is("accdescr") || is("links") || is("link") || is("properties") || is("details")) continue;
        // A message: A->>B: text, with any of the arrows (and + or - before B).
        static const char* const kArrows[] = {"<<-->>", "<<->>", "-->>", "->>", "-->", "->", "--x", "-x", "--)", "-)"};
        const size_t colon = s.find(':');
        const std::string_view head = s.substr(0, colon);
        size_t at = std::string_view::npos;
        std::string_view arrow;
        const bool pointed = head.find('>') != std::string_view::npos;
        for (size_t i = 0; i < head.size() && at == std::string_view::npos; ++i)
            for (const char* ar : kArrows) {
                const std::string_view a = ar;
                if (head.substr(i, a.size()) != a) continue;
                if (pointed && a.find('>') == std::string_view::npos) continue;  // (x or ) in a name, before the real arrow)
                at = i, arrow = a;
                break;
            }
        if (at == std::string_view::npos) return fail(k, "not a message, a note or a block: \"" + std::string(s.substr(0, 30)) + "\"");
        Event e;
        e.kind = kMessage;
        std::string_view from = diagram_trim(head.substr(0, at)), to = diagram_trim(head.substr(at + arrow.size()));
        if (to.starts_with('+')) e.activate = true, to = diagram_trim(to.substr(1));
        else if (to.starts_with('-')) e.deactivate = true, to = diagram_trim(to.substr(1));
        if (from.empty() || to.empty()) return fail(k, "a message from whom to whom?");
        e.a = part(from);
        e.b = part(to);
        e.said = colon == std::string_view::npos ? std::vector<std::string>{} : diagram_label(s.substr(colon + 1));
        if (e.said.size() == 1 && e.said[0].empty()) e.said.clear();
        e.dotted = arrow.starts_with("--") || arrow.starts_with("<<--");
        e.both = arrow.starts_with("<<");
        e.head = arrow.ends_with('x') ? kHeadCross : arrow.ends_with(')') ? kHeadOpen : arrow.ends_with(">>") ? kHeadArrow : kHeadNone;
        if (numbered) e.numbered = true, e.number = number, number += step;
        const int here = static_cast<int>(events_.size());
        if (!create.empty()) {
            Part& c = parts_[static_cast<size_t>(part(create))];
            if (c.created < 0) c.created = here;
            create.clear();
        }
        if (!destroy.empty()) {
            Part& c = parts_[static_cast<size_t>(part(destroy))];
            if (c.destroyed < 0) c.destroyed = here;
            destroy.clear();
        }
        events_.push_back(std::move(e));
    }
    if (!open.empty()) return fail(st.size() - 1, open.back() == 'b' ? "a box is not ended" : "a block is not ended (end)");
    if (parts_.empty()) {
        *err = "nobody in it";
        return false;
    }
    // (made by a message they are not in: made at the top, as the others)
    for (Part& p : parts_)
        if (p.created >= 0) {
            const Event& e = events_[static_cast<size_t>(p.created)];
            if (&parts_[static_cast<size_t>(e.b)] != &p) p.created = -1;
        }
    return true;
}

int Sequence::level_at(int part, float y) const {
    int level = 0;
    for (const Bar& b : bars_)
        if (b.part == part && b.y0 <= y + 0.01f && y <= b.y1 + 0.01f) level = std::max(level, b.level + 1);
    return level;
}

float Sequence::edge_at(int part, float y, int s) const {
    const float x = parts_[static_cast<size_t>(part)].x, bw = 10 * u_;
    const int level = level_at(part, y);
    if (level == 0) return x;
    return x + static_cast<float>(level - 1) * bw / 2 + static_cast<float>(s) * bw / 2;
}

void Sequence::text_block(Drawing* d, const std::vector<std::string>& lines, float x, float top, DiagramColour c, int align, bool bold, float size) const {
    if (size <= 0) size = size_;
    const float line = font_.line(size);
    float y = top + (line - font_.ascent(size) - font_.descent(size)) / 2 + font_.ascent(size);
    for (const std::string& l : lines) {
        d->text(l, x, y, size, c, font_, align, bold);
        y += line;
    }
}

void Sequence::lay_out(const Tight& t, Drawing* d, DiagramGeometry* geo) {
    const float u = u_, line = line_;
    const float wrap = std::max(font_.width("abcdefghijklmnopqrstuvwxyz", size_), 160 * u) * t.wrap;
    const float px = 16 * u, py = 13 * u, bw = 10 * u;
    const float margin = 44 * u * t.margin;
    const float min_w = 96 * u * t.min_w;
    const float loop_w = 30 * u, loop_h = 20 * u;
    const float note_pad = 9 * u, frame_pad = 16 * u, inset = 9 * u, group_pad = 10 * u;
    const float number_r = 9 * u;
    const float number_size = size_ * 0.74f;
    // A message's number, and half its circle's width (wider for a long number); 0 for none.
    auto number_of = [](const Event& e) { return e.numbered ? std::to_string(e.number) : std::string(); };
    auto number_half = [&](const Event& e) { return e.numbered ? std::max(number_r, font_.width(number_of(e), number_size, true) / 2 + 4 * u) : 0.0f; };
    const bool dark = p_.dark();
    bars_.clear();

    // ---- sizes
    float head_h = 0;
    for (Part& p : parts_) {
        p.end_y = -1, p.reach = 0;
        p.lines = wrapped(p.name, wrap);
        const float tw = widest(p.lines), th = static_cast<float>(p.lines.size()) * line;
        switch (p.kind) {
            case kActor:
                p.w = std::max(tw + 8 * u, 44 * u);
                head_h = std::max(head_h, 36 * u + 4 * u + th);
                break;
            case kDatabase: {
                const DiagramVec v = diagram_shape_size(kShapeCylinder, tw, th, px, py);
                p.w = std::max(v.x, min_w), head_h = std::max(head_h, v.y);
                break;
            }
            default:
                p.w = std::max(tw + 2 * px, min_w) + (p.kind == kCollections ? 4 * u : 0);
                head_h = std::max(head_h, th + 2 * py + (p.kind == kCollections ? 4 * u : 0));
                break;
        }
    }
    for (Event& e : events_) {
        e.lines = e.said;
        if (e.kind == kMessage) e.lines = diagram_wrap(e.lines, wrap * 1.6f, size_, font_);
        else if (e.kind == kNote) e.lines = diagram_wrap(e.lines, wrap * 1.25f, size_, font_);
        else if (e.kind == kOpen || e.kind == kElse) {
            if (!e.lines.empty()) {
                e.lines.front() = "[" + e.lines.front();
                e.lines.back() += "]";
            }
            e.lines = diagram_wrap(e.lines, wrap * 1.6f, size_, font_);
        }
        e.tw = widest(e.lines);
        e.th = static_cast<float>(e.lines.size()) * line;
    }
    const float tab_size = size_ * 0.92f;
    for (Frame& f : frames_) {
        const Event& e = events_[static_cast<size_t>(f.open)];
        f.tab_w = e.word == "rect" ? 0 : font_.width(e.word, tab_size, true) + 2 * 8 * u + 6 * u;
        f.tab_h = font_.line(tab_size) + 6 * u;
    }

    // ---- across: the gaps between lifelines, wide enough for what is between them
    const size_t n = parts_.size();
    std::vector<float> gap(n > 1 ? n - 1 : 0, 0);
    for (size_t k = 0; k + 1 < n; ++k) {
        gap[k] = parts_[k].w / 2 + parts_[k + 1].w / 2 + margin;
        if (parts_[k].group != parts_[k + 1].group) gap[k] += (parts_[k].group >= 0 ? group_pad : 0) + (parts_[k + 1].group >= 0 ? group_pad : 0);
    }
    struct Need {
        int lo, hi;
        float want;
    };
    std::vector<Need> needs;
    float left = parts_[0].w / 2, right = parts_.back().w / 2;  // past the first and last lifelines
    auto need = [&](int lo, int hi, float want) {
        if (lo == hi) return;
        if (lo > hi) std::swap(lo, hi);
        needs.push_back({lo, hi, want});
    };
    auto out_right = [&](int a, float reach) {  // something reaching `reach` right of a's lifeline
        if (static_cast<size_t>(a) + 1 < n) need(a, a + 1, reach + 12 * u);
        else right = std::max(right, reach);
    };
    auto out_left = [&](int a, float reach) {
        if (a > 0) need(a - 1, a, reach + 12 * u);
        else left = std::max(left, reach);
    };
    const float activation = bw * 1.5f;  // (room for a bar or two at an end)
    for (const Event& e : events_) {
        if (e.kind == kMessage) {
            const float number = e.numbered ? 2 * number_half(e) + 4 * u : 0;
            if (e.a == e.b) out_right(e.a, std::max(8 * u + e.tw, loop_w) + activation + number);
            else if (parts_[static_cast<size_t>(e.b)].created >= 0 && &events_[static_cast<size_t>(parts_[static_cast<size_t>(e.b)].created)] == &e)
                need(e.a, e.b, e.tw + 2 * 14 * u + activation + number + parts_[static_cast<size_t>(e.b)].w / 2);
            else need(e.a, e.b, e.tw + 2 * 14 * u + activation + number);
        } else if (e.kind == kNote) {
            const float nw = e.tw + 2 * note_pad;
            if (e.side == 1) out_right(e.a, 12 * u + activation + nw);
            else if (e.side == -1) out_left(e.a, 12 * u + nw);
            else if (e.a == e.b) {
                const float half = std::max(nw, 60 * u) / 2;
                out_left(e.a, half), out_right(e.a, half);
            } else {
                need(e.a, e.b, nw - 2 * 22 * u);
            }
        }
    }
    for (const Group& g : groups_) {
        int lo = -1, hi = -1;
        for (size_t k = 0; k < n; ++k)
            if (parts_[k].group == &g - groups_.data()) hi = static_cast<int>(k), lo = lo < 0 ? hi : lo;
        if (lo < 0) continue;
        const float lw = widest(g.label, true) + 2 * group_pad;
        need(lo, hi, lw - parts_[static_cast<size_t>(lo)].w / 2 - parts_[static_cast<size_t>(hi)].w / 2);
    }
    // One gap's needs first, then the wider ones, narrowest first, shared out among their gaps.
    std::stable_sort(needs.begin(), needs.end(), [](const Need& a, const Need& b) { return a.hi - a.lo < b.hi - b.lo; });
    for (const Need& nd : needs) {
        float have = 0;
        for (int k = nd.lo; k < nd.hi; ++k) have += gap[static_cast<size_t>(k)];
        if (have >= nd.want) continue;
        const float more = (nd.want - have) / static_cast<float>(nd.hi - nd.lo);
        for (int k = nd.lo; k < nd.hi; ++k) gap[static_cast<size_t>(k)] += more;
    }
    parts_[0].x = 0;
    for (size_t k = 1; k < n; ++k) parts_[k].x = parts_[k - 1].x + gap[k - 1];

    // ---- down: each thing in its band
    float y = 0;
    const float title_size = size_ * 1.15f;
    const std::vector<std::string> title_lines = title_.empty() ? std::vector<std::string>{} : diagram_wrap({title_}, style_.room * 0.95f - 16 * u, title_size, font_);
    float title_w = 0;
    for (const std::string& l : title_lines) title_w = std::max(title_w, font_.width(l, title_size, true));
    const float title_h = title_.empty() ? 0 : static_cast<float>(title_lines.size()) * font_.line(title_size) + 12 * u;
    y += title_h;
    float group_band = 0;
    for (const Group& g : groups_) group_band = std::max(group_band, static_cast<float>(g.label.size()) * line + 6 * u + group_pad);
    if (!groups_.empty()) y += std::max(group_band, group_pad);
    const float top = y;
    for (Part& p : parts_) p.head_y = top;
    y += head_h + 18 * u;
    std::vector<std::vector<size_t>> open_bars(n);
    float anchor = y;  // where an activation said now begins or ends: the last message's line, just after it
    bool after_message = false;
    auto start_bar = [&](int who, float at) {
        Bar b;
        b.part = who, b.level = static_cast<int>(open_bars[static_cast<size_t>(who)].size()), b.y0 = at;
        open_bars[static_cast<size_t>(who)].push_back(bars_.size());
        bars_.push_back(b);
    };
    auto end_bar = [&](int who, float at) {
        std::vector<size_t>& o = open_bars[static_cast<size_t>(who)];
        if (o.empty()) return;
        Bar& b = bars_[o.back()];
        b.y1 = std::max(at, b.y0 + 14 * u);
        o.pop_back();
        y = std::max(y, b.y1 + 4 * u);
    };
    for (size_t i = 0; i < events_.size(); ++i) {
        Event& e = events_[i];
        const bool was_message = after_message;
        after_message = false;
        switch (e.kind) {
            case kMessage: {
                y += 8 * u;
                e.text_y = y;
                if (!e.lines.empty()) y += e.th + 3 * u;
                e.y = y;
                Part& to = parts_[static_cast<size_t>(e.b)];
                if (to.created == static_cast<int>(i)) {  // a box made here, the arrow to its side
                    e.y = y = std::max(y, e.text_y + head_h / 2);
                    to.head_y = e.y - head_h / 2;
                    y = to.head_y + head_h;
                }
                if (e.a == e.b) y += loop_h;
                if (e.activate) start_bar(e.b, e.y);
                if (e.deactivate) end_bar(e.a, e.y);
                for (const int who : {e.a, e.b})
                    if (parts_[static_cast<size_t>(who)].destroyed == static_cast<int>(i)) parts_[static_cast<size_t>(who)].end_y = e.y + (e.a == e.b ? loop_h : 0);
                y += 12 * u;
                anchor = e.a == e.b ? e.y + loop_h : e.y;  // (to itself: where it comes back)
                after_message = true;
                break;
            }
            case kNote:
                y += 8 * u;
                e.y = y;
                e.h = e.th + 2 * note_pad * 0.8f;
                y += e.h + 8 * u;
                break;
            case kOpen: {
                Frame& f = frames_[static_cast<size_t>(e.frame)];
                y += 10 * u;
                f.top = e.y = y;
                y += e.word == "rect" ? 2 * u : std::max(f.tab_h, e.th + 6 * u) + 8 * u;  // (a rect has no tab)
                break;
            }
            case kElse:
                y += 10 * u;
                e.y = y;
                y += (e.lines.empty() ? 4 * u : e.th + 6 * u) + 6 * u;
                break;
            case kEnd: {
                Frame& f = frames_[static_cast<size_t>(e.frame)];
                y += 8 * u;
                f.bottom = e.y = y;
                y += 4 * u;
                break;
            }
            case kActivate:
                start_bar(e.a, was_message ? anchor : y);
                if (was_message) after_message = true;
                break;
            case kDeactivate:
                end_bar(e.a, was_message ? anchor : y);
                if (was_message) after_message = true;
                break;
        }
    }
    y += 14 * u;
    const float foot = y;
    for (size_t k = 0; k < n; ++k)
        while (!open_bars[k].empty()) end_bar(static_cast<int>(k), foot);
    // (a lifeline that ends ends its activations there)
    for (Bar& b : bars_)
        if (const float end = parts_[static_cast<size_t>(b.part)].end_y; end >= 0) b.y1 = std::min(b.y1, end);
    std::erase_if(bars_, [](const Bar& b) { return b.y1 <= b.y0; });
    for (const Bar& b : bars_) {
        Part& p = parts_[static_cast<size_t>(b.part)];
        p.reach = std::max(p.reach, static_cast<float>(b.level) * bw / 2 + bw / 2);
    }

    // ---- across again: messages' ends, notes' boxes, frames round what is in them
    for (size_t i = 0; i < events_.size(); ++i) {
        Event& e = events_[i];
        if (e.kind == kMessage) {
            const Part &a = parts_[static_cast<size_t>(e.a)], &b = parts_[static_cast<size_t>(e.b)];
            if (e.a == e.b) {
                e.x0 = edge_at(e.a, e.y, 1);
                e.x1 = e.x0 + std::max(loop_w, 8 * u + number_half(e) + e.tw);
                continue;
            }
            const int s = b.x > a.x ? 1 : -1;
            e.x0 = edge_at(e.a, e.y, s);
            e.x1 = b.created == static_cast<int>(i) ? b.x - static_cast<float>(s) * b.w / 2 : edge_at(e.b, e.y, -s);
        } else if (e.kind == kNote) {
            const Part &a = parts_[static_cast<size_t>(e.a)], &b = parts_[static_cast<size_t>(e.b)];
            const float nw = e.tw + 2 * note_pad;
            if (e.side == 1) e.x0 = a.x + a.reach + 12 * u, e.x1 = e.x0 + nw;
            else if (e.side == -1) e.x1 = a.x - a.reach - 12 * u, e.x0 = e.x1 - nw;
            else if (e.a == e.b) {
                const float w = std::max(nw, 60 * u);
                e.x0 = a.x - w / 2, e.x1 = a.x + w / 2;
            } else {
                e.x0 = a.x - 22 * u, e.x1 = b.x + 22 * u;
                if (e.x1 - e.x0 < nw) {
                    const float more = (nw - (e.x1 - e.x0)) / 2;
                    e.x0 -= more, e.x1 += more;
                }
            }
        }
    }
    // Frames, the innermost first: round the messages and notes in them, and the frames in them.
    std::vector<int> order(frames_.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return frames_[static_cast<size_t>(a)].depth > frames_[static_cast<size_t>(b)].depth; });
    std::vector<float> lo(frames_.size(), 1e30f), hi(frames_.size(), -1e30f);
    for (size_t i = 0; i < events_.size(); ++i) {
        const Event& e = events_[i];
        if (e.kind != kMessage && e.kind != kNote) continue;
        // (the innermost frame open here)
        int f = -1;
        for (size_t k = 0; k < frames_.size(); ++k) {
            const Frame& fr = frames_[k];
            if (static_cast<int>(i) > fr.open && static_cast<int>(i) < fr.end && (f < 0 || fr.depth > frames_[static_cast<size_t>(f)].depth)) f = static_cast<int>(k);
        }
        if (f < 0) continue;
        const float pad = e.kind == kNote ? 10 * u : frame_pad;
        float x0 = std::min(e.x0, e.x1), x1 = std::max(e.x0, e.x1);
        if (e.kind == kMessage) x0 = std::min(x0, parts_[static_cast<size_t>(e.a)].x), x1 = std::max(x1, parts_[static_cast<size_t>(e.a)].x);
        if (e.kind == kMessage && e.a != e.b) x0 = std::min(x0, parts_[static_cast<size_t>(e.b)].x), x1 = std::max(x1, parts_[static_cast<size_t>(e.b)].x);
        if (e.kind == kMessage && e.a != e.b) {  // (and its label, if it is wider than the line)
            const float mid = (e.x0 + e.x1) / 2;
            x0 = std::min(x0, mid - e.tw / 2), x1 = std::max(x1, mid + e.tw / 2);
        }
        lo[static_cast<size_t>(f)] = std::min(lo[static_cast<size_t>(f)], x0 - pad);
        hi[static_cast<size_t>(f)] = std::max(hi[static_cast<size_t>(f)], x1 + pad);
    }
    for (const int k : order) {
        Frame& f = frames_[static_cast<size_t>(k)];
        float x0 = lo[static_cast<size_t>(k)], x1 = hi[static_cast<size_t>(k)];
        if (x0 > x1) x0 = parts_.front().x - frame_pad, x1 = parts_.back().x + frame_pad;  // (nothing in it: across them all)
        // Wide enough for its tab and what it says (and its else parts').
        float want = f.tab_w + events_[static_cast<size_t>(f.open)].tw + 2 * 12 * u;
        for (const int el : f.elses) want = std::max(want, events_[static_cast<size_t>(el)].tw + 2 * 12 * u);
        if (x1 - x0 < want) {
            const float more = (want - (x1 - x0)) / 2;
            x0 -= more, x1 += more;
        }
        f.x0 = x0, f.x1 = x1;
        if (f.parent >= 0) {
            const float in = f.depth <= 16 ? inset : 0;  // (frames nested deeper than this are drawn on one another)
            lo[static_cast<size_t>(f.parent)] = std::min(lo[static_cast<size_t>(f.parent)], x0 - in);
            hi[static_cast<size_t>(f.parent)] = std::max(hi[static_cast<size_t>(f.parent)], x1 + in);
        }
    }
    for (Group& g : groups_) {
        g.x0 = 1e30f, g.x1 = -1e30f;
        for (const Part& p : parts_)
            if (p.group == &g - groups_.data()) g.x0 = std::min(g.x0, p.x - p.w / 2 - group_pad), g.x1 = std::max(g.x1, p.x + p.w / 2 + group_pad);
        const float lw = widest(g.label, true) + 2 * group_pad;
        if (g.x1 - g.x0 < lw) {
            const float more = (lw - (g.x1 - g.x0)) / 2;
            g.x0 -= more, g.x1 += more;
        }
    }

    // ---- the whole: from its leftmost thing to its rightmost, a margin round it
    float x_lo = parts_.front().x - left, x_hi = parts_.back().x + right;
    for (const Part& p : parts_) x_lo = std::min(x_lo, p.x - p.w / 2), x_hi = std::max(x_hi, p.x + p.w / 2);
    for (const Event& e : events_) {
        if (e.kind == kNote || (e.kind == kMessage && e.a == e.b)) x_lo = std::min(x_lo, e.x0), x_hi = std::max(x_hi, e.x1);
        if (e.kind == kMessage && e.a != e.b) {
            const float mid = (e.x0 + e.x1) / 2;
            x_lo = std::min(x_lo, mid - e.tw / 2), x_hi = std::max(x_hi, mid + e.tw / 2);
        }
    }
    for (const Frame& f : frames_) x_lo = std::min(x_lo, f.x0), x_hi = std::max(x_hi, f.x1);
    for (const Group& g : groups_) x_lo = std::min(x_lo, g.x0), x_hi = std::max(x_hi, g.x1);
    const float edge = 8 * u;
    if (x_hi - x_lo < title_w) {
        const float more = (title_w - (x_hi - x_lo)) / 2;
        x_lo -= more, x_hi += more;
    }
    const float ox = edge - x_lo, oy = edge;
    for (Part& p : parts_) p.x += ox, p.head_y += oy, p.end_y = p.end_y >= 0 ? p.end_y + oy : -1;
    for (Event& e : events_) e.x0 += ox, e.x1 += ox, e.y += oy, e.text_y += oy;
    for (Frame& f : frames_) f.x0 += ox, f.x1 += ox, f.top += oy, f.bottom += oy;
    for (Group& g : groups_) g.x0 += ox, g.x1 += ox;
    for (Bar& b : bars_) b.y0 += oy, b.y1 += oy;
    const float foot_y = foot + oy, top_y = top + oy;
    d->w = x_hi - x_lo + 2 * edge;
    d->h = foot_y + head_h + (groups_.empty() ? 0 : group_pad) + edge;

    // ---- drawn, the back first
    const DiagramColour text = p_.of(kMdText);
    const DiagramColour node_fill = p_.tone(kMdBorder, dark ? 0.17f : 0.11f), node_stroke = p_.tone(kMdBorder, dark ? 0.8f : 0.75f);
    const DiagramColour lifeline = p_.tone(kMdText, dark ? 0.3f : 0.32f);
    const DiagramColour edge_c = p_.tone(kMdText, dark ? 0.62f : 0.68f);
    const DiagramColour note_fill = p_.tone(kMdSlice1, dark ? 0.2f : 0.16f), note_stroke = p_.tone(kMdSlice1, 0.7f);
    const DiagramColour frame_c = p_.tone(kMdAccent, dark ? 0.5f : 0.55f), tab_fill = p_.tone(kMdAccent, dark ? 0.16f : 0.12f);
    const DiagramColour bar_fill = p_.tone(kMdBorder, dark ? 0.3f : 0.2f);
    const DiagramColour bg{p_.bg[0], p_.bg[1], p_.bg[2], 1};
    const float stroke_w = 1.3f * u, edge_w = 1.5f * u;
    // A colour the text says (a box's, a rect's): made quieter in a dark theme, so that text stays readable on it.
    auto said = [&](DiagramColour c) {
        if (c.none()) return c;
        if (c.a < 1) return c;
        const float k = dark ? 0.16f : 0.5f;
        return DiagramColour{p_.bg[0] + (c.r - p_.bg[0]) * k, p_.bg[1] + (c.g - p_.bg[1]) * k, p_.bg[2] + (c.b - p_.bg[2]) * k, 1};
    };
    auto side = [&](float x0, float y0, float x1, float y1, DiagramColour c, float w, std::vector<float> dash = {}) {
        RasterPath p;
        p.move_to(x0, y0), p.line_to(x1, y1);
        const u8 cap = dash.empty() ? RasterStroke::kSquare : RasterStroke::kButt;
        d->shape(std::move(p), {}, c, w, std::move(dash), cap);
    };
    // Where what a frame (or a part of one) says goes across: in the
    // middle, or just right of its tab if that is in the way.
    auto said_x = [&](const Frame& f, const Event& ev, float room_left) {
        float cx = (f.x0 + f.x1) / 2;
        if (cx - ev.tw / 2 < room_left + 10 * u) cx = room_left + 10 * u + ev.tw / 2;
        return cx;
    };
    if (!title_.empty()) {
        text_block(d, title_lines, d->w / 2, oy, text, 1, true, title_size);
        if (geo) geo->boxes.push_back({"node", "title", d->w / 2 - title_w / 2, oy, title_w, title_h - 12 * u});
    }
    // Boxes round participants.
    for (size_t k = 0; k < groups_.size(); ++k) {
        const Group& g = groups_[k];
        if (g.x0 > g.x1) continue;
        const float y0 = top_y - std::max(group_band, group_pad), y1 = foot_y + head_h + group_pad;
        RasterPath box;
        box.rect(g.x0, y0, g.x1 - g.x0, y1 - y0, 4 * u);
        d->shape(std::move(box), !g.fill.none() ? said(g.fill) : p_.tone(kMdAccent, dark ? 0.06f : 0.045f), p_.tone(kMdAccent, dark ? 0.35f : 0.4f), u);
        text_block(d, g.label, (g.x0 + g.x1) / 2, y0 + 4 * u, text, 1, true);
        if (geo) geo->boxes.push_back({"cluster", "box:" + std::to_string(k + 1), g.x0, y0, g.x1 - g.x0, y1 - y0});
    }
    // rect frames: their colour behind everything in them.
    for (const Frame& f : frames_) {
        const Event& e = events_[static_cast<size_t>(f.open)];
        if (e.word != "rect") continue;
        RasterPath r;
        r.rect(f.x0, f.top, f.x1 - f.x0, f.bottom - f.top, 3 * u);
        d->shape(std::move(r), !e.fill.none() ? said(e.fill) : p_.tone(kMdAccent, dark ? 0.1f : 0.08f));
    }
    // Lifelines, and their activations. (A lifeline stops for the text
    // over it: what a message or a frame says.)
    struct Hole {
        float x0, x1, y0, y1;
    };
    std::vector<Hole> holes;
    for (const Event& e : events_)
        if (e.kind == kMessage && e.a != e.b && !e.lines.empty()) {
            const float mid = (e.x0 + e.x1) / 2;
            holes.push_back({mid - e.tw / 2, mid + e.tw / 2, e.text_y, e.text_y + e.th});
        }
    for (const Frame& f : frames_) {
        const Event& e = events_[static_cast<size_t>(f.open)];
        if (e.word == "rect") continue;
        if (!e.lines.empty()) {
            const float cx = said_x(f, e, f.x0 + f.tab_w);
            holes.push_back({cx - e.tw / 2, cx + e.tw / 2, f.top + 3 * u, f.top + 3 * u + e.th});
        }
        for (const int el : f.elses) {
            const Event& ev = events_[static_cast<size_t>(el)];
            if (ev.lines.empty()) continue;
            const float cx = said_x(f, ev, f.x0);
            holes.push_back({cx - ev.tw / 2, cx + ev.tw / 2, ev.y + 3 * u, ev.y + 3 * u + ev.th});
        }
    }
    std::stable_sort(holes.begin(), holes.end(), [](const Hole& a, const Hole& b) { return a.y0 < b.y0; });
    for (size_t k = 0; k < n; ++k) {
        const Part& p = parts_[k];
        const float y0 = p.head_y + head_h, y1 = p.end_y >= 0 ? p.end_y : foot_y;
        float from = y0;
        for (const Hole& h : holes) {
            if (p.x < h.x0 - 4 * u || p.x > h.x1 + 4 * u || h.y1 < from || h.y0 > y1) continue;
            if (h.y0 - 2 * u > from) side(p.x, from, p.x, h.y0 - 2 * u, lifeline, u);
            from = std::max(from, h.y1 + 2 * u);
        }
        if (from < y1) side(p.x, from, p.x, y1, lifeline, u);
        if (geo) {
            const float r = std::max(p.reach, 2 * u);
            geo->boxes.push_back({"cluster", p.id, p.x - r, y0, 2 * r, y1 - y0});
        }
    }
    std::vector<size_t> bar_order(bars_.size());
    for (size_t k = 0; k < bar_order.size(); ++k) bar_order[k] = k;
    std::stable_sort(bar_order.begin(), bar_order.end(), [&](size_t a, size_t b) { return bars_[a].level < bars_[b].level; });
    for (const size_t k : bar_order) {
        const Bar& b = bars_[k];
        const float x = parts_[static_cast<size_t>(b.part)].x + static_cast<float>(b.level) * bw / 2 - bw / 2;
        RasterPath r;
        r.rect(x, b.y0, bw, b.y1 - b.y0, u);
        d->shape(std::move(r), bar_fill, node_stroke, u);
    }
    // Frames: their sides, a tab with what they are, what they say; their parts' dividers.
    for (size_t k = 0; k < frames_.size(); ++k) {
        const Frame& f = frames_[k];
        const Event& e = events_[static_cast<size_t>(f.open)];
        if (e.word == "rect") continue;
        const float fw = u;
        // (each side its own shape: a thousand frames, one in another, are a thousand thin shapes, not a thousand big ones)
        side(f.x0, f.top, f.x1, f.top, frame_c, fw);
        side(f.x1, f.top, f.x1, f.bottom, frame_c, fw);
        side(f.x1, f.bottom, f.x0, f.bottom, frame_c, fw);
        side(f.x0, f.bottom, f.x0, f.top, frame_c, fw);
        const float cut = 6 * u;
        const RasterPoint tab[] = {{f.x0, f.top}, {f.x0 + f.tab_w, f.top}, {f.x0 + f.tab_w, f.top + f.tab_h - cut}, {f.x0 + f.tab_w - cut, f.top + f.tab_h}, {f.x0, f.top + f.tab_h}};
        RasterPath tp;
        tp.polygon(tab, 5);
        d->shape(std::move(tp), tab_fill, frame_c, fw);
        text_block(d, {e.word}, f.x0 + 8 * u, f.top + (f.tab_h - font_.line(tab_size)) / 2, text, 0, true, tab_size);
        if (geo) {
            geo->boxes.push_back({"frame", e.word + ":" + std::to_string(k + 1), f.x0, f.top, f.x1 - f.x0, f.bottom - f.top});
            geo->boxes.push_back({"label", "tab:" + std::to_string(k + 1), f.x0, f.top, f.tab_w, f.tab_h});
        }
        // What it says: in the middle, or just right of the tab if that is in the way.
        auto said_text = [&](const Event& ev, float top_at, float room_left) {
            if (ev.lines.empty()) return;
            const float cx = said_x(f, ev, room_left);
            text_block(d, ev.lines, cx, top_at, text, 1);
            if (geo) geo->boxes.push_back({"label", "said:" + std::to_string(&ev - events_.data()), cx - ev.tw / 2, top_at, ev.tw, ev.th});
        };
        said_text(e, f.top + 3 * u, f.x0 + f.tab_w);
        for (const int el : f.elses) {
            const Event& ev = events_[static_cast<size_t>(el)];
            side(f.x0, ev.y, f.x1, ev.y, frame_c, fw, {4 * u, 3 * u});
            said_text(ev, ev.y + 3 * u, f.x0);
        }
    }
    // Notes.
    for (size_t i = 0; i < events_.size(); ++i) {
        const Event& e = events_[i];
        if (e.kind != kNote) continue;
        RasterPath r;
        r.rect(e.x0, e.y, e.x1 - e.x0, e.h, 2 * u);
        d->shape(std::move(r), note_fill, note_stroke, stroke_w);
        text_block(d, e.lines, (e.x0 + e.x1) / 2, e.y + (e.h - e.th) / 2, text, 1);
        if (geo) geo->boxes.push_back({"node", "note:" + std::to_string(i), e.x0, e.y, e.x1 - e.x0, e.h});
    }
    // Messages.
    const float head = 8 * u + edge_w * 1.6f;
    for (size_t i = 0; i < events_.size(); ++i) {
        const Event& e = events_[i];
        if (e.kind != kMessage) continue;
        std::vector<float> dash;
        if (e.dotted) dash = {2.2f * edge_w, 2.4f * edge_w};
        RasterPath path;
        std::string to = parts_[static_cast<size_t>(e.b)].id;
        if (e.a == e.b) {  // out to the right, down and back, its corners round
            const float x0 = e.x0, xr = x0 + loop_w, y0 = e.y, y1 = e.y + loop_h, r = 7 * u;
            const float back = edge_at(e.a, y1, 1);
            const float stop = back + (e.head != kHeadNone ? diagram_head_length(e.head, head) * (e.head == kHeadOpen ? 0.2f : 1) : 0);
            path.move_to(x0, y0);
            path.line_to(xr - r, y0);
            path.quad_to(xr, y0, xr, y0 + r);
            path.line_to(xr, y1 - r);
            path.quad_to(xr, y1, xr - r, y1);
            path.line_to(stop, y1);
            if (geo) {
                RasterPath whole = path;
                whole.line_to(back, y1);
                geo->lines.push_back({parts_[static_cast<size_t>(e.a)].id, to, diagram_sample(whole, 9)});
            }
            d->shape(std::move(path), {}, edge_c, edge_w, dash, RasterStroke::kButt);
            if (e.head != kHeadNone) diagram_head(*d, e.head, {back, y1}, {-1, 0}, head, edge_c, edge_w);
            if (!e.lines.empty()) {
                const float tx = x0 + 8 * u + number_half(e);  // (clear of its number)
                text_block(d, e.lines, tx, e.text_y, text, 0);
                if (geo) geo->boxes.push_back({"label", "message:" + std::to_string(i), tx, e.text_y, e.tw, e.th});
            }
        } else {
            const float s = e.x1 > e.x0 ? 1.0f : -1.0f;
            const bool made = parts_[static_cast<size_t>(e.b)].created == static_cast<int>(i);
            if (made) to = "head:" + to;
            const float a = e.x0, b = e.x1;
            // (a head at its start begins at its number's circle, not under it)
            float start = a;
            if (e.both) start += s * number_half(e);
            const u8 back_head = e.head == kHeadNone ? static_cast<u8>(kHeadArrow) : e.head;  // (<<->>: a head at each end)
            const float ha = e.both ? diagram_head_length(back_head, head) : 0;
            const bool ended = parts_[static_cast<size_t>(e.b)].destroyed == static_cast<int>(i) && e.head == kHeadCross;  // (its lifeline's cross is its head)
            const float hb = e.head != kHeadNone && !ended ? diagram_head_length(e.head, head) : 0;
            path.move_to(start + s * ha * (e.head == kHeadOpen ? 0.2f : 1), e.y);
            path.line_to(b - s * hb * (e.head == kHeadOpen ? 0.2f : 1), e.y);
            d->shape(std::move(path), {}, edge_c, edge_w, dash, RasterStroke::kButt);
            if (e.head != kHeadNone && !ended) diagram_head(*d, e.head, {b, e.y}, {s, 0}, head, edge_c, edge_w);
            if (e.both) diagram_head(*d, back_head, {start, e.y}, {-s, 0}, head, edge_c, edge_w);
            if (geo) {
                std::vector<RasterPoint> pts;
                for (int k = 0; k <= 8; ++k) pts.push_back({a + (b - a) * static_cast<float>(k) / 8, e.y});
                geo->lines.push_back({parts_[static_cast<size_t>(e.a)].id, to, std::move(pts)});
            }
            if (!e.lines.empty()) {
                const float mid = (a + b) / 2;
                text_block(d, e.lines, mid, e.text_y, text, 1);
                if (geo) geo->boxes.push_back({"label", "message:" + std::to_string(i), mid - e.tw / 2, e.text_y, e.tw, e.th});
            }
        }
        if (e.numbered) {  // autonumber: in a circle at its start
            const std::string num = number_of(e);
            const float r = number_r, half = number_half(e);
            RasterPath c;
            c.rect(e.x0 - half, e.y - r, 2 * half, 2 * r, r);
            d->shape(std::move(c), edge_c);
            d->text(num, e.x0, e.y + (font_.ascent(number_size) - font_.descent(number_size)) / 2, number_size, bg, font_, 1, true);
            if (geo) geo->boxes.push_back({"number", "number:" + num, e.x0 - half, e.y - r, 2 * half, 2 * r});
        }
    }
    // Ends of lifelines that were destroyed: a cross.
    for (const Part& p : parts_) {
        if (p.end_y < 0) continue;
        const float r = 7 * u;
        RasterPath x;
        x.move_to(p.x - r, p.end_y - r), x.line_to(p.x + r, p.end_y + r);
        x.move_to(p.x + r, p.end_y - r), x.line_to(p.x - r, p.end_y + r);
        d->shape(std::move(x), {}, edge_c, 2 * u, {}, RasterStroke::kRound);
    }
    // The participants, at the top (or where they were made) and again at the foot.
    auto participant = [&](const Part& p, float y0, const std::string& name) {
        const float x0 = p.x - p.w / 2;
        const float th = static_cast<float>(p.lines.size()) * line;
        if (geo) geo->boxes.push_back({"node", name, x0, y0, p.w, head_h});
        switch (p.kind) {
            case kActor: {
                // A stick figure, its name under it.
                const float fh = head_h - th - 4 * u, cx = p.x;
                const float r = fh * 0.17f, neck = y0 + 2 * r + u, hip = y0 + fh * 0.66f, arms = neck + fh * 0.13f, spread = fh * 0.3f;
                RasterPath headc;
                headc.ellipse(cx, y0 + r + u * 0.5f, r, r);
                d->shape(std::move(headc), node_fill, node_stroke, 1.5f * u);
                RasterPath body;
                body.move_to(cx, neck), body.line_to(cx, hip);
                body.move_to(cx - spread, arms), body.line_to(cx + spread, arms);
                body.move_to(cx - spread * 0.8f, y0 + fh), body.line_to(cx, hip), body.line_to(cx + spread * 0.8f, y0 + fh);
                d->shape(std::move(body), {}, node_stroke, 1.5f * u, {}, RasterStroke::kRound);
                text_block(d, p.lines, cx, y0 + head_h - th, text, 1);
                break;
            }
            case kDatabase: {
                RasterPath outline, rim;
                diagram_shape(kShapeCylinder, x0, y0, p.w, head_h, u, &outline);
                diagram_shape_extra(kShapeCylinder, x0, y0, p.w, head_h, u, &rim);
                d->shape(std::move(outline), node_fill, node_stroke, stroke_w);
                d->shape(std::move(rim), {}, node_stroke, stroke_w);
                const float ry = std::min(head_h / 4, p.w / 2 / (2.5f + p.w / (50 * u)));
                text_block(d, p.lines, p.x, y0 + (head_h - th) / 2 + ry / 2, text, 1);
                break;
            }
            case kCollections: {
                const float o = 4 * u;
                RasterPath back, front;
                back.rect(x0 + o, y0, p.w - o, head_h - o, 3 * u);
                d->shape(std::move(back), node_fill, node_stroke, stroke_w);
                front.rect(x0, y0 + o, p.w - o, head_h - o, 3 * u);
                d->shape(std::move(front), node_fill, node_stroke, stroke_w);
                text_block(d, p.lines, p.x - o / 2, y0 + o + (head_h - o - th) / 2, text, 1);
                break;
            }
            default: {
                RasterPath box;
                box.rect(x0, y0, p.w, head_h, 3 * u);
                d->shape(std::move(box), node_fill, node_stroke, stroke_w);
                text_block(d, p.lines, p.x, y0 + (head_h - th) / 2, text, 1);
                break;
            }
        }
    };
    for (const Part& p : parts_) {
        participant(p, p.head_y, "head:" + p.id);
        if (p.end_y < 0) participant(p, foot_y, "foot:" + p.id);
    }
}

}  // namespace

bool sequence_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    Sequence seq(style);
    if (!seq.parse(st, err)) return false;
    DiagramGeometry g;
    seq.lay_out({}, out, geo ? &g : nullptr);
    // Too wide for the room: narrower (labels wrapped shorter, boxes and
    // gaps smaller), if that is less too wide.
    if (out->w > style.room) {
        Drawing d2;
        DiagramGeometry g2;
        seq.lay_out({0.6f, 0.6f, 0.45f}, &d2, geo ? &g2 : nullptr);
        if (d2.w < out->w) {
            *out = std::move(d2);
            g = std::move(g2);
        }
    }
    if (geo) *geo = std::move(g);
    return true;
}

}  // namespace blit
