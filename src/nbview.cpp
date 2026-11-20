#include "nbview.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>

#include "base64.hpp"
#include "json.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

// An output's rows at most (as Jupyter scrolls a long one): the rest is a
// line that says how much more there is.
constexpr u32 kMostRows = 60;
constexpr u32 kMostColumns = 64;  // a table's, read
constexpr u32 kTab = 8;           // a terminal's tab stops

// ---------------------------------------------------------------- text

// (As the view draws it: an emoji joined to the one before takes no columns of its own.)
u32 cols_of(std::string_view s) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    Joiner j;
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        w += j.width(d.cp);
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

// ---------------------------------------------------------------- math

// LaTeX math as text: its symbols as Unicode's (\alpha α, \le ≤, \int ∫),
// a fraction a/b, a power or index of digits raised or lowered (x²),
// spacing as spaces, its braces and sizing gone; what is not known as it is.
std::string math_text(std::string_view tex) {
    static const std::map<std::string_view, std::string_view> kWords = {
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"varepsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"},
        {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"}, {"pi", "π"},
        {"rho", "ρ"}, {"sigma", "σ"}, {"tau", "τ"}, {"upsilon", "υ"}, {"phi", "φ"}, {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"},
        {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Phi", "Φ"}, {"Psi", "Ψ"},
        {"Omega", "Ω"}, {"infty", "∞"}, {"int", "∫"}, {"iint", "∬"}, {"oint", "∮"}, {"sum", "∑"}, {"prod", "∏"}, {"partial", "∂"}, {"nabla", "∇"},
        {"pm", "±"}, {"mp", "∓"}, {"times", "×"}, {"cdot", "·"}, {"div", "÷"}, {"le", "≤"}, {"leq", "≤"}, {"ge", "≥"}, {"geq", "≥"},
        {"ne", "≠"}, {"neq", "≠"}, {"approx", "≈"}, {"equiv", "≡"}, {"sim", "∼"}, {"propto", "∝"}, {"to", "→"}, {"rightarrow", "→"},
        {"leftarrow", "←"}, {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"}, {"Leftrightarrow", "⇔"}, {"iff", "⇔"}, {"mapsto", "↦"}, {"in", "∈"},
        {"notin", "∉"}, {"subset", "⊂"}, {"subseteq", "⊆"}, {"cup", "∪"}, {"cap", "∩"}, {"forall", "∀"}, {"exists", "∃"}, {"emptyset", "∅"},
        {"sqrt", "√"}, {"ldots", "…"}, {"dots", "…"}, {"cdots", "⋯"}, {"circ", "∘"}, {"angle", "∠"}, {"hbar", "ℏ"}, {"ell", "ℓ"},
        {"langle", "⟨"}, {"rangle", "⟩"}, {"lfloor", "⌊"}, {"rfloor", "⌋"}, {"lceil", "⌈"}, {"rceil", "⌉"}, {"neg", "¬"}, {"land", "∧"},
        {"lor", "∨"}, {"wedge", "∧"}, {"vee", "∨"}, {"oplus", "⊕"}, {"otimes", "⊗"}, {"perp", "⊥"}, {"parallel", "∥"}, {"mid", "|"},
        {"quad", "  "}, {"qquad", "    "}, {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"}, {"log", "log"}, {"ln", "ln"}, {"exp", "exp"},
        {"lim", "lim"}, {"max", "max"}, {"min", "min"}, {"det", "det"}, {"{", "{"}, {"}", "}"}, {"$", "$"}, {"%", "%"}, {"&", "&"}, {"_", "_"},
        {",", " "}, {";", " "}, {":", " "}, {" ", " "}, {"!", ""}, {"\\", " "}};
    // (Sizing, and fonts whose text is kept as it is.)
    static const std::string_view kDropped[] = {"left", "right", "big", "Big", "bigg", "Bigg", "displaystyle", "limits", "nolimits"};
    static const std::string_view kFonts[] = {"mathrm", "text", "mathbf", "mathit", "mathsf", "mathtt", "mathcal", "mathbb", "operatorname", "boldsymbol", "textrm", "textbf"};
    static const char* const kUp[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    static const char* const kDown[] = {"₀", "₁", "₂", "₃", "₄", "₅", "₆", "₇", "₈", "₉"};
    // A group ({...}, or one character, or a command) from `at`, and where it ends.
    auto group = [&](size_t at, size_t* end) -> std::string_view {
        while (at < tex.size() && tex[at] == ' ') ++at;
        if (at >= tex.size()) return *end = at, std::string_view();
        if (tex[at] == '{') {
            int depth = 0;
            for (size_t k = at; k < tex.size(); ++k) {
                if (tex[k] == '{') ++depth;
                else if (tex[k] == '}' && --depth == 0) return *end = k + 1, tex.substr(at + 1, k - at - 1);
            }
            return *end = tex.size(), tex.substr(at + 1);
        }
        size_t k = at + 1;
        if (tex[at] == '\\')
            while (k < tex.size() && std::isalpha(static_cast<unsigned char>(tex[k]))) ++k;
        else
            while (k < tex.size() && (static_cast<unsigned char>(tex[k]) & 0xC0) == 0x80) ++k;
        return *end = k, tex.substr(at, k - at);
    };
    std::string out;
    for (size_t i = 0; i < tex.size();) {
        const char c = tex[i];
        if (c == '\\') {
            size_t k = i + 1;
            while (k < tex.size() && std::isalpha(static_cast<unsigned char>(tex[k]))) ++k;
            if (k == i + 1 && k < tex.size()) ++k;  // (a symbol's: \, \{)
            const std::string_view word = tex.substr(i + 1, k - i - 1);
            i = k;
            if (word == "frac" || word == "dfrac" || word == "tfrac") {
                size_t e1 = i, e2 = i;
                const std::string top = math_text(group(i, &e1)), bottom = math_text(group(e1, &e2));
                auto atom = [](const std::string& s) { return s.find_first_of(" +-*/=,") == std::string::npos; };
                out += (atom(top) ? top : "(" + top + ")") + "/" + (atom(bottom) ? bottom : "(" + bottom + ")");
                i = e2;
            } else if (std::ranges::find(kFonts, word) != std::end(kFonts)) {
                size_t e = i;
                out += math_text(group(i, &e));
                i = e;
            } else if (std::ranges::find(kDropped, word) != std::end(kDropped)) {
                if (i < tex.size() && (tex[i] == '(' || tex[i] == ')' || tex[i] == '[' || tex[i] == ']' || tex[i] == '|' || tex[i] == '.')) {
                    if (tex[i] != '.') out += tex[i];
                    ++i;
                }
            } else if (const auto it = kWords.find(word); it != kWords.end()) {
                out += it->second;
                if (word.size() > 1 && std::isalpha(static_cast<unsigned char>(word[0])) && i < tex.size() && std::isalpha(static_cast<unsigned char>(tex[i]))) out += ' ';
            } else {
                out += '\\';
                out += word;
            }
            continue;
        }
        if (c == '^' || c == '_') {
            size_t e = i + 1;
            const std::string_view g = group(i + 1, &e);
            const bool digits = !g.empty() && std::ranges::all_of(g, [](char d) { return d >= '0' && d <= '9'; });
            if (digits) {
                for (const char d : g) out += (c == '^' ? kUp : kDown)[d - '0'];
            } else {
                const std::string inner = math_text(g);
                out += c;
                out += inner.size() > 1 && cols_of(inner) > 1 ? "(" + inner + ")" : inner;
            }
            i = e;
            continue;
        }
        if (c == '{' || c == '}' || c == '&') {
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Markdown with its math ($...$, $$...$$) as text that says just it (in
// italics, as Jupyter's math is), so that its backslashes and underscores
// are not read as Markdown; its lines stay where they were. Code is left be.
std::string md_math(std::string_view md) {
    std::string out;
    bool fenced = false;
    auto said = [&](std::string_view tex) {
        // (each line of it in italics of its own: a line's place is kept)
        for (size_t at = 0; at <= tex.size();) {
            size_t nl = tex.find('\n', at);
            if (nl == std::string_view::npos) nl = tex.size();
            const std::string text = math_text(tex.substr(at, nl - at));
            if (const std::string_view t = trim(text); !t.empty()) out += "*" + md_escape(t) + "*";
            if (nl < tex.size()) out += '\n';
            at = nl + 1;
        }
    };
    for (size_t i = 0; i < md.size();) {
        const size_t line_start = i == 0 || md[i - 1] == '\n' ? i : std::string_view::npos;
        if (line_start != std::string_view::npos) {
            const std::string_view rest = trim(md.substr(i, md.find('\n', i) - i));
            if (rest.starts_with("```") || rest.starts_with("~~~")) fenced = !fenced;
            if (fenced || rest.starts_with("```") || rest.starts_with("~~~")) {
                const size_t nl = md.find('\n', i);
                out += md.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl + 1 - i);
                i = nl == std::string_view::npos ? md.size() : nl + 1;
                continue;
            }
        }
        const char c = md[i];
        if (c == '\\' && i + 1 < md.size()) {  // (an escape: as it is)
            out += md.substr(i, 2);
            i += 2;
            continue;
        }
        if (c == '`') {  // (code: as it is)
            size_t n = i;
            while (n < md.size() && md[n] == '`') ++n;
            size_t close = md.find(md.substr(i, n - i), n);
            if (close != std::string_view::npos && md.substr(n, close - n).find("\n\n") != std::string_view::npos) close = std::string_view::npos;  // (not past its paragraph)
            const size_t end = close == std::string_view::npos ? n : close + (n - i);
            out += md.substr(i, end - i);
            i = end;
            continue;
        }
        if (c == '$') {
            const bool display = md.substr(i, 2) == "$$";
            const size_t open = i + (display ? 2 : 1);
            size_t close = md.find(display ? "$$" : "$", open);
            // (Inline: not "$5 and $10": the text right inside the marks is not a space, nor is a digit after the last.)
            if (close != std::string_view::npos && md.substr(open, close - open).find_first_of(display ? "`" : "`\n") != std::string_view::npos) close = std::string_view::npos;  // (not into code)
            if (!display && close != std::string_view::npos &&
                (open == close || md[open] == ' ' || md[close - 1] == ' ' ||
                 (close + 1 < md.size() && std::isdigit(static_cast<unsigned char>(md[close + 1])))))
                close = std::string_view::npos;
            if (close != std::string_view::npos) {
                said(md.substr(open, close - open));
                i = close + (display ? 2 : 1);
                continue;
            }
        }
        out += c;
        ++i;
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
// (Read once, from the start: what is left is kept as it comes.)
std::string shown_html(std::string_view html) {
    static constexpr std::string_view kCut[][2] = {{"<!--", "-->"}, {"<script", "</script>"}, {"<style", "</style>"}};
    const std::string low = lower(html);
    std::string out;
    out.reserve(html.size());
    size_t kept = 0;
    for (size_t at = 0; (at = low.find('<', at)) != std::string::npos;) {
        size_t end = std::string::npos;
        bool cut = false;
        for (const auto& [open, close] : kCut) {
            if (low.compare(at, open.size(), open) != 0) continue;
            const size_t e = low.find(close, at + open.size());
            end = e == std::string::npos ? low.size() : e + close.size();
            cut = true;
            break;
        }
        if (!cut) {
            ++at;
            continue;
        }
        out.append(html.substr(kept, at - kept));
        kept = at = end;
    }
    out.append(html.substr(std::min(kept, html.size())));
    return out;
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
// Only what can be shown is read: the first kMostRows + 1 rows of its
// body (`more`: how many rows come after them), kMostColumns columns.
bool html_table(std::string_view table, std::vector<std::vector<std::string>>* cells, std::vector<int>* align, u32* more) {
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
    // Where each tag that ends a cell's text is next, looked for again only
    // once passed (a table without one is not searched to its end for each cell).
    struct Next {
        std::string_view what;
        size_t at = 0;
        bool known = false;
    };
    Next next_td{"<td"}, next_th{"<th"}, end_tr{"</tr"}, end_td{"</td"}, end_th{"</th"};
    auto find = [&](Next& n, size_t from) {
        if (!n.known || (n.at != std::string::npos && n.at < from)) n.at = low.find(n.what, from), n.known = true;
        return n.at;
    };
    bool head = false;
    int row_align = 2;
    size_t body_rows = 0;
    *more = 0;
    std::vector<Cell>* row = nullptr;
    for (size_t i = 0; i < table.size();) {
        const size_t open = low.find('<', i);
        if (open == std::string::npos) break;
        const size_t close = low.find('>', open);
        if (close == std::string::npos) break;
        const std::string_view tag = table.substr(open, close - open + 1);
        std::string name = low.substr(open + 1, std::min<size_t>(close - open - 1, 16));
        name = name.substr(0, name.find_first_of(" \t\n/>"));
        i = close + 1;
        if (name == "thead") head = true;
        else if (low.compare(open, 8, "</thead>") == 0 || name == "tbody") head = false;
        else if (name == "tr") {
            if (!head && ++body_rows > kMostRows + 1) {  // (the rest only counted)
                for (size_t at = open; (at = low.find("<tr", at)) != std::string::npos; at += 3)
                    if (at + 3 < low.size() && (low[at + 3] == '>' || std::isspace(static_cast<unsigned char>(low[at + 3])))) ++*more;
                break;
            }
            rows.emplace_back();
            in_head.push_back(head);
            row = &rows.back();
            row_align = align_of(tag);
        } else if ((name == "td" || name == "th") && row) {
            // Its text: to its end (or the next cell's start), without tags.
            size_t end = std::min({find(name == "td" ? end_td : end_th, i), find(next_td, i), find(next_th, i), find(end_tr, i)});
            if (end == std::string::npos) end = table.size();
            const size_t from = i;
            i = end;
            if (row->size() >= kMostColumns) continue;  // (past what is shown)
            std::string text;
            for (size_t k = from; k < end; ++k) {
                if (table[k] == '<') {
                    const size_t gt = table.find('>', k);
                    if (gt == std::string_view::npos || gt >= end) break;
                    if (low.compare(k, 3, "<br") == 0) text += ' ';
                    k = gt;
                    continue;
                }
                text += table[k];
            }
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
            const int across = std::clamp(std::atoi(attribute(tag, "colspan").c_str()), 1, static_cast<int>(kMostColumns));
            const int down = std::clamp(std::atoi(attribute(tag, "rowspan").c_str()), 1, static_cast<int>(kMostRows) + 2);  // (no more rows are read)
            for (int k = 0; k < across && row->size() < kMostColumns; ++k) {
                Cell part = c;
                if (k > 0 && !in_head.back()) part.text.clear();  // (a heading over columns names each)
                if (row->size() >= spanned.size()) spanned.resize(row->size() + 1, 0), spanning.resize(row->size() + 1);
                spanned[row->size()] = static_cast<u32>(down - 1);
                spanning[row->size()] = part;
                row->push_back(std::move(part));
            }
            fill_spanned();
        }
        if (rows.size() > 4 * kMostRows) break;  // (rows of a header without end)
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

// The columns a table's cell (Markdown, as html_markdown makes it) takes
// drawn: its escapes, entities and bold marks not counted.
u32 shown_cols(std::string_view md) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(md.data());
    Joiner j;
    for (size_t i = 0; i < md.size();) {
        if (md[i] == '\\' && i + 1 < md.size() && std::ispunct(static_cast<unsigned char>(md[i + 1]))) {
            ++w, i += 2;
            j.reset();
            continue;
        }
        if (md.substr(i, 2) == "**") {
            i += 2;
            continue;
        }
        if (md[i] == '&') {
            const size_t semi = md.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 12) {
                ++w, i = semi + 1;
                j.reset();
                continue;
            }
        }
        const Decoded d = decode_utf8(p + i, md.size() - i);
        w += j.width(d.cp);
        i += d.len;
    }
    return w;
}

// Each column's width as drawn without being made narrower.
std::vector<u32> natural_widths(const std::vector<std::vector<std::string>>& cells) {
    std::vector<u32> w;
    for (const std::vector<std::string>& r : cells) {
        if (r.size() > w.size()) w.resize(r.size(), 1);
        for (size_t c = 0; c < r.size(); ++c) w[c] = std::max(w[c], shown_cols(r[c]));
    }
    return w;
}

// The columns a table takes drawn whole (md_table: a bar and a space each side of each cell).
u32 table_cols(const std::vector<std::vector<std::string>>& cells) {
    const std::vector<u32> w = natural_widths(cells);
    u32 total = static_cast<u32>(3 * w.size() + 1);
    for (const u32 x : w) total += x;
    return total;
}

// The widest of a text's lines, in columns.
u32 widest_line(std::string_view text) {
    u32 most = 0;
    for (size_t at = 0; at <= text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        most = std::max(most, cols_of(text.substr(at, nl - at)));
        at = nl + 1;
    }
    return most;
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

// Lines of runs at most `width` columns wide, tabs to a terminal's stops: a
// long line goes on in the next after its last space, as Jupyter wraps
// one (a word longer than the width, where the width ends); and which of
// the text's lines each is of. No more than `most` + 1 are made.
std::vector<std::vector<MdRun>> wrap_runs(const std::vector<MdRun>& runs, u32 width, std::vector<u32>* of, size_t most = SIZE_MAX) {
    std::vector<std::vector<MdRun>> lines;
    of->clear();
    std::string row;          // the line being made: its bytes,
    std::vector<u32> from;    // the run each is of,
    u32 col = 0, source = 0;  // its columns, and the text's line it is of
    size_t space = std::string::npos;  // where it may break (after its last space), and its columns before that
    u32 space_col = 0;
    // Its first n bytes, a line.
    auto line = [&](size_t n) {
        std::vector<MdRun> out;
        for (size_t k = 0; k < n;) {
            const MdRun& as = runs[from[k]];
            size_t e = k + 1;
            while (e < n && runs[from[e]].ink == as.ink && runs[from[e]].style == as.style && runs[from[e]].tone == as.tone) ++e;
            out.push_back({row.substr(k, e - k), as.ink, as.style, as.tone});
            k = e;
        }
        lines.push_back(std::move(out));
        of->push_back(source);
        row.erase(0, n);
        from.erase(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(n));
        space = std::string::npos;
    };
    for (u32 r = 0; r < runs.size(); ++r) {
        const std::string_view s = runs[r].text;
        const u8* p = reinterpret_cast<const u8*>(s.data());
        for (size_t i = 0; i < s.size();) {
            if (lines.size() > most) return lines;
            if (s[i] == '\n') {
                line(row.size());
                ++source;
                col = 0;
                ++i;
                continue;
            }
            if (s[i] == '\t') {
                const u32 to = std::min((col / kTab + 1) * kTab, width);
                row.append(to > col ? to - col : 0, ' ');
                from.resize(row.size(), r);
                col = to;
                ++i;
                if (col >= width) line(row.size()), col = 0;
                else space = row.size(), space_col = col;
                continue;
            }
            const Decoded d = decode_utf8(p + i, s.size() - i);
            const u32 w = cp_width(d.cp);
            if (col + w > width && col > 0) {
                if (space != std::string::npos && space < row.size()) {
                    col -= space_col;
                    line(space);
                } else {
                    line(row.size());
                    col = 0;
                }
                if (col + w > width && col > 0) line(row.size()), col = 0;  // (a wide character after what was carried over)
                if (lines.size() > most) return lines;
            }
            row.append(s.substr(i, d.len));
            from.resize(row.size(), r);
            col += w;
            i += d.len;
            if (d.cp == ' ') space = row.size(), space_col = col;
        }
    }
    line(row.size());
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
        p.lines = static_cast<u64>(std::max<i64>(o["blit_lines"].integer(0), 0));  // (a running cell's long text, cut: its lines)
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
            if (html_table(std::string_view(h).substr(t, e - t), &p.cells, &p.align, &p.more)) parts.push_back(std::move(p));
            at = e;
        }
        words(std::string_view(h).substr(at));
        if (parts.empty()) return false;
        if (std::ranges::any_of(parts, [](const NbPart& p) { return p.kind == NbPart::kTable; })) out.plain = plain;
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
        p.text = md_math(text_of(md));
        out.parts.push_back(std::move(p));
        return out;
    }
    if (data["text/html"].exists() && html(false)) return out;
    if (const json::Value latex = data["text/latex"]; latex.exists()) {  // (its math as text, as a Markdown cell's is)
        std::string tex(trim(text_of(latex)));
        for (const auto& [open, close] : {std::pair{"$$", "$$"}, std::pair{"\\[", "\\]"}, std::pair{"$", "$"}, std::pair{"\\(", "\\)"}})
            if (tex.size() >= 4 && tex.starts_with(open) && tex.ends_with(close)) {
                tex = tex.substr(std::strlen(open), tex.size() - std::strlen(open) - std::strlen(close));
                break;
            }
        NbPart p;
        p.kind = NbPart::kMarkdown;
        p.text = md_math("$$" + tex + "$$");
        out.parts.push_back(std::move(p));
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
            NbPart& last = out.back().parts.back();
            if (r.parts.back().lines) last.lines = static_cast<u64>(std::count(last.text.begin(), last.text.end(), '\n')) + r.parts.back().lines;
            last.text += r.parts.back().text;
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

    // Whether rows are kept by that key (cache: there is one); this
    // drawing has them, so they stay.
    bool kept(const std::string& key) const {
        cache->drawn.insert(key);
        return cache->rows.contains(key);
    }

    MdPictureFn md_picture() const {
        return [this](const std::string& src, u32 r, u32 w, u32 h, MdPic* out) { return look.picture && look.picture(src, r, w, h, out) == kNbPictureReady; };
    }

    // Rows of text, cut after kMostRows with a line that says how many more
    // there are. A running cell's text cut short (its first lines and its
    // last, `said_lines` its newlines): its last line after that one.
    void text(std::vector<MdRow>& out, std::string_view s, u8 ink, u8 tone, bool stale, u64 said_lines = 0) {
        std::string_view body = s;
        while (body.ends_with('\n')) body.remove_suffix(1);
        u64 own = static_cast<u64>(std::count(body.begin(), body.end(), '\n'));
        std::string_view tail;
        if (said_lines > own && own > 0) {
            const size_t nl = body.rfind('\n');
            tail = body.substr(nl + 1);
            body = body.substr(0, nl);
            --own;
        }
        // (Only as much as can be shown is read: the first lines, a
        // megabyte at most; the rest only counted.)
        const u32 last = static_cast<u32>(said_lines > own + 1 ? said_lines - 1 : own);
        size_t upto = 0;
        for (u32 k = 0; k <= kMostRows && upto < body.size(); ++k) {
            const size_t nl = body.find('\n', upto);
            upto = nl == std::string_view::npos ? body.size() : nl + 1;
        }
        std::vector<MdRun> runs = nb_ansi(body.substr(0, std::min<size_t>(upto, 1 << 20)), ink);
        if (stale)
            for (MdRun& r : runs) r.ink = kMdMuted;
        for (MdRun& r : runs)
            if (r.tone == kMdNoTone) r.tone = tone;
        std::vector<u32> of;
        const std::vector<std::vector<MdRun>> lines = wrap_runs(runs, room, &of, kMostRows);
        const u32 shown = static_cast<u32>(std::min<size_t>(lines.size(), kMostRows));
        for (u32 k = 0; k < shown; ++k) {
            MdRow r;
            r.runs = lines[k];
            if (tone) r.fill = tone, r.fill_from = 0, r.fill_to = room;
            out.push_back(std::move(r));
        }
        if (shown < lines.size() || (shown && of[shown - 1] < last)) {  // (how many of the text's own lines are not begun)
            const u32 more = last - of[shown - 1];
            MdRow r;
            const std::string say = more ? "\xE2\x80\xA6 " + std::to_string(more) + (more == 1 ? " more line" : " more lines") : "\xE2\x80\xA6 more of this line";
            r.runs.push_back({say, kMdMuted, kMdItalic, tone});
            if (tone) r.fill = tone, r.fill_from = 0, r.fill_to = room;
            out.push_back(std::move(r));
        }
        if (said_lines > own + 1 && !tail.empty()) {
            std::vector<MdRun> end = nb_ansi(tail.substr(0, 4096), ink);
            for (MdRun& r : end) {
                if (stale) r.ink = kMdMuted;
                if (r.tone == kMdNoTone) r.tone = tone;
            }
            std::vector<u32> end_of;
            for (std::vector<MdRun>& l : wrap_runs(end, room, &end_of, 3)) {
                MdRow r;
                r.runs = std::move(l);
                if (tone) r.fill = tone, r.fill_from = 0, r.fill_to = room;
                out.push_back(std::move(r));
            }
        }
    }

    // How many of a table's first columns fit in the room whole (one at least).
    size_t columns_that_fit(const std::vector<std::vector<std::string>>& cells) const {
        const std::vector<u32> w = natural_widths(cells);
        size_t keep = 0;
        for (u32 used = 1; keep < w.size() && used + w[keep] + 3 <= room; ++keep) used += w[keep] + 3;
        return std::max<size_t>(keep, 1);
    }

    // Rows of something drawn (a table, Markdown), cut as text is (not
    // through a picture); `unread` rows more that were not drawn.
    static void cut(std::vector<MdRow>& part, u32 unread = 0) {
        if (part.size() <= kMostRows + 1 && !unread) return;
        size_t at = std::min<size_t>(kMostRows, part.size());
        while (at > 0 && at < part.size() && std::ranges::any_of(part[at].pics, [](const MdPic& p) { return p.part > 0; })) --at;
        const size_t more = part.size() - at + unread;
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
            // A table wider than the room is not squeezed into it (its
            // numbers broken over lines): its output's own text is drawn
            // instead if that fits, else the columns that fit.
            const bool too_wide = std::ranges::any_of(o.parts, [&](const NbPart& p) { return p.kind == NbPart::kTable && table_cols(p.cells) > room; });
            if (too_wide && !trim(o.plain).empty() && widest_line(o.plain) <= room) {
                text(part, o.plain, kMdText, kMdNoTone, stale);
                shift(part);
                if (o.count >= 0) put_label(part, 0, "Out[" + std::to_string(o.count) + "]:", stale ? kMdMuted : kMdAnsiYellow, out.empty() ? 0 : 1);
                out.insert(out.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
                continue;
            }
            for (const NbPart& p : o.parts) {
                std::vector<MdRow> rows_of;
                u32 dropped = 0;  // (a table's columns that do not fit)
                switch (p.kind) {
                    case NbPart::kText: text(rows_of, p.text, p.ink, p.tone, stale, p.lines); break;
                    case NbPart::kPicture: picture(rows_of, p, stale); break;
                    case NbPart::kTable: {
                        if (table_cols(p.cells) <= room) {
                            rows_of = md_table(p.cells, p.align, room);
                            break;
                        }
                        std::vector<std::vector<std::string>> cells = p.cells;
                        std::vector<int> align = p.align;
                        const size_t keep = columns_that_fit(cells);
                        dropped = static_cast<u32>(align.size() - std::min(keep, align.size()));
                        for (std::vector<std::string>& r : cells) r.resize(std::min(r.size(), keep));
                        align.resize(std::min(align.size(), keep));
                        rows_of = md_table(cells, align, room);
                        break;
                    }
                    case NbPart::kMarkdown: rows_of = md_render(p.text, room, md_picture(), look.colour, {}, look.glyphs); break;
                }
                if (p.kind == NbPart::kTable || p.kind == NbPart::kMarkdown) {
                    cut(rows_of, p.more);
                    if (dropped) {
                        MdRow r;
                        r.runs.push_back({"\xE2\x80\xA6 " + std::to_string(dropped) + (dropped == 1 ? " more column" : " more columns"), kMdMuted, kMdItalic});
                        rows_of.push_back(std::move(r));
                    }
                    if (stale)
                        for (MdRow& r : rows_of)
                            for (MdRun& run : r.runs) run.ink = kMdMuted;
                }
                part.insert(part.end(), std::make_move_iterator(rows_of.begin()), std::make_move_iterator(rows_of.end()));
            }
            if (part.empty()) continue;
            shift(part);
            if (o.count >= 0) put_label(part, 0, "Out[" + std::to_string(o.count) + "]:", stale ? kMdMuted : kMdAnsiYellow, out.empty() ? 0 : 1);
            if (!out.empty() && out.back().fill != kMdNoTone && part.front().fill != kMdNoTone) out.emplace_back();  // (two tinted, stderr and an error: apart)
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
        if (gutter && cols_of(label) + 1 > gutter) {  // (too long for the gutter: on a row of its own above)
            MdRow r;
            r.runs.push_back(MdRun(label, ink));
            if (!more.empty()) r.runs.push_back(MdRun(" " + more, kMdMuted, kMdItalic));
            at = at ? at - 1 : 0;  // (above the row it would be beside)
            r.line = at < part.size() ? part[at].line : 0;
            part.insert(part.begin() + static_cast<std::ptrdiff_t>(std::min(at, part.size())), std::move(r));
            return;
        }
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
            if (cache && kept(key)) {
                part = cache->rows[key];
            } else {
                part = md_render(md_math(now.source), room, md_picture(), look.colour, {}, look.glyphs);
                if (cache) cache->rows[key] = part;
            }
            for (MdRow& r : part) r.line = first + r.line;
            shift(part);
        } else if (now.kind == "raw") {
            text(part, now.source, kMdText, kMdNoTone, false);
            for (size_t k = 0; k < part.size(); ++k) part[k].line = std::min<u32>(first + static_cast<u32>(k), last);
            shift(part);
        } else {
            const std::string code_key = "c" + now.source;  // (kept as a Markdown cell's are: coloured once)
            if (cache && kept(code_key)) {
                part = cache->rows[code_key];
            } else {
                part = md_code(look.lang, now.source, room, look.colour);
                for (MdRow& r : part)  // (with no grammar for it: the text's colour, not a string's)
                    for (MdRun& run : r.runs)
                        if (run.ink == kMdCode) run.ink = kMdCodePlain;
                if (cache) cache->rows[code_key] = part;
            }
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
                if (cache && kept(key)) {
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

namespace {
// The labels' columns (In [n]:, as wide as the widest needs; and a space),
// 0 when there is not room for them beside the cells.
u32 gutter_of(u32 width, const std::vector<NotebookCell>& model) {
    u32 label = 7;  // "In [1]:"
    for (const NotebookCell& c : model) label = std::max(label, 6 + static_cast<u32>(std::to_string(std::max<i64>(c.count, 0)).size()));
    label = std::min(label, 10u);  // (a longer count, "In [12345]:", goes above its cell: the rest do not move over for it)
    return std::max<u32>(width, 12) >= 50 ? label + 1 : 0;
}
}  // namespace

u32 nb_room(u32 width, const std::vector<NotebookCell>& model) { return std::max<u32>(width, 12) - gutter_of(width, model); }

std::vector<MdRow> nb_render(std::string_view script, const std::vector<NotebookCell>& model, const NbLook& look, NbCache* cache) {
    std::vector<MdRow> rows;
    const std::vector<NotebookCell> now = notebook_cells(script);
    const std::vector<CellLines> lines = notebook_cell_lines(script);
    const std::vector<CellMatch> match = notebook_match(now, model);
    // The labels beside the cells, as wide as the widest needs, when there
    // is room for them; else above.
    const u32 width = std::max<u32>(look.width, 12);
    Drawer dr{look, cache, rows};
    dr.gutter = gutter_of(look.width, model);
    dr.room = width - dr.gutter;
    if (cache) {
        const std::string key = look.key + "\n" + std::to_string(width) + "\n" + std::to_string(dr.gutter) + "\n" + look.lang;
        if (cache->look != key) {
            cache->look = key;
            cache->rows.clear();
        }
        // (outputs no longer in the model let go of, and their rows: another's
        // may come to be where they were)
        std::set<const std::string*> held;
        for (const NotebookCell& c : model)
            if (c.outputs) held.insert(c.outputs.get());
        std::erase_if(cache->read, [&](const auto& e) {
            if (held.contains(e.first)) return false;
            const std::string gone = "o" + std::to_string(reinterpret_cast<std::uintptr_t>(e.first));
            cache->rows.erase(gone);
            cache->rows.erase(gone + "s");
            return true;
        });
        cache->drawn.clear();
    }
    for (size_t i = 0; i < now.size() && i < lines.size(); ++i) {
        const NotebookCell* ran = match[i].at >= 0 ? &model[static_cast<size_t>(match[i].at)] : nullptr;
        dr.cell(now[i], lines[i], ran, match[i].same);
    }
    // (Kept: the rows this drawing has, so that however many cells the
    // notebook has, the next drawing finds them all.)
    if (cache) std::erase_if(cache->rows, [&](const auto& e) { return !cache->drawn.contains(e.first); });
    return rows;
}

}  // namespace blit
