#include "nbview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "base64.hpp"
#include "json.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

// An output's rows at most (as Jupyter scrolls a long one): the rest is a
// line that says how much more there is.
constexpr u32 kMostRows = 60;
constexpr u32 kTab = 8;  // a terminal's tab stops

// ---------------------------------------------------------------- text

u32 cols_of(std::string_view s) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        w += cp_width(d.cp);
        i += d.len;
    }
    return w;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A JSON string, or (as Jupyter writes a long one) the lines of one.
std::string text_of(json::Value v) {
    if (v.is_string()) return std::string(v.str());
    std::string out;
    for (const json::Value l : v) out += l.str();
    return out;
}

// JSON as a person reads it: two spaces an indent, a member a line.
void indented(std::string& out, json::Value v, int depth) {
    auto line = [&](int d) {
        out += '\n';
        out.append(static_cast<size_t>(d) * 2, ' ');
    };
    if ((v.is_array() || v.is_object()) && v.size()) {
        const bool object = v.is_object();
        out += object ? '{' : '[';
        bool first = true;
        for (const json::Value e : v) {
            if (!first) out += ',';
            first = false;
            line(depth + 1);
            if (object) {
                json::quote(out, e.key());
                out += ": ";
            }
            indented(out, e, depth + 1);
        }
        line(depth);
        out += object ? '}' : ']';
    } else {
        json::dump(out, v);
    }
}

// Text as Markdown that says just it, but for its HTML entities (&amp;,
// &#39;), which Markdown reads as HTML would.
std::string html_markdown(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '&') {  // (an entity whole: &name; or &#123;)
            size_t e = i + 1;
            while (e < text.size() && e - i < 12 && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '#')) ++e;
            if (e < text.size() && text[e] == ';' && e > i + 1) {
                out += text.substr(i, e + 1 - i);
                i = e;
                continue;
            }
            out += "\\&";
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            out += ' ';
        } else {
            out += md_escape(std::string_view(&c, 1));
        }
    }
    return out;
}

// ---------------------------------------------------------------- HTML

// The value of a tag's attribute, or "".
std::string attribute(std::string_view tag, std::string_view name) {
    const std::string low = lower(tag);
    for (size_t at = 0; (at = low.find(name, at)) != std::string::npos; at += name.size()) {
        if (at == 0 || !std::isspace(static_cast<unsigned char>(low[at - 1]))) continue;
        size_t k = at + name.size();
        while (k < low.size() && low[k] == ' ') ++k;
        if (k >= low.size() || low[k] != '=') continue;
        ++k;
        while (k < low.size() && low[k] == ' ') ++k;
        if (k < low.size() && (low[k] == '"' || low[k] == '\'')) {
            const size_t end = low.find(low[k], k + 1);
            return std::string(tag.substr(k + 1, (end == std::string::npos ? low.size() : end) - k - 1));
        }
        size_t end = k;
        while (end < low.size() && !std::isspace(static_cast<unsigned char>(low[end])) && low[end] != '>') ++end;
        return std::string(tag.substr(k, end - k));
    }
    return {};
}

// How a tag says its text is aligned (style="text-align: right", align=...): -1 left, 0 centre, 1 right, 2 not said.
int align_of(std::string_view tag) {
    std::string a = lower(attribute(tag, "align"));
    const std::string style = lower(attribute(tag, "style"));
    if (const size_t at = style.find("text-align"); at != std::string::npos) {
        const size_t colon = style.find(':', at);
        if (colon != std::string::npos) a = std::string(trim(style.substr(colon + 1, style.find(';', colon) - colon - 1)));
    }
    if (a == "left") return -1;
    if (a == "center" || a == "centre") return 0;
    if (a == "right") return 1;
    return 2;
}

// HTML without what is not shown: comments, scripts, styles.
std::string shown_html(std::string html) {
    auto cut = [&](std::string_view open, std::string_view close) {
        for (size_t c; (c = lower(html).find(open)) != std::string::npos;) {
            const size_t e = lower(html).find(close, c + open.size());
            html.erase(c, e == std::string::npos ? std::string::npos : e + close.size() - c);
        }
    };
    cut("<!--", "-->");
    cut("<script", "</script>");
    cut("<style", "</style>");
    return html;
}

// HTML's text as Markdown paragraphs: each block (a paragraph, a line
// broken, a heading, a list's item) one of its own, the tags gone.
std::string html_paragraphs(std::string_view html) {
    std::string out, block;
    auto end_block = [&] {
        const std::string_view t = trim(block);
        if (!t.empty()) out += (out.empty() ? "" : "\n\n") + html_markdown(t);
        block.clear();
    };
    for (size_t i = 0; i < html.size();) {
        if (html[i] != '<') {
            block += html[i++];
            continue;
        }
        const size_t close = html.find('>', i);
        if (close == std::string_view::npos) break;
        std::string name = lower(html.substr(i + 1, close - i - 1));
        if (name.starts_with('/')) name.erase(0, 1);
        name = name.substr(0, name.find_first_of(" \t\n/"));
        static const char* const kBlocks[] = {"p", "br", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "pre", "ul", "ol", "table", "caption"};
        if (std::ranges::any_of(kBlocks, [&](const char* b) { return name == b; })) end_block();
        else if (name == "td" || name == "th") block += ' ';
        i = close + 1;
    }
    end_block();
    return out;
}

// A <table>'s cells (Markdown), its header's rows made one (each column's
// headings in turn), the index's cells (<th> in its body) in bold; and how
// each column is aligned (to the right, as Jupyter shows a table, unless
// it says otherwise). Cells over more than one column or row fill each.
bool html_table(std::string_view table, std::vector<std::vector<std::string>>* cells, std::vector<int>* align) {
    struct Cell {
        std::string text;
        bool head = false;
        int align = 2;
    };
    std::vector<std::vector<Cell>> rows;
    std::vector<bool> in_head;
    std::vector<u32> spanned;  // for each column, rows still taken by a cell above
    std::vector<Cell> spanning;
    const std::string low = lower(table);
    bool head = false;
    int row_align = 2;
    std::vector<Cell>* row = nullptr;
    for (size_t i = 0; i < table.size();) {
        const size_t open = low.find('<', i);
        if (open == std::string::npos) break;
        const size_t close = low.find('>', open);
        if (close == std::string::npos) break;
        const std::string_view tag = table.substr(open, close - open + 1);
        std::string name = low.substr(open + 1, close - open - 1);
        name = name.substr(0, name.find_first_of(" \t\n/>"));
        i = close + 1;
        if (name == "thead") head = true;
        else if (low.compare(open, 8, "</thead>") == 0 || name == "tbody") head = false;
        else if (name == "tr") {
            rows.emplace_back();
            in_head.push_back(head);
            row = &rows.back();
            row_align = align_of(tag);
        } else if ((name == "td" || name == "th") && row) {
            // Its text: to its end (or the next cell's start), without tags.
            size_t end = low.find(name == "td" ? "</td" : "</th", i);
            const size_t next = std::min({low.find("<td", i), low.find("<th", i), low.find("</tr", i)});
            end = std::min(end, next);
            if (end == std::string::npos) end = table.size();
            std::string text;
            for (size_t k = i; k < end; ++k) {
                if (table[k] == '<') {
                    const size_t gt = table.find('>', k);
                    if (gt == std::string_view::npos || gt >= end) break;
                    if (lower(table.substr(k, 3)) == "<br") text += ' ';
                    k = gt;
                    continue;
                }
                text += table[k];
            }
            i = end;
            Cell c{html_markdown(trim(text)), name == "th", align_of(tag) != 2 ? align_of(tag) : row_align};
            // (the columns cells above still take, filled)
            auto fill_spanned = [&] {
                while (row->size() < spanned.size() && spanned[row->size()] > 0) {
                    --spanned[row->size()];
                    Cell s = spanning[row->size()];
                    if (!in_head.back()) s.text.clear();
                    row->push_back(std::move(s));
                }
            };
            fill_spanned();
            const int across = std::clamp(std::atoi(attribute(tag, "colspan").c_str()), 1, 1000);
            const int down = std::clamp(std::atoi(attribute(tag, "rowspan").c_str()), 1, 100000);
            for (int k = 0; k < across; ++k) {
                Cell part = c;
                if (k > 0 && !in_head.back()) part.text.clear();  // (a heading over columns names each)
                if (row->size() >= spanned.size()) spanned.resize(row->size() + 1, 0), spanning.resize(row->size() + 1);
                spanned[row->size()] = static_cast<u32>(down - 1);
                spanning[row->size()] = part;
                row->push_back(std::move(part));
            }
            fill_spanned();
        }
        if (rows.size() > 100000) break;
    }
    if (rows.empty()) return false;
    // With no <thead>: the first rows that are all headings are the header
    // (none: the first row is).
    if (std::ranges::none_of(in_head, [](bool h) { return h; }))
        for (size_t r = 0; r < rows.size() && !rows[r].empty() && std::ranges::all_of(rows[r], [](const Cell& c) { return c.head; }) && r + 1 < rows.size(); ++r) in_head[r] = true;
    if (std::ranges::none_of(in_head, [](bool h) { return h; }) && rows.size() > 1) in_head[0] = true;
    size_t ncols = 0;
    for (const std::vector<Cell>& r : rows) ncols = std::max(ncols, r.size());
    if (ncols == 0) return false;
    std::vector<std::string> header(ncols);
    std::vector<int> col_align(ncols, 2);
    cells->clear();
    for (size_t r = 0; r < rows.size(); ++r) {
        if (in_head[r]) {
            for (size_t c = 0; c < rows[r].size(); ++c) {
                const std::string& t = rows[r][c].text;
                if (!t.empty() && header[c] != t && !std::string_view(header[c]).ends_with(t)) header[c] += (header[c].empty() ? "" : " ") + t;
                if (col_align[c] == 2 && rows[r][c].align != 2) col_align[c] = rows[r][c].align;
            }
            continue;
        }
        std::vector<std::string> out(ncols);
        for (size_t c = 0; c < rows[r].size(); ++c) {
            const Cell& cell = rows[r][c];
            out[c] = cell.head && !cell.text.empty() ? "**" + cell.text + "**" : cell.text;
        }
        cells->push_back(std::move(out));
    }
    cells->insert(cells->begin(), std::move(header));
    align->assign(ncols, 1);
    for (size_t c = 0; c < ncols; ++c)
        if (col_align[c] != 2) (*align)[c] = col_align[c];
    return true;
}

// ---------------------------------------------------------------- outputs

// The ink of a colour a terminal names (one of 256, or red-green-blue):
// the theme's own colour for its hue.
u8 ink_of_rgb(int r, int g, int b) {
    const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
    if (hi - lo < 40) return hi < 140 ? kMdMuted : kMdText;  // (a grey)
    double h = 0;
    const double d = hi - lo;
    if (hi == r) h = std::fmod((g - b) / d, 6.0);
    else if (hi == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
    static const u8 kHues[] = {kMdError, kMdAnsiYellow, kMdAnsiGreen, kMdAnsiCyan, kMdAnsiBlue, kMdAnsiMagenta};
    return kHues[static_cast<int>(std::lround(h / 60)) % 6];
}

u8 ink_of_index(int i) {
    static const u8 k16[8] = {kMdText, kMdError, kMdAnsiGreen, kMdAnsiYellow, kMdAnsiBlue, kMdAnsiMagenta, kMdAnsiCyan, kMdText};
    if (i == 8) return kMdMuted;  // (bright black: grey)
    if (i < 16) return k16[i % 8];
    if (i < 232) {
        i -= 16;
        auto level = [](int v) { return v ? 55 + v * 40 : 0; };
        return ink_of_rgb(level(i / 36), level(i / 6 % 6), level(i % 6));
    }
    const int grey = 8 + (i - 232) * 10;
    return ink_of_rgb(grey, grey, grey);
}

// Lines of runs at most `width` columns wide (a long line goes on in the
// next), tabs to a terminal's stops; and which of the text's lines each is of.
std::vector<std::vector<MdRun>> wrap_runs(const std::vector<MdRun>& runs, u32 width, std::vector<u32>* of) {
    std::vector<std::vector<MdRun>> lines(1);
    of->assign(1, 0);
    u32 col = 0, source = 0;
    auto add = [&](const MdRun& as, std::string_view text) {
        if (text.empty()) return;
        std::vector<MdRun>& line = lines.back();
        if (!line.empty() && line.back().ink == as.ink && line.back().style == as.style && line.back().tone == as.tone) line.back().text += text;
        else line.push_back({std::string(text), as.ink, as.style, as.tone});
    };
    for (const MdRun& run : runs) {
        const std::string_view s = run.text;
        const u8* p = reinterpret_cast<const u8*>(s.data());
        size_t start = 0;
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '\n') {
                add(run, s.substr(start, i - start));
                lines.emplace_back();
                of->push_back(++source);
                col = 0;
                start = ++i;
                continue;
            }
            if (s[i] == '\t') {
                add(run, s.substr(start, i - start));
                u32 to = (col / kTab + 1) * kTab;
                if (to > width) to = width;
                add(run, std::string(to > col ? to - col : 0, ' '));
                col = to;
                start = ++i;
                if (col >= width) lines.emplace_back(), of->push_back(source), col = 0;
                continue;
            }
            const Decoded d = decode_utf8(p + i, s.size() - i);
            const u32 w = cp_width(d.cp);
            if (col + w > width && col > 0) {
                add(run, s.substr(start, i - start));
                lines.emplace_back();
                of->push_back(source);
                col = 0;
                start = i;
            }
            col += w;
            i += d.len;
        }
        add(run, s.substr(start));
    }
    return lines;
}

// A terminal's text as it shows: a line written over (\r) shows what was
// written last; backspaces and the bell go.
std::string as_shown(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t at = 0; at <= text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(at, nl - at);
        while (line.ends_with('\r')) line.remove_suffix(1);
        if (const size_t cr = line.rfind('\r'); cr != std::string_view::npos) line.remove_prefix(cr + 1);
        for (const char c : line)
            if (c != '\b' && c != '\a') out += c;
        if (nl < text.size()) out += '\n';
        at = nl + 1;
    }
    return out;
}

// One output read: what it shows, by the richest kind of data in it that
// blit can draw.
NbOutput read_output(json::Value o, const NbKeepFn& keep) {
    NbOutput out;
    const std::string_view type = o["output_type"].str();
    if (type == "stream") {
        NbPart p;
        p.text = as_shown(text_of(o["text"]));
        if (o["name"].str() == "stderr") p.tone = kMdErrorTone;
        out.parts.push_back(std::move(p));
        return out;
    }
    if (type == "error") {
        NbPart p;
        for (const json::Value l : o["traceback"]) p.text += (p.text.empty() ? "" : "\n") + std::string(l.str());
        if (trim(p.text).empty()) {
            p.text = std::string(o["ename"].str()) + ": " + std::string(o["evalue"].str());
            p.ink = kMdError;
        }
        p.text = as_shown(p.text);
        p.tone = kMdErrorTone;
        out.parts.push_back(std::move(p));
        return out;
    }
    if (type != "execute_result" && type != "display_data") return out;
    if (type == "execute_result" && o["execution_count"].is_number()) out.count = o["execution_count"].integer();
    const json::Value data = o["data"], meta = o["metadata"];
    const std::string plain = text_of(data["text/plain"]);
    auto text_part = [&](std::string text, u8 ink = kMdText) {
        if (trim(text).empty()) return;
        NbPart p;
        p.text = std::move(text);
        p.ink = ink;
        out.parts.push_back(std::move(p));
    };
    auto html = [&](bool tables_only) {
        const std::string h = shown_html(text_of(data["text/html"]));
        const std::string low = lower(h);
        if (tables_only && low.find("<table") == std::string::npos) return false;
        std::vector<NbPart> parts;
        size_t at = 0;
        auto words = [&](std::string_view between) {
            std::string md = html_paragraphs(between);
            if (md.empty()) return;
            NbPart p;
            p.kind = NbPart::kMarkdown;
            p.text = std::move(md);
            parts.push_back(std::move(p));
        };
        for (size_t t; (t = low.find("<table", at)) != std::string::npos;) {
            size_t e = low.find("</table", t);
            if (e != std::string::npos) e = low.find('>', e);
            e = e == std::string::npos ? h.size() : e + 1;  // (no end: to the end)
            words(std::string_view(h).substr(at, t - at));
            NbPart p;
            p.kind = NbPart::kTable;
            if (html_table(std::string_view(h).substr(t, e - t), &p.cells, &p.align)) parts.push_back(std::move(p));
            at = e;
        }
        words(std::string_view(h).substr(at));
        if (parts.empty()) return false;
        for (NbPart& p : parts) out.parts.push_back(std::move(p));
        return true;
    };
    auto picture = [&](std::string_view mime, std::string_view kind) {
        const json::Value v = data[mime];
        if (!v.exists()) return false;
        const std::string text = text_of(v);
        const std::string path = keep ? keep(kind == "svg" ? text : from_base64(text), kind) : std::string();
        NbPart p;
        p.kind = NbPart::kPicture;
        p.src = path;
        p.want_w = static_cast<u32>(std::max(0.0, meta[mime]["width"].number()));
        p.want_h = static_cast<u32>(std::max(0.0, meta[mime]["height"].number()));
        p.text = trim(plain).empty() ? "[" + std::string(mime) + "]" : plain;  // (if it cannot be drawn)
        p.ink = trim(plain).empty() ? kMdMuted : kMdText;
        if (path.empty()) {
            p.kind = NbPart::kText;
            p.src.clear();
        }
        out.parts.push_back(std::move(p));
        return true;
    };
    if (html(true) || picture("image/png", "png") || picture("image/jpeg", "jpeg") || picture("image/svg+xml", "svg")) return out;
    if (const json::Value md = data["text/markdown"]; md.exists()) {
        NbPart p;
        p.kind = NbPart::kMarkdown;
        p.text = text_of(md);
        out.parts.push_back(std::move(p));
        return out;
    }
    if (data["text/html"].exists() && html(false)) return out;
    if (const json::Value latex = data["text/latex"]; latex.exists()) {
        text_part(text_of(latex));
        return out;
    }
    if (const json::Value js = data["application/json"]; js.exists()) {
        std::string s;
        indented(s, js, 0);
        text_part(std::move(s));
        return out;
    }
    if (data["text/plain"].exists()) {
        text_part(plain);
        return out;
    }
    std::string kinds;  // (nothing blit draws: what kinds of data it has)
    for (const json::Value m : data) kinds += (kinds.empty() ? "[" : ", ") + std::string(m.key());
    if (!kinds.empty()) text_part(kinds + "]", kMdMuted);
    return out;
}

// A cell's outputs (Jupyter's JSON list of them) read; text a stream
// writes in parts, one text.
std::vector<NbOutput> read_outputs(const std::string& outputs, const NbKeepFn& keep) {
    std::vector<NbOutput> out;
    json::Doc doc;
    if (!doc.parse(outputs)) return out;
    std::string_view stream;
    for (const json::Value o : doc.root()) {
        const bool more = o["output_type"].str() == "stream" && !stream.empty() && o["name"].str() == stream && !out.empty();
        NbOutput r = read_output(o, keep);
        if (more && !r.parts.empty()) {
            out.back().parts.back().text += r.parts.back().text;
            continue;
        }
        stream = o["output_type"].str() == "stream" ? o["name"].str() : std::string_view();
        if (!r.parts.empty()) out.push_back(std::move(r));
    }
    return out;
}

// ---------------------------------------------------------------- layout

struct Drawer {
    const NbLook& look;
    NbCache* cache;
    std::vector<MdRow>& rows;
    u32 gutter = 0;   // columns kept for the labels beside the cells (0: above them)
    u32 room = 80;    // and what is left for the cells

    MdPictureFn md_picture() const {
        return [this](const std::string& src, u32 r, u32 w, u32 h, MdPic* out) { return look.picture && look.picture(src, r, w, h, out) == kNbPictureReady; };
    }

    // Rows of text, cut after kMostRows with a line that says how many more there are.
    void text(std::vector<MdRow>& out, std::string_view s, u8 ink, u8 tone, bool stale) {
        std::string_view body = s;
        while (body.ends_with('\n')) body.remove_suffix(1);
        std::vector<MdRun> runs = nb_ansi(body, ink);
        if (stale)
            for (MdRun& r : runs) r.ink = kMdMuted;
        for (MdRun& r : runs)
            if (r.tone == kMdNoTone) r.tone = tone;
        std::vector<u32> of;
        const std::vector<std::vector<MdRun>> lines = wrap_runs(runs, room, &of);
        const u32 shown = static_cast<u32>(std::min<size_t>(lines.size(), kMostRows));
        for (u32 k = 0; k < shown; ++k) {
            MdRow r;
            r.runs = lines[k];
            if (tone) r.fill = tone, r.fill_from = 0, r.fill_to = room;
            out.push_back(std::move(r));
        }
        if (shown < lines.size()) {  // (how many of the text's own lines are not begun)
            const u32 more = of.back() - of[shown - 1];
            MdRow r;
            const std::string say = more ? "\xE2\x80\xA6 " + std::to_string(more) + (more == 1 ? " more line" : " more lines") : "\xE2\x80\xA6 more of this line";
            r.runs.push_back({say, kMdMuted, kMdItalic, tone});
            if (tone) r.fill = tone, r.fill_from = 0, r.fill_to = room;
            out.push_back(std::move(r));
        }
    }

    // Rows of something drawn (a table, Markdown), cut as text is (not
    // through a picture).
    static void cut(std::vector<MdRow>& part) {
        if (part.size() <= kMostRows + 1) return;
        size_t at = kMostRows;
        while (at > 0 && std::ranges::any_of(part[at].pics, [](const MdPic& p) { return p.part > 0; })) --at;
        const size_t more = part.size() - at;
        part.resize(at);
        MdRow r;
        r.runs.push_back({"\xE2\x80\xA6 " + std::to_string(more) + " more rows", kMdMuted, kMdItalic});
        part.push_back(std::move(r));
    }

    // A picture's rows, or what stands for it.
    void picture(std::vector<MdRow>& out, const NbPart& p, bool stale) {
        MdPic pic;
        const NbPicture got = look.picture ? look.picture(p.src, room, p.want_w, p.want_h, &pic) : kNbPictureFailed;
        if (got == kNbPictureWaiting) return;  // (drawn again once it is read)
        if (got == kNbPictureFailed || !pic.cols || !pic.rows) return text(out, p.text, p.ink, p.tone, stale);
        pic.src = p.src;
        for (u32 r = 0; r < pic.rows; ++r) {
            MdRow row;
            row.pics.push_back(pic);
            row.pics.back().part = r;
            out.push_back(std::move(row));
        }
    }

    // A cell's outputs, each under the one before, moved past the gutter; a
    // result's "Out[n]:" beside it. Stale: of other code than the cell's
    // now (its text and labels quieter).
    std::vector<MdRow> outputs(const std::vector<NbOutput>& read, bool stale) {
        std::vector<MdRow> out;
        for (const NbOutput& o : read) {
            std::vector<MdRow> part;
            for (const NbPart& p : o.parts) {
                std::vector<MdRow> rows_of;
                switch (p.kind) {
                    case NbPart::kText: text(rows_of, p.text, p.ink, p.tone, stale); break;
                    case NbPart::kPicture: picture(rows_of, p, stale); break;
                    case NbPart::kTable: rows_of = md_table(p.cells, p.align, room); break;
                    case NbPart::kMarkdown: rows_of = md_render(p.text, room, md_picture(), look.colour); break;
                }
                if (p.kind == NbPart::kTable || p.kind == NbPart::kMarkdown) {
                    cut(rows_of);
                    if (stale)
                        for (MdRow& r : rows_of)
                            for (MdRun& run : r.runs) run.ink = kMdMuted;
                }
                part.insert(part.end(), std::make_move_iterator(rows_of.begin()), std::make_move_iterator(rows_of.end()));
            }
            if (part.empty()) continue;
            shift(part);
            if (o.count >= 0) put_label(part, 0, "Out[" + std::to_string(o.count) + "]:", stale ? kMdMuted : kMdAnsiYellow, out.empty() ? 0 : 1);
            out.insert(out.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        }
        return out;
    }

    // Everything in rows moved right past the gutter.
    void shift(std::vector<MdRow>& part) const {
        if (!gutter) return;
        for (MdRow& r : part) {
            r.runs.insert(r.runs.begin(), MdRun(std::string(gutter, ' ')));
            if (r.fill) r.fill_from += gutter, r.fill_to += gutter;
            for (MdPic& p : r.pics) p.col += gutter;
        }
    }

    // A label (In [n]:, Out[n]:) beside the row `at` of a cell's rows
    // (right-aligned in the gutter, rows already shifted), or on a row of
    // its own above them when there is no gutter; `before` blank rows above
    // that one.
    void put_label(std::vector<MdRow>& part, size_t at, const std::string& label, u8 ink, u32 before = 0, const std::string& more = {}) const {
        if (gutter) {
            if (part.empty()) part.emplace_back();
            at = std::min(at, part.size() - 1);
            MdRow& r = part[at];
            const u32 w = cols_of(label);
            const std::string pad(gutter > w + 1 ? gutter - w - 1 : 0, ' ');
            if (!r.runs.empty() && r.runs.front().text.find_first_not_of(' ') == std::string::npos && r.runs.front().text.size() == gutter) r.runs.erase(r.runs.begin());
            r.runs.insert(r.runs.begin(), {MdRun(pad), MdRun(label, ink), MdRun(" ")});
            if (!more.empty()) {  // (a word under it)
                const u32 mw = cols_of(more);
                MdRow* under = at + 1 < part.size() ? &part[at + 1] : nullptr;
                if (!under) {
                    part.emplace_back();
                    part.back().runs.push_back(MdRun(std::string(gutter, ' ')));
                    under = &part.back();
                }
                if (!under->runs.empty() && under->runs.front().text.size() == gutter && under->runs.front().text.find_first_not_of(' ') == std::string::npos) under->runs.erase(under->runs.begin());
                under->runs.insert(under->runs.begin(), {MdRun(std::string(gutter > mw + 1 ? gutter - mw - 1 : 0, ' ')), MdRun(more, kMdMuted, kMdItalic), MdRun(" ")});
            }
            return;
        }
        MdRow r;
        r.runs.push_back(MdRun(label, ink));
        if (!more.empty()) r.runs.push_back(MdRun(" " + more, kMdMuted, kMdItalic));
        part.insert(part.begin() + static_cast<std::ptrdiff_t>(std::min(at, part.size())), std::move(r));
        for (u32 k = 0; k < before; ++k) part.insert(part.begin() + static_cast<std::ptrdiff_t>(std::min(at, part.size())), MdRow());
    }

    static void at_line(std::vector<MdRow>& part, u32 line) {
        for (MdRow& r : part) r.line = line;
    }

    // A cell's rows (each its line in the script).
    void cell(const NotebookCell& now, const CellLines& lines, const NotebookCell* ran, bool same) {
        const u32 head = static_cast<u32>(lines.head), first = static_cast<u32>(lines.first);
        const u32 last = static_cast<u32>(lines.end > lines.first ? lines.end - 1 : lines.head);
        std::vector<MdRow> part;
        if (now.kind == "markdown") {
            const std::string key = "m" + now.source;
            if (cache && cache->rows.contains(key)) {
                part = cache->rows[key];
            } else {
                part = md_render(now.source, room, md_picture(), look.colour);
                if (cache) cache->rows[key] = part;
            }
            for (MdRow& r : part) r.line = first + r.line;
            shift(part);
        } else if (now.kind == "raw") {
            text(part, now.source, kMdText, kMdNoTone, false);
            for (size_t k = 0; k < part.size(); ++k) part[k].line = std::min<u32>(first + static_cast<u32>(k), last);
            shift(part);
        } else {
            part = md_code(look.lang, now.source, room, look.colour);
            for (size_t k = 0; k < part.size(); ++k) part[k].line = k == 0 ? head : first + part[k].line;
            if (!part.empty()) part.back().line = last;
            shift(part);
            // In [n]: beside its first line (blank: it has not run; a star:
            // it is running, or waiting to; quieter, and said, if it has
            // changed since).
            const i64 count = ran ? ran->count : -1;
            const std::string label = "In [" + (ran && ran->busy ? std::string("*") : count >= 0 ? std::to_string(count) : std::string(" ")) + "]:";
            const bool changed = ran && !same && (ran->count >= 0 || ran->outputs);
            put_label(part, gutter ? 1 : 0, label, changed ? kMdMuted : kMdAnsiBlue, 0, changed ? "edited" : "");
            if (!gutter && !part.empty()) part.front().line = head;
            if (ran && ran->outputs) {
                const std::string key = "o" + std::to_string(reinterpret_cast<std::uintptr_t>(ran->outputs.get())) + (same ? "" : "s");
                std::vector<MdRow> out;
                if (cache && cache->rows.contains(key)) {
                    out = cache->rows[key];
                } else {
                    const std::vector<NbOutput>* read = nullptr;
                    std::vector<NbOutput> own;
                    if (cache) {
                        NbCache::Read& r = cache->read[ran->outputs.get()];
                        if (!r.outputs) {
                            r.outputs = ran->outputs;
                            r.read = read_outputs(*ran->outputs, look.keep);
                        }
                        read = &r.read;
                    } else {
                        own = read_outputs(*ran->outputs, look.keep);
                        read = &own;
                    }
                    out = outputs(*read, !same);
                    if (cache) cache->rows[key] = out;
                }
                at_line(out, first);  // (a click on an output goes to the cell's first line)
                part.insert(part.end(), std::make_move_iterator(out.begin()), std::make_move_iterator(out.end()));
            }
        }
        if (!rows.empty()) {  // (a blank row between cells)
            MdRow gap;
            gap.line = head;
            rows.push_back(std::move(gap));
        }
        rows.insert(rows.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
};

}  // namespace

std::vector<MdRun> nb_ansi(std::string_view text, u8 ink) {
    std::vector<MdRun> out;
    u8 fg = ink, style = 0, tone = kMdNoTone;
    std::string buf;
    auto flush = [&] {
        if (buf.empty()) return;
        if (!out.empty() && out.back().ink == fg && out.back().style == style && out.back().tone == tone) out.back().text += buf;
        else out.push_back({buf, fg, style, tone});
        buf.clear();
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '\x1b') {
            buf += text[i++];
            continue;
        }
        flush();
        if (i + 1 < text.size() && text[i + 1] == ']') {  // (a title or a link: to its end)
            size_t e = i + 2;
            while (e < text.size() && text[e] != '\a' && !(text[e] == '\x1b' && e + 1 < text.size() && text[e + 1] == '\\')) ++e;
            i = e < text.size() ? e + (text[e] == '\a' ? 1 : 2) : e;
            continue;
        }
        if (i + 1 >= text.size() || text[i + 1] != '[') {
            i += 2;
            continue;
        }
        size_t e = i + 2;
        while (e < text.size() && !(text[e] >= '@' && text[e] <= '~')) ++e;
        if (e >= text.size()) break;
        if (text[e] == 'm') {
            std::vector<int> ps;
            for (size_t k = i + 2; k <= e;) {
                size_t n = k;
                while (n < e && text[n] != ';' && text[n] != ':') ++n;
                ps.push_back(std::atoi(std::string(text.substr(k, n - k)).c_str()));
                k = n + 1;
            }
            for (size_t k = 0; k < ps.size(); ++k) {
                const int p = ps[k];
                auto colour = [&](bool back) {  // 38;5;n and 38;2;r;g;b (48: a background's)
                    u8 got = ink;
                    if (k + 2 < ps.size() && ps[k + 1] == 5) got = ink_of_index(std::clamp(ps[k + 2], 0, 255)), k += 2;
                    else if (k + 4 < ps.size() && ps[k + 1] == 2) got = ink_of_rgb(ps[k + 2], ps[k + 3], ps[k + 4]), k += 4;
                    else k = ps.size();  // (cut short: the rest is not read)
                    if (back) tone = kMdHeaderTone;  // (a background, as the theme's tone)
                    else fg = got;
                };
                if (p == 0) fg = ink, style = 0, tone = kMdNoTone;
                else if (p == 1) style |= kMdBold;
                else if (p == 3) style |= kMdItalic;
                else if (p == 9) style |= kMdStrike;
                else if (p == 22) style &= static_cast<u8>(~kMdBold);
                else if (p == 23) style &= static_cast<u8>(~kMdItalic);
                else if (p == 29) style &= static_cast<u8>(~kMdStrike);
                else if (p >= 30 && p <= 37) fg = ink_of_index(p - 30);
                else if (p >= 90 && p <= 97) fg = ink_of_index(p - 90 + 8);
                else if (p == 39) fg = ink;
                else if ((p >= 40 && p <= 47) || (p >= 100 && p <= 107)) tone = kMdHeaderTone;
                else if (p == 49) tone = kMdNoTone;
                else if (p == 38) colour(false);
                else if (p == 48) colour(true);
            }
        }
        i = e + 1;
    }
    flush();
    return out;
}

std::vector<MdRow> nb_render(std::string_view script, const std::vector<NotebookCell>& model, const NbLook& look, NbCache* cache) {
    std::vector<MdRow> rows;
    const std::vector<NotebookCell> now = notebook_cells(script);
    const std::vector<CellLines> lines = notebook_cell_lines(script);
    const std::vector<CellMatch> match = notebook_match(now, model);
    // The labels beside the cells, as wide as the widest needs, when there
    // is room for them; else above.
    u32 label = 7;  // "In [1]:"
    for (const NotebookCell& c : model) label = std::max(label, 6 + static_cast<u32>(std::to_string(std::max<i64>(c.count, 0)).size()));
    const u32 width = std::max<u32>(look.width, 12);
    Drawer dr{look, cache, rows};
    if (width >= 50) dr.gutter = label + 1;
    dr.room = width - dr.gutter;
    if (cache) {
        const std::string key = look.key + "\n" + std::to_string(width) + "\n" + std::to_string(dr.gutter) + "\n" + look.lang;
        if (cache->look != key) {
            cache->look = key;
            cache->rows.clear();
        }
        // (outputs no longer in the model let go of, and their rows: another's
        // may come to be where they were)
        std::erase_if(cache->read, [&](const auto& e) {
            if (std::ranges::any_of(model, [&](const NotebookCell& c) { return c.outputs.get() == e.first; })) return false;
            const std::string gone = "o" + std::to_string(reinterpret_cast<std::uintptr_t>(e.first));
            cache->rows.erase(gone);
            cache->rows.erase(gone + "s");
            return true;
        });
        if (cache->rows.size() > 4096) cache->rows.clear();
    }
    for (size_t i = 0; i < now.size() && i < lines.size(); ++i) {
        const NotebookCell* ran = match[i].at >= 0 ? &model[static_cast<size_t>(match[i].at)] : nullptr;
        dr.cell(now[i], lines[i], ran, match[i].same);
    }
    return rows;
}

}  // namespace blit
