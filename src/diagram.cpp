#include "diagram.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "utf8.hpp"

namespace blit {

// ---------------------------------------------------------------- colours

DiagramColour DiagramPalette::of(u8 which, float alpha) const {
    const float* c = ink[which < kMdInkCount ? which : 0];
    return {c[0], c[1], c[2], alpha};
}

DiagramColour DiagramPalette::tone(u8 which, float k) const {
    const float* c = ink[which < kMdInkCount ? which : 0];
    return {bg[0] + (c[0] - bg[0]) * k, bg[1] + (c[1] - bg[1]) * k, bg[2] + (c[2] - bg[2]) * k, 1};
}

std::vector<u8> diagram_series(const DiagramPalette& p, u8 avoid) {
    std::vector<u8> out;
    auto near_to = [&](const float* c, u8 o) {
        const float* q = p.ink[o];
        return std::fabs(c[0] - q[0]) + std::fabs(c[1] - q[1]) + std::fabs(c[2] - q[2]) < 0.24f;
    };
    for (const u8 i : {kMdSlice0, kMdSlice1, kMdSlice2, kMdSlice3, kMdHeading1, kMdHeading2, kMdHeading3, kMdAccent, kMdLink, kMdLabel, kMdCodeConstant, kMdCodeKeyword, kMdCodeType,
                       kMdCodeString}) {
        const float* c = p.ink[i];
        // (a colour, not a grey, and not too near the background in
        // brightness; and not near one already taken)
        if (std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}) < 0.12f) continue;
        if (std::fabs(c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f - (p.bg[0] * 0.3f + p.bg[1] * 0.59f + p.bg[2] * 0.11f)) < 0.15f) continue;
        bool near = avoid < kMdInkCount && near_to(c, avoid);
        for (const u8 o : out) near = near || near_to(c, o);
        if (!near) out.push_back(i);
        if (out.size() == 8) break;
    }
    if (out.empty()) out.push_back(kMdAccent);
    return out;
}

bool diagram_colour(std::string_view s, DiagramColour* out) {
    s = diagram_trim(s);
    const std::string l = diagram_lower(s);
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (l.starts_with('#')) {
        const std::string h = l.substr(1);
        for (const char c : h)
            if (hex(c) < 0) return false;
        if (h.size() == 3 || h.size() == 4) {
            out->r = hex(h[0]) * 17 / 255.0f, out->g = hex(h[1]) * 17 / 255.0f, out->b = hex(h[2]) * 17 / 255.0f;
            out->a = h.size() == 4 ? hex(h[3]) * 17 / 255.0f : 1;
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            auto byte = [&](size_t i) { return (hex(h[i]) * 16 + hex(h[i + 1])) / 255.0f; };
            out->r = byte(0), out->g = byte(2), out->b = byte(4), out->a = h.size() == 8 ? byte(6) : 1;
            return true;
        }
        return false;
    }
    if (l.starts_with("rgb")) {
        const size_t open = l.find('(');
        if (open == std::string::npos) return false;
        float v[4] = {0, 0, 0, 1};
        int n = 0;
        const char* p = l.c_str() + open + 1;
        while (n < 4 && *p) {
            char* end = nullptr;
            const float x = std::strtof(p, &end);
            if (end == p) {
                ++p;
                continue;
            }
            v[n] = *end == '%' ? x / 100 * (n < 3 ? 255 : 1) : x;
            ++n;
            p = end;
        }
        if (n < 3) return false;
        out->r = std::clamp(v[0] / 255, 0.0f, 1.0f), out->g = std::clamp(v[1] / 255, 0.0f, 1.0f), out->b = std::clamp(v[2] / 255, 0.0f, 1.0f);
        out->a = std::clamp(v[3], 0.0f, 1.0f);
        return true;
    }
    static const struct {
        const char* name;
        u32 rgb;
    } kNames[] = {{"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000},   {"green", 0x008000},  {"blue", 0x0000ff},      {"yellow", 0xffff00},
                  {"orange", 0xffa500}, {"purple", 0x800080}, {"pink", 0xffc0cb}, {"grey", 0x808080},   {"gray", 0x808080},      {"lightgrey", 0xd3d3d3},
                  {"lightgray", 0xd3d3d3}, {"darkgrey", 0xa9a9a9}, {"darkgray", 0xa9a9a9}, {"cyan", 0x00ffff}, {"magenta", 0xff00ff}, {"brown", 0xa52a2a},
                  {"navy", 0x000080}, {"teal", 0x008080},  {"lime", 0x00ff00},  {"olive", 0x808000},  {"maroon", 0x800000},    {"silver", 0xc0c0c0},
                  {"gold", 0xffd700},   {"coral", 0xff7f50}, {"salmon", 0xfa8072}, {"violet", 0xee82ee}, {"lightblue", 0xadd8e6}, {"lightgreen", 0x90ee90},
                  {"darkgreen", 0x006400}, {"darkblue", 0x00008b}, {"darkred", 0x8b0000}, {"indigo", 0x4b0082}, {"tomato", 0xff6347}, {"skyblue", 0x87ceeb},
                  {"aqua", 0x00ffff},   {"fuchsia", 0xff00ff}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"beige", 0xf5f5dc}, {"turquoise", 0x40e0d0},
                  {"plum", 0xdda0dd},   {"orchid", 0xda70d6}, {"wheat", 0xf5deb3}, {"tan", 0xd2b48c},    {"crimson", 0xdc143c},   {"chocolate", 0xd2691e},
                  {"aquamarine", 0x7fffd4}, {"lightyellow", 0xffffe0}, {"lightpink", 0xffb6c1}, {"steelblue", 0x4682b4}, {"slategray", 0x708090}, {"slategrey", 0x708090}};
    for (const auto& n : kNames)
        if (l == n.name) {
            out->r = (n.rgb >> 16) / 255.0f, out->g = ((n.rgb >> 8) & 255) / 255.0f, out->b = (n.rgb & 255) / 255.0f, out->a = 1;
            return true;
        }
    if (l == "transparent" || l == "none") {
        *out = {};
        out->a = 0.0001f;  // (said, and nothing: not the same as not said)
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the font

DiagramFont::~DiagramFont() {
    for (Fallback& f : fallbacks_)
        if (f.face) FT_Done_Face(f.face);
    if (face_) FT_Done_Face(face_);
    if (lib_) FT_Done_FreeType(lib_);
}

bool DiagramFont::open(const std::string& path, int index, const std::vector<FontFile>& fallbacks) {
    if (same(path, index, fallbacks) && (face_ || lib_)) return face_ != nullptr;
    if (face_) FT_Done_Face(face_), face_ = nullptr;
    for (Fallback& f : fallbacks_)
        if (f.face) FT_Done_Face(f.face);
    fallbacks_.clear();
    for (const FontFile& f : fallbacks) fallbacks_.push_back({f});
    fallback_files_ = fallbacks;
    glyphs_.clear();
    path_ = path, index_ = index;
    if (!lib_ && FT_Init_FreeType(&lib_)) {
        lib_ = nullptr;
        return false;
    }
    if (path.empty() || FT_New_Face(lib_, path.c_str(), index, &face_) || !FT_IS_SCALABLE(face_)) {
        if (face_) FT_Done_Face(face_);
        face_ = nullptr;
        return false;
    }
    units_ = face_->units_per_EM ? static_cast<float>(face_->units_per_EM) : 1000;
    ascent_ = face_->ascender / units_, descent_ = -face_->descender / units_;
    if (!(ascent_ > 0.3f && ascent_ < 2) || !(descent_ >= 0 && descent_ < 1)) ascent_ = 0.8f, descent_ = 0.2f;
    return true;
}

namespace {

struct Outliner {
    RasterPath* out;
    float k;  // ems to a font unit
    bool open = false;
};
int ft_move(const FT_Vector* to, void* user) {
    Outliner& o = *static_cast<Outliner*>(user);
    if (o.open) o.out->close();
    o.out->move_to(to->x * o.k, -to->y * o.k);
    o.open = true;
    return 0;
}
int ft_line(const FT_Vector* to, void* user) {
    Outliner& o = *static_cast<Outliner*>(user);
    o.out->line_to(to->x * o.k, -to->y * o.k);
    return 0;
}
int ft_conic(const FT_Vector* c, const FT_Vector* to, void* user) {
    Outliner& o = *static_cast<Outliner*>(user);
    o.out->quad_to(c->x * o.k, -c->y * o.k, to->x * o.k, -to->y * o.k);
    return 0;
}
int ft_cubic(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
    Outliner& o = *static_cast<Outliner*>(user);
    o.out->cubic_to(c1->x * o.k, -c1->y * o.k, c2->x * o.k, -c2->y * o.k, to->x * o.k, -to->y * o.k);
    return 0;
}

}  // namespace

FT_Face DiagramFont::face_for(u32 cp, unsigned* gi, float* units) const {
    if ((*gi = FT_Get_Char_Index(face_, cp))) {
        *units = units_;
        return face_;
    }
    auto has = [&](Fallback& f) {
        if (!f.tried) {
            f.tried = true;
            if (FT_New_Face(lib_, f.file.path.c_str(), f.file.index, &f.face) || !FT_IS_SCALABLE(f.face)) {
                if (f.face) FT_Done_Face(f.face);
                f.face = nullptr;
            } else {
                f.units = f.face->units_per_EM ? static_cast<float>(f.face->units_per_EM) : 1000;
            }
        }
        if (!f.face || !(*gi = FT_Get_Char_Index(f.face, cp))) return false;
        *units = f.units;
        return true;
    };
    for (Fallback& f : fallbacks_)
        if (has(f)) return f.face;
    if (cp < 0x20) return nullptr;
    for (const FontFile& file : font_fallbacks_for(cp)) {
        if (std::ranges::any_of(fallbacks_, [&](const Fallback& f) { return f.file == file; })) continue;
        fallbacks_.push_back({file});
        if (has(fallbacks_.back())) return fallbacks_.back().face;
    }
    return nullptr;
}

const DiagramFont::Glyph& DiagramFont::glyph(u32 cp) const {
    Glyph& g = glyphs_[cp];
    if (g.done) return g;
    g.done = true;
    const u32 base = cp & 0x7FFFFFFF;
    const bool bold = cp >> 31;
    const u32 w = cp_width(base);
    g.advance = 0.6f * static_cast<float>(w);
    if (!face_) {
        if (w && base != ' ') g.shape.rect(0.08f, -0.7f, 0.6f * w - 0.16f, 0.7f, 0);
        return g;
    }
    unsigned gi = 0;
    float units = units_;
    FT_Face face = face_for(base, &gi, &units);
    if (!face) {
        // In no font: a box, as wide as the character is (a space is a space).
        if (w && base != ' ' && base >= 0x20) {
            const float x0 = 0.1f, x1 = 0.6f * static_cast<float>(w) - 0.1f, y0 = -0.72f, y1 = 0.02f, t = 0.06f;
            const RasterPoint outer[] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, inner[] = {{x0 + t, y0 + t}, {x0 + t, y1 - t}, {x1 - t, y1 - t}, {x1 - t, y0 + t}};
            g.shape.polygon(outer, 4);
            g.shape.polygon(inner, 4);
        }
        return g;
    }
    // In the font's own units, unhinted: the same outline on every system,
    // at every size.
    if (FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) return g;
    g.advance = face->glyph->advance.x / units;
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return g;
    FT_Outline& ol = face->glyph->outline;
    if (bold) FT_Outline_Embolden(&ol, static_cast<FT_Pos>(units / 28));
    FT_Outline_Funcs fns{};
    fns.move_to = ft_move, fns.line_to = ft_line, fns.conic_to = ft_conic, fns.cubic_to = ft_cubic;
    Outliner o{&g.shape, 1 / units};
    FT_Outline_Decompose(&ol, &fns, &o);
    if (o.open) g.shape.close();
    if (bold) g.advance += 1 / 28.0f;
    return g;
}

float DiagramFont::width(std::string_view text, float size, bool bold) const {
    float w = 0;
    const u8* p = reinterpret_cast<const u8*>(text.data());
    for (size_t i = 0; i < text.size();) {
        const Decoded d = decode_utf8(p + i, text.size() - i);
        i += d.len ? d.len : 1;
        if (d.cp == kDiagramBold) bold = !bold;
        if (d.cp == kDiagramBold || d.cp == kDiagramItalic) continue;
        w += glyph(d.cp | (bold ? 0x80000000u : 0)).advance;
    }
    return w * size;
}

void DiagramFont::outline(std::string_view text, float x, float y, float size, bool bold, RasterPath* out) const {
    const u8* p = reinterpret_cast<const u8*>(text.data());
    bool italic = false;
    for (size_t i = 0; i < text.size();) {
        const Decoded d = decode_utf8(p + i, text.size() - i);
        i += d.len ? d.len : 1;
        if (d.cp == kDiagramBold) bold = !bold;
        if (d.cp == kDiagramItalic) italic = !italic;
        if (d.cp == kDiagramBold || d.cp == kDiagramItalic) continue;
        const Glyph& g = glyph(d.cp | (bold ? 0x80000000u : 0));
        // (italic: the upright outline slanted, a fifth of its height across)
        const float slant = italic ? 0.2f : 0;
        auto px = [&](const float* q) { return x + (q[0] - q[1] * slant) * size; };
        size_t at = 0;
        for (const u8 v : g.shape.verbs) {
            const float* q = g.shape.pts.data() + at;
            switch (v) {
                case RasterPath::kMove: out->move_to(px(q), y + q[1] * size), at += 2; break;
                case RasterPath::kLine: out->line_to(px(q), y + q[1] * size), at += 2; break;
                case RasterPath::kQuad: out->quad_to(px(q), y + q[1] * size, px(q + 2), y + q[3] * size), at += 4; break;
                case RasterPath::kCubic:
                    out->cubic_to(px(q), y + q[1] * size, px(q + 2), y + q[3] * size, px(q + 4), y + q[5] * size);
                    at += 6;
                    break;
                default: out->close(); break;
            }
        }
        x += g.advance * size;
    }
}

// ---------------------------------------------------------------- the drawing

void Drawing::shape(RasterPath p, DiagramColour fill, DiagramColour stroke, float width, std::vector<float> dash, u8 cap) {
    DiagramItem it;
    it.path = std::move(p);
    it.fill = fill, it.stroke = stroke, it.width = width;
    it.dash = std::move(dash);
    it.cap = cap;
    items.push_back(std::move(it));
}

void Drawing::text(std::string_view s, float x, float baseline, float size, DiagramColour c, const DiagramFont& font, int align, bool bold) {
    if (s.empty()) return;
    DiagramItem it;
    it.kind = DiagramItem::kText;
    it.text = std::string(s);
    const float wide = font.width(s, size, bold);
    it.x = align == 1 ? x - wide / 2 : align == 2 ? x - wide : x;
    it.y = baseline, it.size = size, it.bold = bold;
    it.fill = c;
    items.push_back(std::move(it));
}

void diagram_paint(const Drawing& d, float scale, const DiagramFont& font, RasterImage* out) {
    for (const DiagramItem& it : d.items) {
        if (it.kind == DiagramItem::kText) {
            RasterPath p;
            font.outline(it.text, it.x, it.y, it.size, it.bold, &p);
            if (it.italic)
                for (size_t i = 0; i < p.pts.size(); i += 2) p.pts[i] += (it.y - p.pts[i + 1]) * 0.2f;
            const float c[4] = {it.fill.r, it.fill.g, it.fill.b, it.fill.a};
            out->fill(raster_flatten(p, scale, 0, 0, 0.04f), c);
            continue;
        }
        if (!it.fill.none()) {
            const float c[4] = {it.fill.r, it.fill.g, it.fill.b, it.fill.a};
            out->fill(raster_flatten(it.path, scale, 0, 0), c);
        }
        if (!it.stroke.none() && it.width > 0) {
            RasterLines l = raster_flatten(it.path, scale, 0, 0);
            if (!it.dash.empty()) {
                std::vector<float> dash = it.dash;
                for (float& v : dash) v *= scale;
                l = raster_dash(l, dash.data(), dash.size());
            }
            RasterStroke st;
            st.width = it.width * scale;
            st.cap = it.cap, st.join = it.join;
            const float c[4] = {it.stroke.r, it.stroke.g, it.stroke.b, it.stroke.a};
            out->fill(raster_stroke(l, st), c);
        }
    }
}

std::string DiagramGeometry::dump() const {
    std::string out;
    char buf[160];
    if (edges >= 0) out += "edges " + std::to_string(edges) + "\n";
    for (const Box& b : boxes) {
        std::snprintf(buf, sizeof buf, " %.1f %.1f %.1f %.1f\n", b.x, b.y, b.w, b.h);
        out += b.kind + " " + b.name + buf;
    }
    for (const Line& l : lines) {
        out += "edge " + l.from + " " + l.to;
        for (const RasterPoint& p : l.pts) {
            std::snprintf(buf, sizeof buf, " %.1f,%.1f", p.x, p.y);
            out += buf;
        }
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------- Mermaid's text

std::string_view diagram_trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string diagram_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> diagram_statements(std::string_view src, bool split_semicolons, std::string* title) {
    std::vector<std::string> out;
    bool first = true, front = false;
    for (size_t at = 0; at <= src.size();) {
        const size_t nl = src.find('\n', at);
        std::string_view l = src.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? src.size() + 1 : nl + 1;
        // (front matter, --- to ---, at the top: only its title is read)
        if (first && diagram_trim(l).empty()) continue;
        if (first && diagram_trim(l) == "---") {
            first = false, front = true;
            continue;
        }
        first = false;
        if (front) {
            if (diagram_trim(l) == "---") front = false;
            else if (const std::string_view t = diagram_trim(l); title && t.starts_with("title:")) {
                std::string_view v = diagram_trim(t.substr(6));
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                *title = std::string(v);
            }
            continue;
        }
        if (const size_t c = l.find("%%"); c != std::string_view::npos) l = l.substr(0, c);
        if (split_semicolons) {
            bool quoted = false;
            size_t from = 0;
            for (size_t i = 0; i < l.size(); ++i) {
                if (l[i] == '"') quoted = !quoted;
                if (l[i] == ';' && !quoted) {
                    if (!diagram_trim(l.substr(from, i - from)).empty()) out.emplace_back(diagram_trim(l.substr(from, i - from)));
                    from = i + 1;
                }
            }
            l = l.substr(from);
        }
        if (!diagram_trim(l).empty()) out.emplace_back(diagram_trim(l));
    }
    return out;
}

std::vector<DiagramLine> diagram_lines(std::string_view src, std::string* title) {
    std::vector<DiagramLine> out;
    bool first = true, front = false;
    int no = 0;
    for (size_t at = 0; at <= src.size();) {
        const size_t nl = src.find('\n', at);
        std::string_view l = src.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? src.size() + 1 : nl + 1;
        ++no;
        // (front matter as diagram_statements() reads it)
        if (first && diagram_trim(l).empty()) continue;
        if (first && diagram_trim(l) == "---") {
            first = false, front = true;
            continue;
        }
        first = false;
        if (front) {
            if (diagram_trim(l) == "---") front = false;
            else if (const std::string_view t = diagram_trim(l); title && t.starts_with("title:")) {
                std::string_view v = diagram_trim(t.substr(6));
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
                *title = std::string(v);
            }
            continue;
        }
        if (const size_t c = l.find("%%"); c != std::string_view::npos) l = l.substr(0, c);
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.remove_suffix(1);
        if (!diagram_trim(l).empty()) out.push_back({no, std::string(l)});
    }
    return out;
}

std::vector<std::string> diagram_label(std::string_view s) {
    s = diagram_trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    bool markdown = false;
    if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2), markdown = true;
    std::string t(s);
    auto replace = [&](std::string_view from, std::string_view to) {
        for (size_t at = 0; (at = t.find(from, at)) != std::string::npos; at += to.size()) t.replace(at, from.size(), to);
    };
    for (const char* br : {"<br/>", "<br />", "<br>", "<BR>", "<BR/>", "\\n"}) replace(br, "\n");
    if (markdown) {  // (**bold** and __bold__, *italic*: the font's marks for them, kDiagramBold and kDiagramItalic)
        replace("**", "\x02"), replace("__", "\x02");
        if (std::ranges::count(t, '*') % 2 == 0) replace("*", "\x03");
    }
    for (const auto& [e, r] : std::initializer_list<std::pair<const char*, const char*>>{
             {"&quot;", "\""}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"#quot;", "\""}, {"#amp;", "&"}, {"#lt;", "<"}, {"#gt;", ">"}})
        replace(e, r);
    // #9829; and &#9829;: a character by its number
    for (size_t at = 0; (at = t.find('#', at)) != std::string::npos;) {
        size_t e = at + 1;
        while (e < t.size() && std::isdigit(static_cast<unsigned char>(t[e])) && e - at < 8) ++e;
        if (e > at + 1 && e < t.size() && t[e] == ';') {
            const u32 cp = static_cast<u32>(std::strtoul(t.c_str() + at + 1, nullptr, 10));
            std::string u;
            if (cp && cp < 0x80) u += static_cast<char>(cp);
            else if (cp < 0x800) u += static_cast<char>(0xC0 | (cp >> 6)), u += static_cast<char>(0x80 | (cp & 0x3F));
            else if (cp < 0x10000) u += static_cast<char>(0xE0 | (cp >> 12)), u += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), u += static_cast<char>(0x80 | (cp & 0x3F));
            else if (cp < 0x110000)
                u += static_cast<char>(0xF0 | (cp >> 18)), u += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), u += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                    u += static_cast<char>(0x80 | (cp & 0x3F));
            const size_t from = at > 0 && t[at - 1] == '&' ? at - 1 : at;
            t.replace(from, e + 1 - from, u);
            at = from + u.size();
        } else {
            ++at;
        }
    }
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t nl = t.find('\n', at);
        out.emplace_back(diagram_trim(std::string_view(t).substr(at, nl == std::string::npos ? std::string::npos : nl - at)));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return out;
}

std::vector<std::string> diagram_wrap(const std::vector<std::string>& lines, float most, float size, const DiagramFont& font) {
    std::vector<std::string> out;
    for (const std::string& l : lines) {
        if (font.width(l, size) <= most) {
            out.push_back(l);
            continue;
        }
        std::string row;
        size_t at = 0;
        while (at < l.size()) {
            size_t sp = l.find(' ', at);
            if (sp == std::string::npos) sp = l.size();
            const std::string word = l.substr(at, sp - at);
            at = sp + 1;
            const std::string with = row.empty() ? word : row + " " + word;
            if (font.width(with, size) <= most) {
                row = with;
                continue;
            }
            if (!row.empty()) out.push_back(row), row.clear();
            // A word longer than the line: broken where it must be.
            std::string part;
            const u8* p = reinterpret_cast<const u8*>(word.data());
            for (size_t i = 0; i < word.size();) {
                const Decoded d = decode_utf8(p + i, word.size() - i);
                const size_t len = d.len ? d.len : 1;
                const std::string next = part + word.substr(i, len);
                if (!part.empty() && font.width(next, size) > most) out.push_back(part), part = word.substr(i, len);
                else part = next;
                i += len;
            }
            row = part;
        }
        out.push_back(row);
    }
    // Bold and italic carried over the breaks: each row closes what is open
    // at its end, and the next opens it again.
    bool bold = false, italic = false;
    for (std::string& row : out) {
        const std::string open = std::string(bold ? "\x02" : "") + (italic ? "\x03" : "");
        for (const char c : row) {
            if (c == '\x02') bold = !bold;
            if (c == '\x03') italic = !italic;
        }
        row = open + row + (bold ? "\x02" : "") + (italic ? "\x03" : "");
    }
    return out;
}

// ---------------------------------------------------------------- shapes

void diagram_shape(u8 shape, float x, float y, float w, float h, float u, RasterPath* out) {
    auto poly = [&](std::initializer_list<RasterPoint> pts) { out->polygon(pts.begin(), pts.size()); };
    switch (shape) {
        case kShapeRound: out->rect(x, y, w, h, std::min(h / 2, 10 * u)); break;
        case kShapeState: out->rect(x, y, w, h, std::min(h / 2, 9 * u)); break;
        case kShapeStadium: out->rect(x, y, w, h, h / 2); break;
        case kShapeSubroutine:
        case kShapeBar: out->rect(x, y, w, h, shape == kShapeBar ? std::min(w, h) / 2 : 0); break;
        case kShapeNote: out->rect(x, y, w, h, 2 * u); break;
        case kShapeCircle:
        case kShapeDoubleCircle:
        case kShapeStart:
        case kShapeEnd: out->ellipse(x + w / 2, y + h / 2, w / 2, h / 2); break;
        case kShapeDiamond: poly({{x + w / 2, y}, {x + w, y + h / 2}, {x + w / 2, y + h}, {x, y + h / 2}}); break;
        case kShapeHexagon: {
            const float i = std::min(h / 4, w / 3);
            poly({{x + i, y}, {x + w - i, y}, {x + w, y + h / 2}, {x + w - i, y + h}, {x + i, y + h}, {x, y + h / 2}});
            break;
        }
        case kShapeLeanRight:
        case kShapeLeanLeft:
        case kShapeTrapezoid:
        case kShapeTrapezoidAlt: {
            const float s = std::min(h * 0.35f, w / 4);
            if (shape == kShapeLeanRight) poly({{x + s, y}, {x + w, y}, {x + w - s, y + h}, {x, y + h}});
            else if (shape == kShapeLeanLeft) poly({{x, y}, {x + w - s, y}, {x + w, y + h}, {x + s, y + h}});
            else if (shape == kShapeTrapezoid) poly({{x + s, y}, {x + w - s, y}, {x + w, y + h}, {x, y + h}});
            else poly({{x, y}, {x + w, y}, {x + w - s, y + h}, {x + s, y + h}});
            break;
        }
        case kShapeAsymmetric: poly({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, {x + std::min(h / 4, w / 4), y + h / 2}}); break;
        case kShapeCylinder: {
            const float rx = w / 2, ry = std::min(h / 4, rx / (2.5f + w / (50 * u)));
            out->arc(x + rx, y + ry, rx, ry, 3.14159265f, 6.28318531f);
            out->line_to(x + w, y + h - ry);
            out->arc(x + rx, y + h - ry, rx, ry, 0, 3.14159265f);
            out->close();
            break;
        }
        default: out->rect(x, y, w, h, std::min(h / 2, 3 * u)); break;
    }
}

bool diagram_shape_extra(u8 shape, float x, float y, float w, float h, float u, RasterPath* out) {
    switch (shape) {
        case kShapeSubroutine: {
            const float i = std::min(8 * u, w / 6);
            out->move_to(x + i, y), out->line_to(x + i, y + h);
            out->move_to(x + w - i, y), out->line_to(x + w - i, y + h);
            return true;
        }
        case kShapeCylinder: {
            const float rx = w / 2, ry = std::min(h / 4, rx / (2.5f + w / (50 * u)));
            out->arc(x + rx, y + ry, rx, ry, 3.14159265f, 0);
            return true;
        }
        case kShapeDoubleCircle: {
            const float i = std::min(4 * u, w / 6);
            out->ellipse(x + w / 2, y + h / 2, w / 2 - i, h / 2 - i);
            return true;
        }
        default: return false;
    }
}

DiagramVec diagram_shape_size(u8 shape, float tw, float th, float px, float py) {
    switch (shape) {
        case kShapeCircle:
        case kShapeDoubleCircle: {
            const float d = std::sqrt(tw * tw + th * th) + py * 1.2f + (shape == kShapeDoubleCircle ? py : 0);
            return {d, d};
        }
        case kShapeDiamond: {
            // The text's box (padded a little) within the diamond: a diamond
            // with half-diagonals a and b holds a box w by h when w / 2a +
            // h / 2b <= 1; this share of it goes across.
            // Never taller than wide: text taller than it is wide (blank
            // lines, say) is in a square one.
            const float bw = tw + px * 0.8f, bh = th + py * 0.8f, t = 0.38f;
            if (bh / t > bw / (1 - t)) return {bw + bh, bw + bh};
            return {bw / (1 - t), bh / t};
        }
        case kShapeStadium: {
            const float h = th + 2 * py;
            return {tw + 2 * px + h * 0.3f, h};
        }
        case kShapeHexagon: {
            const float h = th + 2 * py;
            return {tw + 2 * px + h * 0.5f, h};
        }
        case kShapeLeanRight:
        case kShapeLeanLeft:
        case kShapeTrapezoid:
        case kShapeTrapezoidAlt: {
            const float h = th + 2 * py;
            return {tw + 2 * px + h * 0.7f, h};
        }
        case kShapeAsymmetric: {
            const float h = th + 2 * py;
            return {tw + 2 * px + h * 0.25f, h};
        }
        case kShapeSubroutine: return {tw + 3 * px, th + 2 * py};
        case kShapeCylinder: {
            const float w = tw + 2 * px;
            return {w, th + 2 * py + w / 2 / (2.5f + w / (50 * px / 15)) * 1.6f};
        }
        default: return {tw + 2 * px, th + 2 * py};
    }
}

DiagramVec diagram_shape_hit(u8 shape, float x, float y, float w, float h, float u, DiagramVec from, DiagramVec to) {
    RasterPath p;
    diagram_shape(shape, x, y, w, h, u, &p);
    const RasterLines l = raster_flatten(p, 1, 0, 0, 0.02f);
    double best = 2;
    DiagramVec hit = to;
    const double dx = to.x - from.x, dy = to.y - from.y;
    for (size_t f = 0; f < l.figures(); ++f) {
        const size_t s = l.starts[f], e = l.end_of(f);
        for (size_t i = s; i < e; ++i) {
            const RasterPoint a = l.pts[i], b = l.pts[i + 1 < e ? i + 1 : s];
            const double ex = b.x - a.x, ey = b.y - a.y;
            const double den = dx * ey - dy * ex;
            if (std::fabs(den) < 1e-12) continue;
            const double t = ((a.x - from.x) * ey - (a.y - from.y) * ex) / den;
            const double v = ((a.x - from.x) * dy - (a.y - from.y) * dx) / den;
            if (t >= 0 && t <= 1 && v >= 0 && v <= 1 && t < best) {
                best = t;
                hit = {static_cast<float>(from.x + dx * t), static_cast<float>(from.y + dy * t)};
            }
        }
    }
    return hit;
}

// ---------------------------------------------------------------- lines

float diagram_head_length(u8 head, float size) {
    switch (head) {
        case kHeadArrow: return size * 0.85f;
        case kHeadCircle: return size * 0.42f;
        case kHeadCross: return size * 0.42f;
        default: return 0;
    }
}

float diagram_head(Drawing& d, u8 head, DiagramVec tip, DiagramVec dir, float size, DiagramColour c, float width) {
    const DiagramVec n{-dir.y, dir.x};
    switch (head) {
        case kHeadArrow: {
            const float l = size, half = size * 0.42f;
            const RasterPoint pts[] = {{tip.x, tip.y},
                                       {tip.x - dir.x * l + n.x * half, tip.y - dir.y * l + n.y * half},
                                       {tip.x - dir.x * l * 0.82f, tip.y - dir.y * l * 0.82f},
                                       {tip.x - dir.x * l - n.x * half, tip.y - dir.y * l - n.y * half}};
            RasterPath p;
            p.polygon(pts, 4);
            d.shape(std::move(p), c, c, width * 0.6f);
            break;
        }
        case kHeadOpen: {
            const float l = size, half = size * 0.42f;
            RasterPath p;
            p.move_to(tip.x - dir.x * l + n.x * half, tip.y - dir.y * l + n.y * half);
            p.line_to(tip.x, tip.y);
            p.line_to(tip.x - dir.x * l - n.x * half, tip.y - dir.y * l - n.y * half);
            d.shape(std::move(p), {}, c, width, {}, RasterStroke::kRound);
            break;
        }
        case kHeadCircle: {
            const float r = size * 0.32f;
            RasterPath p;
            p.ellipse(tip.x - dir.x * r, tip.y - dir.y * r, r, r);
            d.shape(std::move(p), c);
            break;
        }
        case kHeadCross: {
            const float r = size * 0.3f;
            const DiagramVec m{tip.x - dir.x * r * 1.4f, tip.y - dir.y * r * 1.4f};
            RasterPath p;
            p.move_to(m.x - (dir.x + n.x) * r, m.y - (dir.y + n.y) * r), p.line_to(m.x + (dir.x + n.x) * r, m.y + (dir.y + n.y) * r);
            p.move_to(m.x - (dir.x - n.x) * r, m.y - (dir.y - n.y) * r), p.line_to(m.x + (dir.x - n.x) * r, m.y + (dir.y - n.y) * r);
            d.shape(std::move(p), {}, c, width * 1.2f, {}, RasterStroke::kRound);
            break;
        }
        default: break;
    }
    return diagram_head_length(head, size);
}

void diagram_curve(const std::vector<DiagramVec>& pts, DiagramVec a0, DiagramVec a1, RasterPath* out, DiagramVec mid_axis) {
    const size_t n = pts.size();
    if (n == 0) return;
    out->move_to(pts[0].x, pts[0].y);
    if (n == 1) return;
    auto along = [](DiagramVec a, DiagramVec d) { return a.x * d.x + a.y * d.y; };
    std::vector<DiagramVec> t(n);
    for (size_t i = 0; i < n; ++i) {
        const DiagramVec prev = pts[i ? i - 1 : 0], next = pts[i + 1 < n ? i + 1 : n - 1];
        t[i] = {(next.x - prev.x) / (i == 0 || i + 1 == n ? 1.0f : 2.0f), (next.y - prev.y) / (i == 0 || i + 1 == n ? 1.0f : 2.0f)};
    }
    // The ends leave and arrive along their axes, as far along as the
    // next point is; a point between keeps its curve within the parts
    // either side of it (along the axis), so that it never doubles back.
    const bool axis = a0.x != 0 || a0.y != 0;
    if (axis) {
        // (an end leaving across the axis, from a diamond's corner, leaves
        // as far as half the way to the next point, even when that is not
        // the way it leaves)
        const DiagramVec m = mid_axis.x != 0 || mid_axis.y != 0 ? mid_axis : a0;
        auto leave = [&](DiagramVec a, DiagramVec d) {
            const float on = along(d, a);
            if (a.x == m.x && a.y == m.y) return on;
            return std::max(on, 0.5f * std::sqrt(d.x * d.x + d.y * d.y));
        };
        const float d0 = leave(a0, {pts[1].x - pts[0].x, pts[1].y - pts[0].y});
        t[0] = {a0.x * d0 * 1.5f, a0.y * d0 * 1.5f};
        const float d1 = leave(a1, {pts[n - 1].x - pts[n - 2].x, pts[n - 1].y - pts[n - 2].y});
        t[n - 1] = {a1.x * d1 * 1.5f, a1.y * d1 * 1.5f};
        a0 = m;
        const DiagramVec across{a0.y, -a0.x};
        for (size_t i = 1; i + 1 < n; ++i) {
            // (on a straight run along the axis: along it)
            const float before = std::fabs(along({pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y}, across));
            const float after = std::fabs(along({pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y}, across));
            if (before < 0.5f || after < 0.5f) {
                const float d = (std::fabs(along({pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y}, a0)) + std::fabs(along({pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y}, a0))) / 2;
                t[i] = {a0.x * d, a0.y * d};
            }
            const float gap = std::min(std::fabs(along({pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y}, a0)),
                                       std::fabs(along({pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y}, a0)));
            const float reach = std::fabs(along(t[i], a0)) / 3;
            if (reach > gap * 0.5f && reach > 0) {
                const float k = gap * 0.5f / reach;
                t[i] = {t[i].x * k, t[i].y * k};
            }
        }
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        const DiagramVec p = pts[i], q = pts[i + 1];
        out->cubic_to(p.x + t[i].x / 3, p.y + t[i].y / 3, q.x - t[i + 1].x / 3, q.y - t[i + 1].y / 3, q.x, q.y);
    }
}

std::vector<RasterPoint> diagram_sample(const RasterPath& p, int n) {
    const RasterLines l = raster_flatten(p, 1, 0, 0, 0.1f);
    std::vector<RasterPoint> out;
    if (l.pts.empty() || n < 2) return out;
    std::vector<double> at(l.pts.size(), 0);
    for (size_t i = 1; i < l.pts.size(); ++i) {
        const double dx = l.pts[i].x - l.pts[i - 1].x, dy = l.pts[i].y - l.pts[i - 1].y;
        at[i] = at[i - 1] + std::sqrt(dx * dx + dy * dy);  // (not hypot: the system's may differ from one to another)
    }
    size_t j = 0;
    for (int k = 0; k < n; ++k) {
        const double want = at.back() * k / (n - 1);
        while (j + 1 < at.size() && at[j + 1] < want) ++j;
        if (j + 1 >= at.size()) {
            out.push_back(l.pts.back());
            continue;
        }
        const double seg = at[j + 1] - at[j], t = seg > 0 ? (want - at[j]) / seg : 0;
        out.push_back({static_cast<float>(l.pts[j].x + (l.pts[j + 1].x - l.pts[j].x) * t), static_cast<float>(l.pts[j].y + (l.pts[j + 1].y - l.pts[j].y) * t)});
    }
    return out;
}

// ---------------------------------------------------------------- the kinds

u8 diagram_make(std::string_view source, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    *out = Drawing{};
    std::string title;
    const std::vector<std::string> st = diagram_statements(source, false, &title);
    if (st.empty()) {
        *err = "empty";
        return kDiagramError;
    }
    const std::string kind = diagram_lower(st[0].substr(0, st[0].find(' ')));
    bool ok = false;
    if (kind == "graph" || kind == "flowchart" || kind == "flowchart-elk") ok = flowchart_layout(diagram_statements(source, true), false, style, out, err, geo);
    else if (kind == "statediagram" || kind == "statediagram-v2") ok = flowchart_layout(st, true, style, out, err, geo);
    else if (kind == "sequencediagram") ok = sequence_layout(st, style, out, err, geo);
    else if (kind == "pie") ok = pie_layout(st, style, out, err, geo);
    else if (kind == "classdiagram" || kind == "classdiagram-v2") ok = classdiagram_layout(st, style, out, err, geo);
    else if (kind == "erdiagram") ok = erdiagram_layout(st, style, out, err, geo);
    else if (kind == "gantt") ok = gantt_layout(diagram_lines(source), style, out, err, geo);
    else if (kind == "timeline") ok = timeline_layout(diagram_lines(source), style, out, err, geo);
    else if (kind == "mindmap") ok = mindmap_layout(diagram_lines(source), style, out, err, geo);
    else {
        *err = st[0].substr(0, st[0].find(' ')) + " is not drawn yet: shown as text";
        return kDiagramNone;
    }
    if (!ok) return kDiagramError;
    // A title from the front matter, over it in the middle.
    if (!title.empty() && style.font) {
        const float size = style.size * 1.15f, tw = style.font->width(title, size, true);
        const float band = style.font->line(size) + style.unit() * 10;
        const float shift_x = tw > out->w ? (tw - out->w) / 2 : 0;
        for (DiagramItem& it : out->items) {
            for (size_t i = 0; i < it.path.pts.size(); i += 2) it.path.pts[i] += shift_x, it.path.pts[i + 1] += band;
            it.x += shift_x, it.y += band;
        }
        if (geo) {
            for (DiagramGeometry::Box& b : geo->boxes) b.x += shift_x, b.y += band;
            for (DiagramGeometry::Line& l : geo->lines)
                for (RasterPoint& p : l.pts) p.x += shift_x, p.y += band;
        }
        out->w = std::max(out->w, tw);
        out->h += band;
        out->text(title, out->w / 2, style.unit() * 4 + style.font->ascent(size), size, style.palette.of(kMdText), *style.font, 1, true);
    }
    return kDiagramDrawn;
}

}  // namespace blit
