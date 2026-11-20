// Pie charts (pie): a disc cut into wedges, each a slice's share of the
// whole, its percentage on it where it fits; a legend beside it (under it,
// if that fits the room better) with each slice's colour, name (and value,
// with showData) and percentage; the title over it all.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "diagram.hpp"

namespace blit {

namespace {

struct Slice {
    std::string name;
    double value = 0;
    DiagramColour colour;
};

// The text that reads on a colour: dark on a light one, light on a dark one.
DiagramColour text_on(DiagramColour c) {
    const float lum = c.r * 0.3f + c.g * 0.59f + c.b * 0.11f;
    return lum > 0.55f ? DiagramColour{0.1f, 0.1f, 0.12f, 1} : DiagramColour{0.97f, 0.97f, 0.98f, 1};
}

std::string percent(double share) {
    char buf[32];
    const double pct = share * 100;
    if (std::fabs(pct - std::round(pct)) < 0.05) std::snprintf(buf, sizeof buf, "%.0f%%", pct);
    else std::snprintf(buf, sizeof buf, "%.1f%%", pct);
    return buf;
}

std::string value_text(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

// Colours for n slices: the theme's own colours that are not too like one
// another, the background or grey; then the same again nearer the text's
// colour (lighter in a dark theme), then again nearer the background. (Made
// quieter each time, the repeats were muddy: greys and navies alike.)
std::vector<DiagramColour> slice_colours(const DiagramPalette& p, size_t n) {
    static const u8 kInks[] = {kMdSlice0,     kMdSlice1,      kMdSlice2,       kMdSlice3,       kMdHeading1,     kMdHeading2, kMdHeading3, kMdAccent,
                               kMdLink,       kMdCodeString,  kMdCodeType,     kMdCodeFunction, kMdCodeKeyword,  kMdCodeConstant, kMdError,  kMdLabel};
    std::vector<DiagramColour> got;
    auto far = [](DiagramColour a, DiagramColour b, float most) {
        const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
        return dr * dr + dg * dg + db * db > most * most;
    };
    const DiagramColour bg{p.bg[0], p.bg[1], p.bg[2], 1};
    for (const u8 ink : kInks) {
        const DiagramColour c = p.of(ink);
        const float hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
        if (hi - lo < 0.12f || !far(c, bg, 0.25f)) continue;
        bool apart = true;
        for (const DiagramColour& o : got) apart = apart && far(c, o, 0.16f);
        if (apart) got.push_back(c);
    }
    if (got.empty()) got.push_back(p.of(kMdText));
    const DiagramColour fg = p.of(kMdText);
    std::vector<DiagramColour> out;
    for (size_t k = 0; k < n; ++k) {
        const DiagramColour c = got[k % got.size()];
        const size_t round = (k / got.size()) % 3;
        const DiagramColour to = round == 1 ? fg : bg;
        const float t = round == 0 ? 0 : round == 1 ? 0.45f : 0.5f;
        out.push_back({c.r + (to.r - c.r) * t, c.g + (to.g - c.g) * t, c.b + (to.b - c.b) * t, 1});
    }
    return out;
}

}  // namespace

bool pie_layout(const std::vector<std::string>& st, const DiagramStyle& style, Drawing* out, std::string* err, DiagramGeometry* geo) {
    if (!style.font) {
        *err = "no font";
        return false;
    }
    const DiagramFont& font = *style.font;
    std::string title;
    std::vector<Slice> slices;
    const std::string head = diagram_lower(st[0]);
    bool show = head.find("showdata") != std::string::npos;
    if (const size_t t = head.find("title"); t != std::string::npos) title = std::string(diagram_trim(std::string_view(st[0]).substr(t + 5)));  // (pie title Pets)
    for (size_t k = 1; k < st.size(); ++k) {
        const std::string_view s = st[k];
        const std::string l = diagram_lower(s);
        if (l.starts_with("title ") || l == "title") {
            title = std::string(diagram_trim(s.substr(5)));
            continue;
        }
        if (l == "showdata") {
            show = true;
            continue;
        }
        if (l.starts_with("acctitle") || l.starts_with("accdescr")) continue;
        const size_t colon = s.rfind(':');
        if (colon == std::string_view::npos) {
            *err = "line " + std::to_string(k + 1) + ": a slice is \"name\" : number";
            return false;
        }
        std::string_view name = diagram_trim(s.substr(0, colon));
        if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front()) name = name.substr(1, name.size() - 2);
        const std::string num(diagram_trim(s.substr(colon + 1)));
        char* end = nullptr;
        const double v = std::strtod(num.c_str(), &end);
        if (num.empty() || !end || *end || !(v >= 0 && v < 1e300)) {  // (nan and inf too)
            *err = "line " + std::to_string(k + 1) + ": a slice is \"name\" : a number, not less than 0";
            return false;
        }
        Slice sl;
        const std::vector<std::string> lines = diagram_label(name);
        for (const std::string& part : lines) sl.name += (sl.name.empty() ? "" : " ") + part;
        sl.value = v;
        slices.push_back(std::move(sl));
    }
    double total = 0;
    for (const Slice& s : slices) total += s.value;
    if (slices.empty() || !(total > 0) || !std::isfinite(total)) {
        *err = "no slices";
        return false;
    }
    const DiagramPalette& p = style.palette;
    const std::vector<DiagramColour> colours = slice_colours(p, slices.size());
    for (size_t k = 0; k < slices.size(); ++k) slices[k].colour = colours[k];

    const float u = style.unit(), size = style.size, line = font.line(size);
    const float edge = 8 * u, sq = 13 * u, row = std::max(line + 6 * u, 20 * u);
    const float title_size = size * 1.15f;
    // The legend: a row each, its colour, its name (and value), its share.
    std::vector<std::string> names, shares;
    float name_w = 0, share_w = 0;
    for (const Slice& s : slices) {
        names.push_back(show ? s.name + " [" + value_text(s.value) + "]" : s.name);
        shares.push_back(percent(s.value / total));
        name_w = std::max(name_w, font.width(names.back(), size));
        share_w = std::max(share_w, font.width(shares.back(), size));
    }
    const float legend_w = sq + 9 * u + name_w + 18 * u + share_w;
    const float legend_h = static_cast<float>(slices.size()) * row;
    // Beside the pie (as tall as the legend, within reason), or under it if
    // that fits the room better.
    const float beside_r = std::clamp(legend_h / 2 + 10 * u, 110 * u, 180 * u), under_r = 110 * u;
    const float beside_w = 2 * beside_r + 36 * u + legend_w + 2 * edge;
    const float under_w = std::max(2 * under_r, legend_w) + 2 * edge;
    const bool under = beside_w > style.room && under_w < beside_w;
    const float r = under ? under_r : beside_r;
    float w = under ? under_w : beside_w;
    // The title, wrapped to the room (or to the pie and its legend, if they are wider).
    std::vector<std::string> title_lines;
    float title_w = 0;
    if (!title.empty()) {
        title_lines = diagram_wrap({title}, std::max(w, style.room) * 0.95f - 2 * edge, title_size, font);
        for (const std::string& l : title_lines) title_w = std::max(title_w, font.width(l, title_size, true));
    }
    const float title_h = title.empty() ? 0 : static_cast<float>(title_lines.size()) * font.line(title_size) + 14 * u;
    w = std::max(w, title_w + 2 * edge);
    const float body_h = under ? 2 * r + 24 * u + legend_h : std::max(2 * r, legend_h);
    const float h = title_h + body_h + 2 * edge;
    const float cx = under ? w / 2 : (w - (2 * r + 36 * u + legend_w)) / 2 + r, cy = edge + title_h + (under ? r : body_h / 2);
    const float lx = under ? w / 2 - legend_w / 2 : cx + r + 36 * u;
    const float ly = under ? cy + r + 24 * u : cy - legend_h / 2;
    out->w = w, out->h = h;

    const DiagramColour text = p.of(kMdText), muted = p.tone(kMdText, 0.65f);
    const DiagramColour bg{p.bg[0], p.bg[1], p.bg[2], 1};
    auto text_at = [&](const std::string& s, float x, float top, float sz, DiagramColour c, int align, bool bold) {
        const float ln = font.line(sz);
        out->text(s, x, top + (ln - font.ascent(sz) - font.descent(sz)) / 2 + font.ascent(sz), sz, c, font, align, bold);
    };
    for (size_t k = 0; k < title_lines.size(); ++k) text_at(title_lines[k], w / 2, edge + static_cast<float>(k) * font.line(title_size), title_size, text, 1, true);
    if (geo && !title.empty()) geo->boxes.push_back({"node", "title", w / 2 - title_w / 2, edge, title_w, title_h - 14 * u});
    // The wedges, from the top round clockwise; a line of the background
    // between them. (Each line stops short of the middle, where the wedges
    // beside it are narrower than a few lines: many lines met there in a
    // blot of the background.)
    const double kPi = 3.14159265358979323846;
    double at = -kPi / 2;
    int drawn = 0;
    for (const Slice& s : slices)
        if (s.value > 0) ++drawn;
    std::vector<double> starts, sweeps;
    for (const Slice& s : slices) {
        if (!(s.value > 0)) continue;
        const double sweep = s.value / total * 2 * kPi;
        RasterPath wedge;
        if (drawn == 1) {
            wedge.ellipse(cx, cy, r, r);
        } else {
            wedge.move_to(cx, cy);
            wedge.arc(cx, cy, r, r, static_cast<float>(at), static_cast<float>(at + sweep));
            wedge.close();
        }
        out->shape(std::move(wedge), s.colour);
        starts.push_back(at), sweeps.push_back(sweep);
        at += sweep;
    }
    const float sep = 2 * u;
    for (size_t k = 0; k < starts.size() && drawn > 1; ++k) {
        const double narrow = std::min({sweeps[k], sweeps[(k + starts.size() - 1) % starts.size()], kPi});
        const float from = std::min(r * 0.85f, static_cast<float>(1.6 * sep / narrow));
        double sn = 0, cs = 1;
        raster_sincos(starts[k], &sn, &cs);
        RasterPath cut;
        cut.move_to(cx + from * static_cast<float>(cs), cy + from * static_cast<float>(sn));
        cut.line_to(cx + r * static_cast<float>(cs), cy + r * static_cast<float>(sn));
        out->shape(std::move(cut), {}, bg, sep);
    }
    // Their shares on them, where they fit.
    at = -kPi / 2;
    for (size_t k = 0; k < slices.size(); ++k) {
        const Slice& s = slices[k];
        if (!(s.value > 0)) continue;
        const double sweep = s.value / total * 2 * kPi, mid = at + sweep / 2;
        at += sweep;
        const std::string& pct = shares[k];
        const float tw = font.width(pct, size, true), th = line;
        const float lr = drawn == 1 ? 0 : r * 0.66f;
        double sn = 0, cs = 1;
        raster_sincos(mid, &sn, &cs);
        const float tx = cx + lr * static_cast<float>(cs), ty = cy + lr * static_cast<float>(sn);
        // (the text's box within the wedge: its arc there long enough, and its corners inside the disc)
        if (drawn > 1 && (sweep * lr < std::hypot(tw, th) + 4 * u || sweep < 0.3)) continue;
        if (std::hypot(std::fabs(tx - cx) + tw / 2, std::fabs(ty - cy) + th / 2) > r - 2 * u) continue;
        text_at(pct, tx, ty - th / 2, size, text_on(s.colour), 1, true);
        if (geo) geo->boxes.push_back({"label", "share:" + std::to_string(k + 1), tx - tw / 2, ty - th / 2, tw, th});
    }
    if (geo) geo->boxes.push_back({"cluster", "pie", cx - r, cy - r, 2 * r, 2 * r});
    // The legend.
    for (size_t k = 0; k < slices.size(); ++k) {
        const float y = ly + static_cast<float>(k) * row;
        RasterPath box;
        box.rect(lx, y + (row - sq) / 2, sq, sq, 2.5f * u);
        out->shape(std::move(box), slices[k].colour);
        text_at(names[k], lx + sq + 9 * u, y + (row - line) / 2, size, text, 0, false);
        text_at(shares[k], lx + legend_w, y + (row - line) / 2, size, muted, 2, false);
        if (geo) geo->boxes.push_back({"node", "legend:" + std::to_string(k + 1), lx, y + u, legend_w, row - 2 * u});
    }
    return true;
}

}  // namespace blit
