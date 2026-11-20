#include "markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>

#include "utf8.hpp"

namespace blit {

namespace {

// ---------------------------------------------------------------- text

// The columns s takes, as the preview draws it (TextBox::put): an emoji
// joined to the one before it (a family, a flag's second half) takes none
// of its own.
u32 cols_of(std::string_view s) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    Joiner j;
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        w += j.width(d.cp);
        i += d.len ? d.len : 1;
    }
    return w;
}

// The bytes of the first character of s (at least 1 if there is any).
size_t first_char(std::string_view s) {
    if (s.empty()) return 0;
    const size_t len = decode_utf8(reinterpret_cast<const u8*>(s.data()), s.size()).len;
    return std::clamp<size_t>(len, 1, s.size());
}

// The bytes of s that fit in n columns.
size_t fit(std::string_view s, u32 n) {
    u32 w = 0;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    size_t i = 0;
    Joiner j;
    while (i < s.size()) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        const u32 cw = j.width(d.cp);
        if (w + cw > n) break;
        w += cw;
        i += d.len ? d.len : 1;
    }
    return i;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool blank(std::string_view s) { return trim(s).empty(); }

// Columns of white space in front (a tab to the next multiple of 4).
int indent_of(std::string_view s) {
    int n = 0;
    for (const char c : s) {
        if (c == ' ') ++n;
        else if (c == '\t') n = (n / 4 + 1) * 4;
        else break;
    }
    return n;
}

// The line without up to n columns of white space in front.
std::string strip_cols(std::string_view s, int n) {
    int at = 0;
    size_t i = 0;
    while (i < s.size() && at < n && (s[i] == ' ' || s[i] == '\t')) {
        const int next = s[i] == '\t' ? (at / 4 + 1) * 4 : at + 1;
        if (next > n) {  // (a tab only partly taken: the rest of it as spaces)
            std::string out(static_cast<size_t>(next - n), ' ');
            return out + std::string(s.substr(i + 1));
        }
        at = next;
        ++i;
    }
    return std::string(s.substr(i));
}

// The line from column n on, whatever is before it (a list item's marker).
std::string from_col(std::string_view s, int n) {
    int at = 0;
    size_t i = 0;
    while (i < s.size() && at < n) {
        if (s[i] == '\t') {
            const int next = (at / 4 + 1) * 4;
            if (next > n) return std::string(static_cast<size_t>(next - n), ' ') + std::string(s.substr(i + 1));
            at = next;
        } else {
            ++at;
        }
        ++i;
    }
    return std::string(s.substr(i));
}

// Tabs as spaces. With `inks` (one for each byte of s), *out_inks gets one
// for each byte of what comes back (a tab's for its spaces).
std::string expand_tabs(std::string_view s, const u8* inks = nullptr, std::vector<u8>* out_inks = nullptr) {
    std::string out;
    u32 col = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\t') {
            const u32 to = (col / 4 + 1) * 4;
            out.append(to - col, ' ');
            if (inks) out_inks->insert(out_inks->end(), to - col, inks[i]);
            col = to;
        } else {
            out += c;
            if (inks) out_inks->push_back(inks[i]);
            if (!is_cont(static_cast<u8>(c))) ++col;
        }
    }
    return out;
}

void encode_utf8(u32 cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------- inline

struct Span {
    std::string text;
    u8 style = 0, ink = kMdText, tone = kMdNoTone;
    std::string link;
    bool brk = false;  // a line break, not text
    std::string image;          // a picture's file (the text is what stands for it, if it cannot be drawn)
    u32 image_w = 0, image_h = 0;  // the size it asks for (<img width=...>; 0: none)
};

// The value of an HTML tag's attribute (`name="v"`, `name='v'`, `name=v`), or "".
std::string attribute(std::string_view tag, std::string_view name) {
    for (size_t at = 0; (at = tag.find(name, at)) != std::string_view::npos; at += name.size()) {
        const bool starts = at > 0 && (tag[at - 1] == ' ' || tag[at - 1] == '\t' || tag[at - 1] == '\n');
        size_t k = at + name.size();
        while (k < tag.size() && tag[k] == ' ') ++k;
        if (!starts || k >= tag.size() || tag[k] != '=') continue;
        ++k;
        while (k < tag.size() && tag[k] == ' ') ++k;
        if (k >= tag.size()) return {};
        if (tag[k] == '"' || tag[k] == '\'') {
            const size_t end = tag.find(tag[k], k + 1);
            return std::string(tag.substr(k + 1, end == std::string_view::npos ? std::string_view::npos : end - k - 1));
        }
        const size_t end = tag.find_first_of(" \t\n/>", k);
        return std::string(tag.substr(k, end == std::string_view::npos ? std::string_view::npos : end - k));
    }
    return {};
}

// A size an attribute gives in pixels ("200", "200px"); 0 for none or a percentage.
u32 attribute_px(std::string_view tag, std::string_view name) {
    const std::string v = attribute(tag, name);
    u32 n = 0;
    size_t k = 0;
    for (; k < v.size() && std::isdigit(static_cast<unsigned char>(v[k])) && n < 100000; ++k) n = n * 10 + static_cast<u32>(v[k] - '0');
    return k < v.size() && v[k] == '%' ? 0 : n;
}

struct Refs {
    std::map<std::string, std::string> to;  // a link's label (lower case) -> where it goes
    size_t longest = 0;                     // the longest label (text longer is none of them)
    void set(std::string label, std::string dest) {
        longest = std::max(longest, label.size());
        to[std::move(label)] = std::move(dest);
    }
    // Footnotes: each one defined, by its label (lower case), with its
    // number (0 until something refers to it: they are numbered in the order
    // they are first referred to, as the text is drawn), and the labels in
    // that order.
    mutable std::map<std::string, int> notes;
    mutable std::vector<std::string> noted;
    size_t longest_note = 0;
    int number(const std::string& label) const {  // (0: no such footnote)
        const auto it = notes.find(label);
        if (it == notes.end()) return 0;
        if (!it->second) {
            noted.push_back(label);
            it->second = static_cast<int>(noted.size());
        }
        return it->second;
    }
    u8 glyphs = kMdAllGlyphs;  // the raised and lowered characters the font has
};

// A number raised (¹²): a footnote's mark.
std::string raised(int n) {
    static const char* const kUp[] = {"\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4", "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9"};
    std::string out;
    for (const char d : std::to_string(n)) out += kUp[d - '0'];
    return out;
}

constexpr int kMostDepth = 32;  // quotes, lists, emphasis and links nested deeper than this are taken as text

std::string entity(std::string_view s, size_t* len) {
    static const std::pair<const char*, const char*> kNamed[] = {{"&amp;", "&"},  {"&lt;", "<"},       {"&gt;", ">"},       {"&quot;", "\""},
                                                                 {"&#39;", "'"},  {"&apos;", "'"},     {"&nbsp;", "\xC2\xA0"}, {"&copy;", "\xC2\xA9"},
                                                                 {"&mdash;", "\xE2\x80\x94"}, {"&ndash;", "\xE2\x80\x93"}, {"&hellip;", "\xE2\x80\xA6"}, {"&rarr;", "\xE2\x86\x92"}};
    for (const auto& [name, text] : kNamed)
        if (s.starts_with(name)) {
            *len = std::string_view(name).size();
            return text;
        }
    if (s.starts_with("&#")) {
        const size_t semi = s.substr(0, 12).find(';');  // (not past where it could be: the rest of a long line, for each &#)
        if (semi != std::string_view::npos && semi < 12) {
            const bool hex = s.size() > 2 && (s[2] == 'x' || s[2] == 'X');
            const std::string num(s.substr(hex ? 3 : 2, semi - (hex ? 3 : 2)));
            char* end = nullptr;
            const unsigned long cp = std::strtoul(num.c_str(), &end, hex ? 16 : 10);
            if (end && *end == 0 && cp > 0 && cp < 0x110000) {
                *len = semi + 1;
                std::string out;
                encode_utf8(static_cast<u32>(cp), out);
                return out;
            }
        }
    }
    *len = 0;
    return {};
}

// ---------------------------------------------------------------- math

// The raised (`up`) or lowered form Unicode has for a character, or 0.
u32 script_of(u32 cp, bool up) {
    static constexpr std::pair<u32, u32> kUp[] = {
        {'0', 0x2070}, {'1', 0xB9},   {'2', 0xB2},   {'3', 0xB3},   {'4', 0x2074}, {'5', 0x2075}, {'6', 0x2076}, {'7', 0x2077}, {'8', 0x2078}, {'9', 0x2079},
        {'+', 0x207A}, {'-', 0x207B}, {0x2212, 0x207B}, {'=', 0x207C}, {'(', 0x207D}, {')', 0x207E},
        {'a', 0x1D43}, {'b', 0x1D47}, {'c', 0x1D9C}, {'d', 0x1D48}, {'e', 0x1D49}, {'f', 0x1DA0}, {'g', 0x1D4D}, {'h', 0x2B0},  {'i', 0x2071}, {'j', 0x2B2},
        {'k', 0x1D4F}, {'l', 0x2E1},  {'m', 0x1D50}, {'n', 0x207F}, {'o', 0x1D52}, {'p', 0x1D56}, {'r', 0x2B3},  {'s', 0x2E2},  {'t', 0x1D57}, {'u', 0x1D58},
        {'v', 0x1D5B}, {'w', 0x2B7},  {'x', 0x2E3},  {'y', 0x2B8},  {'z', 0x1DBB}, {'A', 0x1D2C}, {'B', 0x1D2E}, {'D', 0x1D30}, {'E', 0x1D31}, {'G', 0x1D33},
        {'H', 0x1D34}, {'I', 0x1D35}, {'J', 0x1D36}, {'K', 0x1D37}, {'L', 0x1D38}, {'M', 0x1D39}, {'N', 0x1D3A}, {'O', 0x1D3C}, {'P', 0x1D3E}, {'R', 0x1D3F},
        {'T', 0x1D40}, {'U', 0x1D41}, {'V', 0x2C7D}, {'W', 0x1D42}, {0x3B1, 0x1D45}, {0x3B2, 0x1D5D}, {0x3B3, 0x1D5E}, {0x3B4, 0x1D5F}, {0x3B5, 0x1D4B},
        {0x3B8, 0x1DBF}, {0x3C6, 0x1D60}, {0x3C7, 0x1D61}};
    static constexpr std::pair<u32, u32> kDown[] = {
        {'0', 0x2080}, {'1', 0x2081}, {'2', 0x2082}, {'3', 0x2083}, {'4', 0x2084}, {'5', 0x2085}, {'6', 0x2086}, {'7', 0x2087}, {'8', 0x2088}, {'9', 0x2089},
        {'+', 0x208A}, {'-', 0x208B}, {0x2212, 0x208B}, {'=', 0x208C}, {'(', 0x208D}, {')', 0x208E},
        {'a', 0x2090}, {'e', 0x2091}, {'h', 0x2095}, {'i', 0x1D62}, {'j', 0x2C7C}, {'k', 0x2096}, {'l', 0x2097}, {'m', 0x2098}, {'n', 0x2099}, {'o', 0x2092},
        {'p', 0x209A}, {'r', 0x1D63}, {'s', 0x209B}, {'t', 0x209C}, {'u', 0x1D64}, {'v', 0x1D65}, {'x', 0x2093},
        {0x3B2, 0x1D66}, {0x3B3, 0x1D67}, {0x3C1, 0x1D68}, {0x3C6, 0x1D69}, {0x3C7, 0x1D6A}};
    for (const auto& [from, to] : up ? std::span<const std::pair<u32, u32>>(kUp) : std::span<const std::pair<u32, u32>>(kDown))
        if (from == cp) return to;
    return 0;
}

// s raised or lowered (spaces in it left out), each character in the form
// Unicode has for it, onto *out: when all of them have one and the font
// draws those (`glyphs`: digits, and the rest). False (and nothing added) if not.
bool scripted(std::string_view s, bool up, u8 glyphs, std::string* out) {
    std::string r;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        i += d.len ? d.len : 1;
        if (d.cp == ' ') continue;
        const u32 to = script_of(d.cp, up);
        const bool digit = d.cp >= '0' && d.cp <= '9';
        if (!to || !(glyphs & (digit ? (up ? kMdSuperDigits : kMdSubDigits) : (up ? kMdSuperLetters : kMdSubLetters)))) return false;
        encode_utf8(to, r);
    }
    if (r.empty()) return false;
    *out += r;
    return true;
}

// What follows a ^ or _ that cannot be raised or lowered: in brackets,
// when it is more than a character and not in brackets already (x^(n+1)).
std::string script_mark(char mark, const std::string& s) {
    size_t chars = 0;
    for (const char ch : s) chars += !is_cont(static_cast<u8>(ch));
    const bool own = s.size() >= 2 && ((s.front() == '(' && s.back() == ')') || (s.front() == '[' && s.back() == ']'));
    return mark + (chars > 1 && !own ? "(" + s + ")" : s);
}

// TeX's math as text, onto `out` (md_math_text); groups nested deeper
// than kMostDepth are left as they are (each level looks to its group's end).
void tex_into(std::string_view tex, u8 glyphs, int depth, std::string& out) {
    if (depth >= kMostDepth) {
        out += tex;
        return;
    }
    static const std::map<std::string_view, std::string_view> kWords = {
        // Greek
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ϵ"}, {"varepsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"},
        {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"}, {"omicron", "ο"},
        {"pi", "π"}, {"varpi", "ϖ"}, {"rho", "ρ"}, {"varrho", "ϱ"}, {"sigma", "σ"}, {"varsigma", "ς"}, {"tau", "τ"}, {"upsilon", "υ"}, {"phi", "ϕ"},
        {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"}, {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Xi", "Ξ"},
        {"Pi", "Π"}, {"Sigma", "Σ"}, {"Upsilon", "Υ"}, {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
        // big operators, and others
        {"sum", "∑"}, {"prod", "∏"}, {"coprod", "∐"}, {"int", "∫"}, {"iint", "∬"}, {"iiint", "∭"}, {"oint", "∮"}, {"bigcup", "⋃"}, {"bigcap", "⋂"},
        {"bigoplus", "⨁"}, {"bigotimes", "⨂"}, {"bigvee", "⋁"}, {"bigwedge", "⋀"},
        {"pm", "±"}, {"mp", "∓"}, {"times", "×"}, {"cdot", "·"}, {"cdotp", "·"}, {"div", "÷"}, {"ast", "∗"}, {"star", "⋆"}, {"circ", "∘"}, {"bullet", "•"},
        {"cup", "∪"}, {"cap", "∩"}, {"setminus", "∖"}, {"oplus", "⊕"}, {"ominus", "⊖"}, {"otimes", "⊗"}, {"odot", "⊙"}, {"wedge", "∧"}, {"land", "∧"},
        {"vee", "∨"}, {"lor", "∨"}, {"neg", "¬"}, {"lnot", "¬"}, {"nabla", "∇"}, {"partial", "∂"}, {"infty", "∞"}, {"emptyset", "∅"}, {"varnothing", "∅"},
        {"forall", "∀"}, {"exists", "∃"}, {"nexists", "∄"}, {"aleph", "ℵ"}, {"hbar", "ℏ"}, {"ell", "ℓ"}, {"Re", "ℜ"}, {"Im", "ℑ"}, {"wp", "℘"},
        {"angle", "∠"}, {"triangle", "△"}, {"square", "□"}, {"top", "⊤"}, {"bot", "⊥"}, {"prime", "′"}, {"degree", "°"}, {"therefore", "∴"},
        {"because", "∵"}, {"ldots", "…"}, {"dots", "…"}, {"cdots", "⋯"}, {"vdots", "⋮"}, {"ddots", "⋱"}, {"surd", "√"},
        // relations
        {"le", "≤"}, {"leq", "≤"}, {"ge", "≥"}, {"geq", "≥"}, {"ne", "≠"}, {"neq", "≠"}, {"ll", "≪"}, {"gg", "≫"}, {"approx", "≈"}, {"equiv", "≡"},
        {"sim", "∼"}, {"simeq", "≃"}, {"cong", "≅"}, {"propto", "∝"}, {"prec", "≺"}, {"succ", "≻"}, {"preceq", "⪯"}, {"succeq", "⪰"}, {"in", "∈"},
        {"notin", "∉"}, {"ni", "∋"}, {"subset", "⊂"}, {"supset", "⊃"}, {"subseteq", "⊆"}, {"supseteq", "⊇"}, {"perp", "⊥"}, {"parallel", "∥"},
        {"mid", "∣"}, {"vdash", "⊢"}, {"models", "⊨"}, {"doteq", "≐"},
        // arrows
        {"to", "→"}, {"rightarrow", "→"}, {"gets", "←"}, {"leftarrow", "←"}, {"leftrightarrow", "↔"}, {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"},
        {"Leftrightarrow", "⇔"}, {"iff", "⟺"}, {"implies", "⟹"}, {"impliedby", "⟸"}, {"mapsto", "↦"}, {"longrightarrow", "⟶"}, {"longleftarrow", "⟵"},
        {"Longrightarrow", "⟹"}, {"Longleftarrow", "⟸"}, {"longmapsto", "⟼"}, {"uparrow", "↑"}, {"downarrow", "↓"}, {"Uparrow", "⇑"}, {"Downarrow", "⇓"},
        {"nearrow", "↗"}, {"searrow", "↘"}, {"hookrightarrow", "↪"}, {"rightleftharpoons", "⇌"},
        // delimiters
        {"langle", "⟨"}, {"rangle", "⟩"}, {"lfloor", "⌊"}, {"rfloor", "⌋"}, {"lceil", "⌈"}, {"rceil", "⌉"}, {"lvert", "|"}, {"rvert", "|"}, {"vert", "|"},
        {"lVert", "‖"}, {"rVert", "‖"}, {"Vert", "‖"}, {"|", "‖"}, {"lbrace", "{"}, {"rbrace", "}"}, {"{", "{"}, {"}", "}"},
        // functions' names
        {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"}, {"sec", "sec"}, {"csc", "csc"}, {"cot", "cot"}, {"arcsin", "arcsin"}, {"arccos", "arccos"},
        {"arctan", "arctan"}, {"sinh", "sinh"}, {"cosh", "cosh"}, {"tanh", "tanh"}, {"log", "log"}, {"lg", "lg"}, {"ln", "ln"}, {"exp", "exp"},
        {"lim", "lim"}, {"liminf", "lim inf"}, {"limsup", "lim sup"}, {"sup", "sup"}, {"inf", "inf"}, {"max", "max"}, {"min", "min"}, {"arg", "arg"},
        {"det", "det"}, {"dim", "dim"}, {"gcd", "gcd"}, {"ker", "ker"}, {"deg", "deg"}, {"hom", "hom"}, {"Pr", "Pr"}, {"mod", "mod"}, {"bmod", "mod"}, {"colon", ":"},
        // spacing, and characters TeX takes for its own
        {"quad", "  "}, {"qquad", "    "}, {",", " "}, {";", " "}, {":", " "}, {">", " "}, {" ", " "}, {"!", ""}, {"\\", " "},
        {"$", "$"}, {"%", "%"}, {"&", "&"}, {"_", "_"}, {"#", "#"}};
    // (Sizing, and the like: dropped.)
    static const std::string_view kDropped[] = {"left", "right", "big", "Big", "bigg", "Bigg", "bigl", "bigr", "Bigl", "Bigr", "biggl", "biggr",
                                                "displaystyle", "textstyle", "scriptstyle", "limits", "nolimits", "middle"};
    // (Fonts whose text is drawn as math; and those whose text is words, kept as it is.)
    static const std::string_view kFonts[] = {"mathbf", "mathit", "mathsf", "mathtt", "mathcal", "mathscr", "mathfrak", "boldsymbol", "bm", "mathnormal"};
    static const std::string_view kWordFonts[] = {"text", "textrm", "textit", "textbf", "textsf", "texttt", "mathrm", "operatorname", "mbox", "textnormal"};
    // (Accents over one character: combining marks.)
    static const std::pair<std::string_view, std::string_view> kAccents[] = {{"hat", "\xCC\x82"},   {"widehat", "\xCC\x82"}, {"bar", "\xCC\x84"},
                                                                            {"overline", "\xCC\x85"}, {"vec", "\xE2\x83\x97"}, {"dot", "\xCC\x87"},
                                                                            {"ddot", "\xCC\x88"}, {"tilde", "\xCC\x83"}, {"widetilde", "\xCC\x83"}};
    // A group ({...}, or one character, or a command) from `at`, and where it ends.
    auto group = [&](size_t at, size_t* end) -> std::string_view {
        while (at < tex.size() && tex[at] == ' ') ++at;
        if (at >= tex.size()) return *end = at, std::string_view();
        if (tex[at] == '{') {
            int nest = 0;
            for (size_t k = at; k < tex.size(); ++k) {
                if (tex[k] == '\\') ++k;
                else if (tex[k] == '{') ++nest;
                else if (tex[k] == '}' && --nest == 0) return *end = k + 1, tex.substr(at + 1, k - at - 1);
            }
            return *end = tex.size(), tex.substr(at + 1);
        }
        size_t k = at + 1;
        if (tex[at] == '\\') {
            while (k < tex.size() && std::isalpha(static_cast<unsigned char>(tex[k]))) ++k;
            if (k == at + 1 && k < tex.size()) ++k;
        } else {
            while (k < tex.size() && is_cont(static_cast<u8>(tex[k]))) ++k;
        }
        return *end = k, tex.substr(at, k - at);
    };
    auto inner = [&](std::string_view g) {
        std::string s;
        tex_into(g, glyphs, depth + 1, s);
        return s;
    };
    // (one thing, which needs no brackets round it after a / or a √)
    auto atom = [](const std::string& s) { return !s.empty() && s.find_first_of(" +-*/=,<>") == std::string::npos && s.find("\xE2\x88\x92") == std::string::npos; };
    auto chars = [](const std::string& s) {
        size_t n = 0;
        for (const char ch : s) n += !is_cont(static_cast<u8>(ch));
        return n;
    };
    for (size_t i = 0; i < tex.size();) {
        const char c = tex[i];
        if (c == '\\') {
            size_t k = i + 1;
            while (k < tex.size() && std::isalpha(static_cast<unsigned char>(tex[k]))) ++k;
            if (k == i + 1 && k < tex.size()) ++k;  // (a symbol's: \, \{)
            const std::string_view word = tex.substr(i + 1, k - i - 1);
            i = k;
            size_t e1 = i, e2 = i;
            if (word == "frac" || word == "dfrac" || word == "tfrac" || word == "cfrac") {
                const std::string top = inner(group(i, &e1)), bottom = inner(group(e1, &e2));
                out += (atom(top) ? top : "(" + top + ")") + "/" + (atom(bottom) ? bottom : "(" + bottom + ")");
                i = e2;
            } else if (word == "sqrt") {
                std::string root = "\xE2\x88\x9A";
                size_t at = i;
                while (at < tex.size() && tex[at] == ' ') ++at;
                if (at < tex.size() && tex[at] == '[') {  // (its degree: ∛, ∜, or raised before √)
                    const size_t close = tex.find(']', at);
                    const std::string n = inner(tex.substr(at + 1, (close == std::string_view::npos ? tex.size() : close) - at - 1));
                    i = close == std::string_view::npos ? tex.size() : close + 1;
                    if (n == "3") root = "\xE2\x88\x9B";
                    else if (n == "4") root = "\xE2\x88\x9C";
                    else if (std::string up; scripted(n, true, glyphs, &up)) root = up + root;
                    else root = n + root;
                }
                const std::string body = inner(group(i, &e1));
                out += root + (atom(body) ? body : "(" + body + ")");
                i = e1;
            } else if (word == "mathbb") {  // ℝ, ℕ: the double-struck letters
                const std::string_view g = group(i, &e1);
                i = e1;
                for (const char ch : g) {
                    static const std::pair<char, u32> kOwn[] = {{'C', 0x2102}, {'H', 0x210D}, {'N', 0x2115}, {'P', 0x2119}, {'Q', 0x211A}, {'R', 0x211D}, {'Z', 0x2124}};
                    u32 cp = 0;
                    for (const auto& [l, to] : kOwn)
                        if (l == ch) cp = to;
                    if (!cp && ch >= 'A' && ch <= 'Z') cp = 0x1D538 + static_cast<u32>(ch - 'A');
                    else if (!cp && ch >= 'a' && ch <= 'z') cp = 0x1D552 + static_cast<u32>(ch - 'a');
                    else if (!cp && ch >= '0' && ch <= '9') cp = 0x1D7D8 + static_cast<u32>(ch - '0');
                    if (cp) encode_utf8(cp, out);
                    else if (ch != ' ') out += ch;
                }
            } else if (std::ranges::find(kWordFonts, word) != std::end(kWordFonts)) {
                out += group(i, &e1);
                i = e1;
            } else if (std::ranges::find(kFonts, word) != std::end(kFonts)) {
                out += inner(group(i, &e1));
                i = e1;
            } else if (const auto accent = std::ranges::find_if(kAccents, [&](const auto& a) { return a.first == word; }); accent != std::end(kAccents)) {
                const std::string_view g = group(i, &e1);
                const std::string body = inner(g);
                i = e1;
                if (chars(body) == 1) out += body + std::string(accent->second);
                else out += "\\" + std::string(word) + "{" + body + "}";
            } else if (word == "begin" || word == "end") {  // (an environment's name goes; its rows are rows)
                group(i, &e1);
                i = e1;
            } else if (word == "binom" || word == "dbinom" || word == "tbinom") {
                const std::string top = inner(group(i, &e1)), bottom = inner(group(e1, &e2));
                out += "C(" + top + "," + bottom + ")";
                i = e2;
            } else if (word == "pmod") {
                out += "(mod " + inner(group(i, &e1)) + ")";
                i = e1;
            } else if (std::ranges::find(kDropped, word) != std::end(kDropped)) {
                if (i < tex.size() && std::strchr("()[]|./<>", tex[i])) {
                    if (tex[i] != '.') out += tex[i] == '<' ? "\xE2\x9F\xA8" : tex[i] == '>' ? "\xE2\x9F\xA9" : std::string(1, tex[i]);
                    ++i;
                }
            } else if (const auto it = kWords.find(word); it != kWords.end()) {
                out += it->second;
                if (word.size() > 1 && std::isalpha(static_cast<unsigned char>(word[0])) && i < tex.size() && std::isalnum(static_cast<unsigned char>(tex[i]))) out += ' ';
            } else {  // (not known: as it is, its arguments' braces kept)
                out += '\\';
                out += word;
                while (i < tex.size() && tex[i] == '{') {
                    out += "{" + inner(group(i, &e1)) + "}";
                    i = e1;
                }
            }
            continue;
        }
        if (c == '^' || c == '_') {
            size_t e = i + 1;
            const std::string_view g = group(i + 1, &e);
            i = e;
            if (c == '^' && (g == "\\circ" || g == "{\\circ}")) {
                out += "\xC2\xB0";
                continue;
            }
            const std::string body = inner(g);
            if (!scripted(body, c == '^', glyphs, &out)) out += script_mark(c, body);
            continue;
        }
        if (c == '{' || c == '}' || c == '&') {
            ++i;
            continue;
        }
        if (c == '~') {
            out += ' ';
            ++i;
            continue;
        }
        if (c == '-') {  // (a minus sign, not a hyphen)
            out += "\xE2\x88\x92";
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
}

// What parse_inline looks for in a piece of text again and again, each
// found once (on first use): the marks that close others, brackets'
// partners, runs of backticks, the next of a few characters. (A long line
// of marks that close nothing, thousands of `*`, `[` or `<`, once took time
// with the square of its length: each mark looked to its end.)
// The kinds of inline HTML whose closing tag Marks finds: parse_inline's
// tags (<b>, <kbd>, ...), then a comment's "-->".
constexpr size_t kCommentTag = 15, kTagKinds = kCommentTag + 1;

class Marks {
  public:
    explicit Marks(std::string_view s) : s_(s) { std::ranges::fill(tags_, kUnknown); }

    // Where the run of c (exactly n long) that closes one opened before `from`
    // is, or npos. (Most are a few words on. Once a scan goes far, what a scan
    // finds from each place it passes is kept, and the next to get there stops.)
    size_t closer(size_t from, char c, size_t n) {
        std::vector<size_t>& found_from = closers_[c == '*' ? n - 1 : c == '_' ? n + 2 : c == '~' ? 6 : 7];
        path_.clear();
        size_t steps = 0;
        size_t found = std::string_view::npos;
        for (size_t j = from; j < s_.size();) {
            if (j > from) {
                if (found_from.empty()) {
                    if (++steps > kNear) {
                        found_from.assign(s_.size(), kUnknown);
                        return closer(from, c, n);
                    }
                } else if (found_from[j] != kUnknown) {
                    found = found_from[j];
                    break;
                } else {
                    path_.push_back(j);
                }
            }
            if (s_[j] == '\\') {
                j += 2;
                continue;
            }
            if (s_[j] == '`') {  // (not inside code)
                size_t k = j;
                while (k < s_.size() && s_[k] == '`') ++k;
                const size_t end = ticks(k, k - j);
                j = end != std::string_view::npos ? end + (k - j) : k;
                continue;
            }
            if (s_[j] != c) {
                ++j;
                continue;
            }
            size_t k = j;
            while (k < s_.size() && s_[k] == c) ++k;
            const size_t run = k - j;
            const bool after_space = j > 0 && (s_[j - 1] == ' ' || s_[j - 1] == '\t');
            const bool word_after = c == '_' && k < s_.size() && std::isalnum(static_cast<unsigned char>(s_[k]));
            if (run >= n && !after_space && !word_after && j > from) {
                found = j + (run - n);
                break;
            }
            j = k;
        }
        for (const size_t j : path_) found_from[j] = found;
        return found;
    }

    // The ] or ) that closes the [ or ( at i (brackets inside may nest), or
    // npos. (Looked for, while it is near; past that, every one is paired at once.)
    size_t bracket_end(size_t i, char open) {
        std::vector<size_t>& ends = open == '[' ? square_ : round_;
        const char close = open == '[' ? ']' : ')';
        if (ends.empty()) {
            const size_t near = std::min(s_.size(), i + kNear);
            int depth = 0;
            for (size_t j = i; j < near; ++j) {
                if (s_[j] == '\\') {
                    ++j;
                    continue;
                }
                if (s_[j] == open) ++depth;
                else if (s_[j] == close && --depth == 0) return j;
            }
            if (near == s_.size()) return std::string_view::npos;
            ends.assign(s_.size(), std::string_view::npos);
            std::vector<size_t> opened;
            for (size_t j = 0; j < s_.size(); ++j) {
                if (s_[j] == '\\') {
                    ++j;
                    continue;
                }
                if (s_[j] == open) {
                    opened.push_back(j);
                } else if (s_[j] == close && !opened.empty()) {
                    ends[opened.back()] = j;
                    opened.pop_back();
                }
            }
        }
        return ends[i];
    }

    // Where the first run of backticks at least n long starts, at k (the end
    // of a run) or after, or npos: s.find(std::string(n, '`'), k).
    size_t ticks(size_t k, size_t n) {
        if (!ticks_found_) {  // (looked for, while near; past that, every run is found at once)
            const size_t near = std::min(s_.size(), k + kNear);
            for (size_t j = s_.find('`', k); j < near;) {
                size_t e = j;
                while (e < s_.size() && s_[e] == '`') ++e;
                if (e - j >= n) return j;
                j = s_.find('`', e);
            }
            if (near == s_.size()) return std::string_view::npos;
            ticks_found_ = true;
            for (size_t j = s_.find('`'); j != std::string_view::npos; j = s_.find('`', j)) {
                size_t e = j;
                while (e < s_.size() && s_[e] == '`') ++e;
                tick_at_.push_back(j);
                tick_len_.push_back(e - j);
                j = e;
            }
            // (and for each run, the next one longer: the runs between it and that are no answer where it is not)
            tick_longer_.resize(tick_at_.size());
            std::vector<size_t> later;
            for (size_t r = tick_at_.size(); r-- > 0;) {
                while (!later.empty() && tick_len_[later.back()] <= tick_len_[r]) later.pop_back();
                tick_longer_[r] = later.empty() ? tick_at_.size() : later.back();
                later.push_back(r);
            }
        }
        size_t r = static_cast<size_t>(std::lower_bound(tick_at_.begin(), tick_at_.end(), k) - tick_at_.begin());
        while (r < tick_at_.size() && tick_len_[r] < n) r = tick_longer_[r];
        return r < tick_at_.size() ? tick_at_[r] : std::string_view::npos;
    }

    // The first ch (one of kNext) at `from` or after, asked with `from` never going back.
    size_t next(char ch, size_t from) {
        size_t& at = next_[std::strchr(kNext, ch) - kNext];
        if (at == kUnknown || (at != std::string_view::npos && at < from)) at = s_.find(ch, from);
        return at;
    }

    // The first `what` (one of kFound) at `from` or after, asked with `from` never going back.
    size_t found(std::string_view what, size_t from) {
        size_t& at = found_[what == kFound[0] ? 0 : 1];
        if (at == kUnknown || (at != std::string_view::npos && at < from)) at = s_.find(what, from);
        return at;
    }

    // The first `what` at `from` or after, `what` being the closing tag of
    // inline HTML's kind `kind` (each kind asked with `from` never going back;
    // the kinds are parse_inline's kTags, then kCommentTag for "-->").
    size_t tag_close(size_t kind, std::string_view what, size_t from) {
        size_t& at = tags_[kind];
        if (at == kUnknown || (at != std::string_view::npos && at < from)) at = s_.find(what, from);
        return at;
    }

    // The first $ at `from` or after that can close math ($x$): not after a
    // space or a backslash, not before a digit (so "$5 and $6" is not math),
    // not one of two. Asked with `from` never going back.
    size_t dollar_close(size_t from) {
        if (dollar_ == kUnknown || (dollar_ != std::string_view::npos && dollar_ < from)) {
            dollar_ = std::string_view::npos;
            for (size_t j = s_.find('$', from); j != std::string_view::npos; j = s_.find('$', j + 1)) {
                const char before = j > 0 ? s_[j - 1] : ' ';
                const char after = j + 1 < s_.size() ? s_[j + 1] : ' ';
                if (before != ' ' && before != '\t' && before != '\\' && before != '$' && after != '$' && !std::isdigit(static_cast<unsigned char>(after))) {
                    dollar_ = j;
                    break;
                }
            }
        }
        return dollar_;
    }

    // Where what is before the > at gt ends, the spaces, tabs and returns in front of it left out.
    size_t trimmed_end(size_t gt) {
        if (trimmed_for_ != gt) {
            trimmed_for_ = gt;
            trimmed_ = gt;
            while (trimmed_ > 0 && (s_[trimmed_ - 1] == ' ' || s_[trimmed_ - 1] == '\t' || s_[trimmed_ - 1] == '\r')) --trimmed_;
        }
        return trimmed_;
    }

  private:
    static constexpr size_t kUnknown = std::string_view::npos - 1;
    static constexpr size_t kNear = 256;  // how far a scan looks before what it finds is kept for the next
    static constexpr const char* kNext = "<>:@ `\n";
    static constexpr std::string_view kFound[] = {"$$", "\\)"};
    std::string_view s_;
    std::vector<size_t> closers_[8];  // for each mark (*, **, ***, _, __, ___, ~~, ==), what a scan from each place finds
    std::vector<size_t> path_;
    std::vector<size_t> square_, round_;
    bool ticks_found_ = false;
    std::vector<size_t> tick_at_, tick_len_, tick_longer_;
    size_t next_[7] = {kUnknown, kUnknown, kUnknown, kUnknown, kUnknown, kUnknown, kUnknown};
    size_t found_[2] = {kUnknown, kUnknown};
    size_t dollar_ = kUnknown;
    size_t tags_[kTagKinds];
    size_t trimmed_for_ = std::string_view::npos, trimmed_ = 0;
};

void parse_inline(std::string_view s, const Span& as, const Refs& refs, std::vector<Span>& out) {
    static thread_local int depth = 0;
    if (depth >= kMostDepth) {  // (nested deeper than anyone writes: as it is)
        Span sp = as;
        sp.text = std::string(s);
        out.push_back(std::move(sp));
        return;
    }
    struct Deeper {
        int& d;
        explicit Deeper(int& x) : d(++x) {}
        ~Deeper() { --d; }
    } deeper(depth);
    std::string buf;
    auto flush = [&] {
        if (buf.empty()) return;
        Span sp = as;
        sp.text = std::move(buf);
        out.push_back(std::move(sp));
        buf.clear();
    };
    // Where something is taken out (a comment, a tag), one space after it
    // goes too when there is one before: "a <!-- x --> b" is "a b".
    auto past_gap = [&](size_t at) {
        const bool space_before = !buf.empty() ? buf.back() == ' ' : !out.empty() && out.back().text.ends_with(' ');
        return space_before && at < s.size() && s[at] == ' ' ? at + 1 : at;
    };
    auto link_span = [&](std::string_view text, const std::string& url) {
        flush();
        Span inner = as;
        inner.ink = kMdLink;
        inner.link = url;
        parse_inline(text, inner, refs, out);
    };
    // Math ($x$, $$x$$, $`x`$, \(x\)): its TeX as text, slanted.
    auto math = [&](std::string_view tex) {
        flush();
        Span sp = as;
        sp.text = md_math_text(tex, refs.glyphs);
        sp.style |= kMdItalic;
        if (!sp.text.empty()) out.push_back(std::move(sp));
    };
    Marks marks(s);
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        // (Math does not run into code, nor past a line's end; GitHub's rules
        // for $: no space just inside the marks, no digit just after.)
        const auto math_to = [&](size_t open, size_t close) {
            return close != std::string_view::npos && close > open && marks.next('`', open) > close && marks.next('\n', open) > close;
        };
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == '(') {
            if (const size_t close = marks.found("\\)", i + 2); math_to(i + 2, close)) {
                math(s.substr(i + 2, close - i - 2));
                i = close + 2;
                continue;
            }
        }
        if (c == '$' && i + 1 < s.size()) {
            if (s[i + 1] == '`') {  // ($`x`$: the math in a code span's marks)
                size_t k = i + 1;
                while (k < s.size() && s[k] == '`') ++k;
                const size_t end = marks.ticks(k, k - i - 1);
                if (end != std::string_view::npos && end + (k - i - 1) < s.size() && s[end + (k - i - 1)] == '$') {
                    math(trim(s.substr(k, end - k)));
                    i = end + (k - i - 1) + 1;
                    continue;
                }
            } else if (s[i + 1] == '$') {
                if (const size_t close = marks.found("$$", i + 2); math_to(i + 2, close)) {
                    math(s.substr(i + 2, close - i - 2));
                    i = close + 2;
                    continue;
                }
            } else if (s[i + 1] != ' ' && s[i + 1] != '\t' && s[i + 1] != '\n') {
                if (const size_t close = marks.dollar_close(i + 2); math_to(i + 1, close)) {
                    math(s.substr(i + 1, close - i - 1));
                    i = close + 1;
                    continue;
                }
            }
        }
        if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            buf += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '\n') {
            flush();
            Span br = as;
            br.brk = true;
            out.push_back(br);
            ++i;
            continue;
        }
        if (c == '`') {
            size_t k = i;
            while (k < s.size() && s[k] == '`') ++k;
            const std::string_view run = s.substr(i, k - i);
            const size_t end = marks.ticks(k, run.size());
            if (end == std::string_view::npos || (end + run.size() < s.size() && s[end + run.size()] == '`')) {
                buf += run;
                i = k;
                continue;
            }
            std::string code(s.substr(k, end - k));
            for (char& ch : code)
                if (ch == '\n') ch = ' ';
            if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos) code = code.substr(1, code.size() - 2);
            flush();
            Span sp = as;
            sp.text = code;
            sp.ink = as.link.empty() ? kMdCode : kMdLink;
            sp.tone = kMdCodeTone;
            sp.style &= static_cast<u8>(~kMdItalic);
            out.push_back(std::move(sp));
            i = end + run.size();
            continue;
        }
        if (c == '<') {
            const size_t end = marks.next('>', i);
            if (end != std::string_view::npos) {
                const std::string_view inner = s.substr(i + 1, end - i - 1);
                // (what is between, trimmed, of which no more is copied than tells a <br> or an <img>: each of a
                // thousand < before one > once copied all there was up to it)
                size_t from = i + 1;
                while (from < end && (s[from] == ' ' || s[from] == '\t')) ++from;
                const size_t to = std::max(from, marks.trimmed_end(end));
                const std::string tag = lower(s.substr(from, std::min<size_t>(to - from, 5)));
                if (to - from <= 4 && (tag == "br" || tag == "br/" || tag == "br /")) {
                    flush();
                    Span br = as;
                    br.brk = true;
                    out.push_back(br);
                    i = end + 1;
                    continue;
                }
                const auto in_it = [&](char ch) { return marks.next(ch, i + 1) < end; };
                const size_t colon = in_it(':') ? marks.next(':', i + 1) - i - 1 : std::string_view::npos;
                const bool url = colon != std::string_view::npos && colon > 1 && !in_it(' ') &&
                                 std::all_of(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(colon), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '+' || ch == '.' || ch == '-'; });
                const bool mail = !url && in_it('@') && !in_it(' ') && !inner.empty();
                if (url || mail) {
                    link_span(inner, url ? std::string(inner) : "mailto:" + std::string(inner));
                    i = end + 1;
                    continue;
                }
                // A picture: <img src="..." alt="..." width="...">.
                if (tag.starts_with("img") && (tag.size() == 3 || tag[3] == ' ' || tag[3] == '\t' || tag[3] == '\n' || tag[3] == '/')) {
                    const std::string src = attribute(inner, "src"), alt = attribute(inner, "alt");
                    if (!src.empty()) {
                        flush();
                        Span pic = as;
                        pic.text = "\xE2\x96\xA3 " + (alt.empty() ? std::string("image") : alt);
                        pic.ink = kMdLink;
                        pic.link = as.link.empty() ? src : as.link;
                        pic.image = src;
                        pic.image_w = attribute_px(inner, "width");
                        pic.image_h = attribute_px(inner, "height");
                        out.push_back(std::move(pic));
                    }
                    i = end + 1;
                    continue;
                }
                // Inline HTML that says how its text looks, to its closing tag:
                // <b>, <i>, <u>, <s>, <code> as their Markdown is; <kbd> a key;
                // <sub> and <sup> lowered and raised; <mark> highlighted; <a href> a link.
                static constexpr std::string_view kTags[] = {"b", "strong", "i", "em", "u", "ins", "s", "del", "strike", "code", "kbd", "sub", "sup", "mark", "a"};
                static_assert(std::size(kTags) == kCommentTag);
                size_t name_end = from;
                while (name_end < end && name_end - from < 8 && std::isalnum(static_cast<unsigned char>(s[name_end]))) ++name_end;
                const std::string name = lower(s.substr(from, name_end - from));
                const size_t kind = static_cast<size_t>(std::ranges::find(kTags, name) - std::begin(kTags));
                if (kind < std::size(kTags) && (name_end == end || s[name_end] == ' ' || s[name_end] == '\t' || s[name_end] == '\n')) {
                    const std::string close = "</" + name + ">";
                    if (const size_t at = marks.tag_close(kind, close, end + 1); at != std::string_view::npos) {
                        flush();
                        const std::string_view body = s.substr(end + 1, at - end - 1);
                        Span look = as;
                        if (name == "b" || name == "strong") look.style |= kMdBold;
                        else if (name == "i" || name == "em") look.style |= kMdItalic;
                        else if (name == "u" || name == "ins") look.style |= kMdUnderline;
                        else if (name == "s" || name == "del" || name == "strike") look.ink = kMdMuted, look.style |= kMdStrike;
                        else if (name == "code") look.ink = as.link.empty() ? kMdCode : kMdLink, look.tone = kMdCodeTone, look.style &= static_cast<u8>(~kMdItalic);
                        else if (name == "mark") look.tone = kMdMarkTone;
                        if (name == "a") {
                            const std::string href = attribute(inner, "href");
                            if (href.empty()) parse_inline(body, as, refs, out);
                            else link_span(body, href);
                        } else if (name == "kbd") {  // (a key: on the code's tone, a space each side that does not break)
                            look.tone = kMdCodeTone;
                            Span edge = look;
                            edge.text = "\xC2\xA0";
                            out.push_back(edge);
                            parse_inline(body, look, refs, out);
                            out.push_back(edge);
                        } else if (name == "sub" || name == "sup") {  // (in Unicode's raised or lowered characters where it has them all, else ^x, _x)
                            std::vector<Span> said;
                            parse_inline(body, as, refs, said);
                            std::string text;
                            for (const Span& sp : said) text += sp.text;
                            Span sp = as;
                            if (!scripted(text, name == "sup", refs.glyphs, &sp.text)) sp.text = script_mark(name == "sup" ? '^' : '_', text);
                            out.push_back(std::move(sp));
                        } else {
                            parse_inline(body, look, refs, out);
                        }
                        i = at + close.size();
                        continue;
                    }
                }
                // A comment goes, to its end.
                if (s.substr(i, 4) == "<!--") {
                    const size_t close = marks.tag_close(kCommentTag, "-->", i + 4);
                    if (close != std::string_view::npos) {
                        i = past_gap(close + 3);
                        continue;
                    }
                }
                // Inline HTML (<span>, </b> ...): its tags go, its text stays.
                if (!inner.empty() && (std::isalpha(static_cast<unsigned char>(inner[0])) || inner[0] == '/') && !in_it('<')) {
                    i = past_gap(end + 1);
                    continue;
                }
            }
        }
        if ((c == '!' && i + 1 < s.size() && s[i + 1] == '[') || c == '[') {
            const bool image = c == '!';
            const size_t open = image ? i + 1 : i;
            const size_t close = marks.bracket_end(open, '[');
            if (close != std::string_view::npos) {
                const std::string_view text = s.substr(open + 1, close - open - 1);
                std::string url;
                size_t next = std::string_view::npos;
                if (close + 1 < s.size() && s[close + 1] == '(') {
                    const size_t end = marks.bracket_end(close + 1, '(');
                    if (end != std::string_view::npos) {
                        std::string_view dest = trim(s.substr(close + 2, end - close - 2));
                        if (dest.starts_with('<')) dest = dest.substr(1, dest.find('>') == std::string_view::npos ? dest.size() - 1 : dest.find('>') - 1);
                        else if (const size_t sp = dest.find_first_of(" \t"); sp != std::string_view::npos) dest = dest.substr(0, sp);  // (and a title after it)
                        url = std::string(dest);
                        next = end + 1;
                    }
                } else {
                    std::string_view label = text;
                    size_t end = close + 1;
                    if (close + 1 < s.size() && s[close + 1] == '[') {
                        const size_t e2 = s.find(']', close + 2);
                        if (e2 != std::string_view::npos) {
                            if (e2 > close + 2) label = s.substr(close + 2, e2 - close - 2);
                            end = e2 + 1;
                        }
                    }
                    // (text longer than any label is not looked up: brackets nested thousands deep each copied what they hold)
                    if (const auto it = label.size() <= refs.longest ? refs.to.find(lower(label)) : refs.to.end(); it != refs.to.end()) {
                        url = it->second;
                        next = end;
                    } else if (const int n = !image && text.starts_with('^') && text.size() - 1 <= refs.longest_note ? refs.number(lower(text.substr(1))) : 0) {
                        // A footnote's mark: its number, raised, a link to what it says. (One with nothing defined stays as it is.)
                        flush();
                        Span note = as;
                        note.text = refs.glyphs & kMdSuperDigits ? raised(n) : "[" + std::to_string(n) + "]";
                        note.ink = kMdLink;
                        note.style = 0;
                        note.link = "#fn-" + std::to_string(n);
                        out.push_back(std::move(note));
                        i = close + 1;
                        continue;
                    }
                }
                if (next != std::string_view::npos) {
                    if (image) {  // (what stands for it where it is not drawn: what it shows; a click opens it, or follows the link it is in)
                        flush();
                        Span pic = as;
                        pic.text = "\xE2\x96\xA3 " + (text.empty() ? std::string("image") : std::string(text));
                        pic.ink = kMdLink;
                        pic.link = as.link.empty() ? url : as.link;
                        pic.image = url;
                        out.push_back(std::move(pic));
                    } else {
                        link_span(text, url);
                    }
                    i = next;
                    continue;
                }
            }
        }
        if (c == '*' || c == '_' || ((c == '~' || c == '=') && i + 1 < s.size() && s[i + 1] == c)) {
            size_t k = i;
            while (k < s.size() && s[k] == c) ++k;
            const size_t run = std::min<size_t>(k - i, c == '~' || c == '=' ? 2 : 3);
            const bool space_after = k >= s.size() || s[k] == ' ' || s[k] == '\t' || s[k] == '\n';
            const bool intraword = (c == '_' || c == '=') && i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1]));  // ("a==b" is no highlight)
            if (!space_after && !intraword && ((c != '~' && c != '=') || k - i == 2)) {
                const size_t end = marks.closer(i + run, c, run);
                if (end != std::string_view::npos) {
                    flush();
                    Span inner = as;
                    if (c == '~') inner.ink = kMdMuted, inner.style |= kMdStrike;  // (struck out: a line through, and quieter)
                    else if (c == '=') inner.tone = kMdMarkTone;  // (==highlighted==)
                    else inner.style |= static_cast<u8>(run == 1 ? kMdItalic : run == 2 ? kMdBold : kMdBold | kMdItalic);
                    parse_inline(s.substr(i + run, end - i - run), inner, refs, out);
                    i = end + run;
                    continue;
                }
            }
            buf.append(s.substr(i, k - i));
            i = k;
            continue;
        }
        if (c == '&') {
            size_t len = 0;
            const std::string e = entity(s.substr(i), &len);
            if (len) {
                buf += e;
                i += len;
                continue;
            }
        }
        if ((c == 'h' || c == 'w') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '(') && as.link.empty() &&
            (s.substr(i).starts_with("https://") || s.substr(i).starts_with("http://") || s.substr(i).starts_with("www."))) {
            size_t end = i;
            while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != '\n' && s[end] != '<') ++end;
            while (end > i && std::strchr(".,:;!?)'\"", s[end - 1])) --end;  // (the sentence's, not the address's)
            const std::string url(s.substr(i, end - i));
            link_span(url, url.starts_with("www.") ? "https://" + url : url);
            i = end;
            continue;
        }
        buf += c;
        ++i;
    }
    flush();
}

// ---------------------------------------------------------------- blocks

struct Line {
    std::string_view text;
    u32 no = 0;
};

struct Block {
    enum Kind { kPara, kHeading, kCode, kQuote, kList, kItem, kTable, kRule, kHtml, kNote, kMath, kTerm } kind = kPara;  // (kNote: a footnote's text, its label in `info`; kMath: display math's TeX; kTerm: a term, its definitions its kids)
    u32 line = 0;
    int level = 0;
    std::vector<std::string> text;
    std::vector<u32> nos;
    std::string info;
    std::vector<Block> kids;
    bool ordered = false, loose = false;
    int start = 1, task = -1;
    std::vector<std::vector<std::string>> table;  // the header first
    std::vector<int> align;                       // -1 left, 0 centre, 1 right
};

bool is_rule(std::string_view s) {
    s = trim(s);
    if (s.size() < 3 || (s[0] != '-' && s[0] != '*' && s[0] != '_')) return false;
    int n = 0;
    for (const char c : s) {
        if (c == s[0]) ++n;
        else if (c != ' ' && c != '\t') return false;
    }
    return n >= 3;
}

// An alert's kind (GitHub's "[!NOTE]" and its like, alone on a quote's first
// line, in any case): 1 note, 2 tip, 3 important, 4 warning, 5 caution; 0 for
// anything else (a quote that starts with a bracket).
int alert_kind(std::string_view s) {
    s = trim(s);
    if (s.size() < 6 || s.size() > 12 || !s.starts_with("[!") || s.back() != ']') return 0;
    static const char* const kKinds[] = {"note", "tip", "important", "warning", "caution"};
    const std::string name = lower(s.substr(2, s.size() - 3));
    for (int k = 0; k < 5; ++k)
        if (name == kKinds[k]) return k + 1;
    return 0;
}

// A footnote's definition ("[^label]: what it says"): its label (lower
// case) and where its text starts in s. False for anything else.
bool note_start(std::string_view s, std::string* label = nullptr, size_t* text = nullptr) {
    if (!s.starts_with("[^")) return false;
    const size_t close = s.find(']');
    if (close == std::string_view::npos || close < 3 || close + 1 >= s.size() || s[close + 1] != ':') return false;
    const std::string_view name = s.substr(2, close - 2);
    if (name.find_first_of(" \t[") != std::string_view::npos) return false;
    if (label) *label = lower(name);
    if (text) *text = close + 2;
    return true;
}

bool is_fence(std::string_view s, char* fc, size_t* len) {
    s = trim(s);
    if (s.size() < 3 || (s[0] != '`' && s[0] != '~')) return false;
    size_t k = 0;
    while (k < s.size() && s[k] == s[0]) ++k;
    if (k < 3) return false;
    if (s[0] == '`' && s.substr(k).find('`') != std::string_view::npos) return false;
    *fc = s[0];
    *len = k;
    return true;
}

int atx_level(std::string_view s) {
    int n = 0;
    while (n < static_cast<int>(s.size()) && s[static_cast<size_t>(n)] == '#') ++n;
    if (n < 1 || n > 6) return 0;
    if (static_cast<size_t>(n) < s.size() && s[static_cast<size_t>(n)] != ' ' && s[static_cast<size_t>(n)] != '\t') return 0;
    return n;
}

// A list item's marker: whether it is ordered, its number, its kind
// ('-', '*', '+', '.', ')'), and the column its text starts at.
struct Marker {
    bool ordered = false;
    int number = 0;
    char kind = 0;
    int content = 0;
};
bool list_marker(std::string_view line, Marker* m) {
    const int ind = indent_of(line);
    if (ind > 3) return false;
    const std::string s = strip_cols(line, ind);
    size_t k = 0;
    if (!s.empty() && (s[0] == '-' || s[0] == '*' || s[0] == '+')) {
        m->ordered = false;
        m->kind = s[0];
        k = 1;
    } else {
        while (k < s.size() && k < 9 && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        if (k == 0 || k >= s.size() || (s[k] != '.' && s[k] != ')')) return false;
        m->ordered = true;
        m->number = std::atoi(s.substr(0, k).c_str());
        m->kind = s[k];
        ++k;
    }
    if (k < s.size() && s[k] != ' ' && s[k] != '\t') return false;
    int spaces = 0;
    size_t j = k;
    while (j < s.size() && s[j] == ' ') ++j, ++spaces;
    if (j >= s.size()) spaces = 1;          // (an empty item)
    else if (spaces > 4) spaces = 1;        // (code inside the item)
    m->content = ind + static_cast<int>(k) + spaces;
    return true;
}

std::vector<std::string> table_cells(std::string_view s) {
    s = trim(s);
    if (s.starts_with('|')) s.remove_prefix(1);
    if (s.ends_with('|') && !s.ends_with("\\|")) s.remove_suffix(1);
    std::vector<std::string> out;
    std::string cell;
    bool code = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (s[i] == '`') {
            code = !code;
            cell += '`';
        } else if (s[i] == '|' && !code) {
            out.emplace_back(trim(cell));
            cell.clear();
        } else {
            cell += s[i];
        }
    }
    out.emplace_back(trim(cell));
    return out;
}

bool table_delims(std::string_view s, std::vector<int>* align) {
    if (s.find('-') == std::string_view::npos || (s.find('|') == std::string_view::npos && trim(s).find(' ') != std::string_view::npos)) return false;
    align->clear();
    for (const std::string& c : table_cells(s)) {
        if (c.empty()) return false;
        const bool left = c.front() == ':', right = c.back() == ':';
        const std::string_view mid = std::string_view(c).substr(left ? 1 : 0, c.size() - (left ? 1 : 0) - (right ? 1 : 0));
        if (mid.empty() || mid.find_first_not_of('-') != std::string_view::npos) return false;
        align->push_back(left && right ? 0 : right ? 1 : -1);
    }
    return !align->empty();
}

// Whether a line (trimmed) opens display math: "$$" or "\[" alone, or with
// the math after it and its close at the end ($$ x $$).
bool math_open(std::string_view s) {
    if (s == "$$" || s == "\\[") return true;
    return s.size() >= 4 && ((s.starts_with("$$") && s.ends_with("$$")) || (s.starts_with("\\[") && s.ends_with("\\]")));
}

// Whether a line starts a block of its own (and so ends a paragraph).
bool starts_block(std::string_view line) {
    if (blank(line) || indent_of(line) > 3) return false;
    const std::string s = strip_cols(line, indent_of(line));
    char fc = 0;
    size_t fl = 0;
    Marker m;
    if (is_fence(s, &fc, &fl) || atx_level(s) || is_rule(s) || s.starts_with('>') || math_open(trim(s))) return true;
    if (list_marker(line, &m)) return (!m.ordered || m.number == 1) && !blank(std::string_view(s).substr(std::min(s.size(), static_cast<size_t>(m.content - indent_of(line)))));
    return false;
}

// Whether a line is a definition (": what a term means", under the term).
bool is_definition(std::string_view s) {
    if (indent_of(s) > 3) return false;
    s = trim(s);
    return s == ":" || (s.size() > 1 && s[0] == ':' && (s[1] == ' ' || s[1] == '\t'));
}

// Whether a line starts a block of HTML (as CommonMark has it): a comment,
// a tag of a kind that makes blocks (<div>, <p>, <details>, <h1>, <table>
// ...), or any other tag alone on its line. A paragraph that starts with an
// inline tag (<kbd>, <b>) is a paragraph.
bool html_start(std::string_view s) {
    if (s.size() < 2 || s[0] != '<') return false;
    if (s[1] == '!') return true;
    const size_t from = s[1] == '/' ? 2 : 1;
    size_t end = from;
    while (end < s.size() && end - from < 12 && std::isalnum(static_cast<unsigned char>(s[end]))) ++end;
    if (end == from || !std::isalpha(static_cast<unsigned char>(s[from])) || (end < s.size() && !std::strchr(" \t>/", s[end]))) return false;
    static constexpr std::string_view kBlocks[] = {"address", "article", "aside", "blockquote", "body", "center", "dd", "details", "dialog", "div", "dl", "dt",
                                                   "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr",
                                                   "html", "li", "main", "nav", "ol", "p", "pre", "script", "section", "style", "summary", "table", "tbody",
                                                   "td", "tfoot", "th", "thead", "tr", "ul"};
    if (std::ranges::find(kBlocks, lower(s.substr(from, end - from))) != std::end(kBlocks)) return true;
    const std::string_view t = trim(s);
    return t.back() == '>' && t.find('>') == t.size() - 1;
}

std::vector<Block> parse_blocks(const std::vector<Line>& lines, Refs& refs);

void parse_into(const std::vector<Line>& lines, Refs& refs, std::vector<Block>& out) {
    const size_t n = lines.size();
    size_t i = 0;
    size_t no_math_to = 0;  // (display math opened before this line found no close before it: none opened before it will)
    while (i < n) {
        const std::string_view t = lines[i].text;
        const u32 no = lines[i].no;
        if (blank(t)) {
            ++i;
            continue;
        }
        const int ind = indent_of(t);
        if (ind >= 4) {  // indented code
            Block b;
            b.kind = Block::kCode;
            b.line = no;
            while (i < n && (blank(lines[i].text) || indent_of(lines[i].text) >= 4)) {
                b.text.push_back(blank(lines[i].text) ? std::string() : strip_cols(lines[i].text, 4));
                b.nos.push_back(lines[i].no);
                ++i;
            }
            while (!b.text.empty() && b.text.back().empty()) b.text.pop_back(), b.nos.pop_back();
            out.push_back(std::move(b));
            continue;
        }
        const std::string s = strip_cols(t, ind);
        char fc = 0;
        size_t flen = 0;
        if (is_fence(s, &fc, &flen)) {
            Block b;
            b.kind = Block::kCode;
            b.line = no;
            b.info = std::string(trim(std::string_view(s).substr(flen)));
            ++i;
            while (i < n) {
                char fc2 = 0;
                size_t flen2 = 0;
                const std::string_view l = lines[i].text;
                if (indent_of(l) <= 3 && is_fence(l, &fc2, &flen2) && fc2 == fc && flen2 >= flen && trim(trim(l).substr(flen2)).empty()) {
                    ++i;
                    break;
                }
                b.text.push_back(strip_cols(l, ind));
                b.nos.push_back(lines[i].no);
                ++i;
            }
            out.push_back(std::move(b));
            continue;
        }
        if (const int level = atx_level(s)) {
            Block b;
            b.kind = Block::kHeading;
            b.line = no;
            b.level = level;
            std::string_view h = trim(std::string_view(s).substr(static_cast<size_t>(level)));
            size_t end = h.size();
            while (end > 0 && h[end - 1] == '#') --end;
            if (end == 0 || h[end - 1] == ' ') h = trim(h.substr(0, end));
            b.text.emplace_back(h);
            out.push_back(std::move(b));
            ++i;
            continue;
        }
        if (is_rule(s)) {
            Block b;
            b.kind = Block::kRule;
            b.line = no;
            out.push_back(std::move(b));
            ++i;
            continue;
        }
        if (s.starts_with('>')) {
            Block b;
            b.kind = Block::kQuote;
            b.line = no;
            std::vector<Line> sub;
            std::vector<std::string> keep;  // (the lines' text, kept alive for the views into it)
            keep.reserve(n - i);
            bool para = false;
            while (i < n) {
                const std::string_view l = lines[i].text;
                const int li = indent_of(l);
                const std::string ls = li <= 3 ? strip_cols(l, li) : std::string();
                if (li <= 3 && ls.starts_with('>')) {
                    std::string rest = ls.substr(1);
                    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                    para = !blank(rest);
                    keep.push_back(std::move(rest));
                } else if (para && !blank(l) && !starts_block(l)) {  // (a paragraph's lazy line)
                    keep.emplace_back(l);
                } else {
                    break;
                }
                sub.push_back({{}, lines[i].no});
                ++i;
            }
            // An alert: its tag says what kind, and is not text of it.
            if (!keep.empty() && (b.level = alert_kind(keep[0])) != 0) keep[0].clear();
            for (size_t k = 0; k < sub.size(); ++k) sub[k].text = keep[k];
            b.kids = parse_blocks(sub, refs);
            out.push_back(std::move(b));
            continue;
        }
        Marker m;
        if (list_marker(t, &m)) {
            Block list;
            list.kind = Block::kList;
            list.line = no;
            list.ordered = m.ordered;
            list.start = m.number;
            const char kind = m.kind;
            bool gap = false;
            while (i < n) {
                Marker mi;
                if (!list_marker(lines[i].text, &mi) || mi.kind != kind || mi.ordered != list.ordered) break;
                if (gap) list.loose = true;
                Block item;
                item.kind = Block::kItem;
                item.line = lines[i].no;
                std::vector<std::string> keep;
                std::vector<u32> nos;
                const std::string first = from_col(lines[i].text, mi.content);
                keep.push_back(first);
                nos.push_back(lines[i].no);
                ++i;
                bool last_blank = blank(first), inner_gap = false;
                while (i < n) {
                    const std::string_view l = lines[i].text;
                    if (blank(l)) {
                        keep.emplace_back();
                        nos.push_back(lines[i].no);
                        last_blank = true;
                        ++i;
                        continue;
                    }
                    Marker next;
                    if (indent_of(l) < mi.content && list_marker(l, &next)) break;  // (the next item, or another list)
                    if (indent_of(l) >= mi.content) {
                        if (last_blank && keep.size() > 1) inner_gap = true;
                        keep.push_back(strip_cols(l, mi.content));
                    } else if (!last_blank && !starts_block(l)) {
                        keep.emplace_back(trim(l));  // (lazy continuation)
                    } else {
                        break;
                    }
                    nos.push_back(lines[i].no);
                    last_blank = false;
                    ++i;
                }
                gap = false;
                while (!keep.empty() && blank(keep.back())) keep.pop_back(), nos.pop_back(), gap = true;
                if (inner_gap) list.loose = true;
                // A task: [ ] or [x] in front.
                if (!keep.empty() && keep[0].size() >= 3 && keep[0][0] == '[' && keep[0][2] == ']' && (keep[0][1] == ' ' || keep[0][1] == 'x' || keep[0][1] == 'X') &&
                    (keep[0].size() == 3 || keep[0][3] == ' ')) {
                    item.task = keep[0][1] == ' ' ? 0 : 1;
                    keep[0] = keep[0].size() > 4 ? keep[0].substr(4) : std::string();
                }
                std::vector<Line> sub;
                for (size_t k = 0; k < keep.size(); ++k) sub.push_back({keep[k], nos[k]});
                item.kids = parse_blocks(sub, refs);
                list.kids.push_back(std::move(item));
                if (i < n && blank(lines[i].text)) gap = true;
            }
            out.push_back(std::move(list));
            continue;
        }
        // Display math: $$ (or \[) to the line that ends with $$ (or \]),
        // at most to a blank line; with none, it is a paragraph.
        if (const std::string_view st = trim(s); i >= no_math_to && (st.starts_with("$$") || st.starts_with("\\["))) {
            const std::string_view close = st[0] == '$' ? "$$" : "\\]";
            Block b;
            b.kind = Block::kMath;
            b.line = no;
            size_t j = i + 1;
            bool closed = st.size() >= 4 && st.ends_with(close);
            b.text.emplace_back(st.substr(2, closed ? st.size() - 4 : std::string_view::npos));
            b.nos.push_back(no);
            for (; !closed && j < n && !blank(lines[j].text); ++j) {
                const std::string_view l = trim(lines[j].text);
                closed = l.ends_with(close);
                b.text.emplace_back(l.substr(0, closed ? l.size() - 2 : std::string_view::npos));
                b.nos.push_back(lines[j].no);
            }
            if (closed) {
                out.push_back(std::move(b));
                i = j;
                continue;
            }
            no_math_to = j;
        }
        if (i + 1 < n && s.find('|') != std::string::npos) {
            std::vector<int> align;
            if (table_delims(lines[i + 1].text, &align)) {
                std::vector<std::string> head = table_cells(s);
                if (head.size() == align.size()) {
                    Block b;
                    b.kind = Block::kTable;
                    b.line = no;
                    b.align = align;
                    b.table.push_back(std::move(head));
                    b.nos.push_back(no);
                    i += 2;
                    while (i < n && !blank(lines[i].text) && (lines[i].text.find('|') != std::string_view::npos) && !starts_block(lines[i].text)) {
                        std::vector<std::string> row = table_cells(lines[i].text);
                        row.resize(align.size());
                        b.table.push_back(std::move(row));
                        b.nos.push_back(lines[i].no);
                        ++i;
                    }
                    out.push_back(std::move(b));
                    continue;
                }
            }
        }
        if (html_start(s)) {
            Block b;
            b.kind = Block::kHtml;
            b.line = no;
            // A comment, and <script>, <style> and <pre>, run to their end,
            // blank lines and all; anything else to a blank line.
            const std::string head = lower(s.substr(0, 8));
            std::string end;
            if (head.starts_with("<!--")) end = "-->";
            else if (head.starts_with("<script")) end = "</script>";
            else if (head.starts_with("<style")) end = "</style>";
            else if (head.starts_with("<pre")) end = "</pre>";
            while (i < n && (!end.empty() || !blank(lines[i].text))) {
                b.text.emplace_back(trim(lines[i].text));
                b.nos.push_back(lines[i].no);
                const bool closed = !end.empty() && lower(lines[i].text).find(end) != std::string::npos;
                ++i;
                if (closed) break;
            }
            out.push_back(std::move(b));
            continue;
        }
        // A footnote's definition: its text, with the lines indented under it
        // (and a paragraph's lines that run on), kept for the end.
        std::string label;
        size_t rest = 0;
        if (note_start(s, &label, &rest)) {
            Block b;
            b.kind = Block::kNote;
            b.line = no;
            b.info = label;
            std::vector<std::string> keep{std::string(trim(std::string_view(s).substr(rest)))};
            std::vector<u32> nos{no};
            ++i;
            bool last_blank = false;
            while (i < n) {
                const std::string_view l = lines[i].text;
                if (blank(l)) {
                    keep.emplace_back();
                    last_blank = true;
                } else if (indent_of(l) >= 2) {
                    keep.push_back(strip_cols(l, 4));
                } else if (!last_blank && !starts_block(l) && !note_start(trim(l))) {
                    keep.emplace_back(l);  // (a paragraph's lazy line)
                } else {
                    break;
                }
                if (!blank(l)) last_blank = false;
                nos.push_back(lines[i].no);
                ++i;
            }
            while (!keep.empty() && blank(keep.back())) keep.pop_back(), nos.pop_back();
            std::vector<Line> sub;
            for (size_t k = 0; k < keep.size(); ++k) sub.push_back({keep[k], nos[k]});
            b.kids = parse_blocks(sub, refs);
            refs.longest_note = std::max(refs.longest_note, label.size());
            refs.notes.emplace(std::move(label), 0);
            out.push_back(std::move(b));
            continue;
        }
        if (s.starts_with('[')) {  // a link's definition: [label]: where
            const size_t close = s.find("]:");
            if (close != std::string::npos && close > 1) {
                std::string_view dest = trim(std::string_view(s).substr(close + 2));
                if (const size_t sp = dest.find_first_of(" \t"); sp != std::string_view::npos) dest = dest.substr(0, sp);
                if (dest.starts_with('<') && dest.ends_with('>')) dest = dest.substr(1, dest.size() - 2);
                if (!dest.empty()) {
                    refs.set(lower(s.substr(1, close - 1)), std::string(dest));
                    ++i;
                    continue;
                }
            }
        }
        // A term and its definitions (": what it means" lines under it, with
        // the lines indented under each, or a paragraph's lines that run on).
        if (i + 1 < n && is_definition(lines[i + 1].text) && !is_definition(t)) {
            Block term;
            term.kind = Block::kTerm;
            term.line = no;
            term.text.emplace_back(trim(t));
            ++i;
            while (i < n && is_definition(lines[i].text)) {
                Block def;
                def.kind = Block::kItem;
                def.line = lines[i].no;
                const std::string_view first = trim(lines[i].text);
                std::vector<std::string> keep{std::string(trim(first.substr(1)))};
                std::vector<u32> nos{lines[i].no};
                ++i;
                bool last_blank = false;
                while (i < n) {
                    const std::string_view l = lines[i].text;
                    if (blank(l)) {  // (a blank line, and more of it indented after)
                        if (i + 1 < n && !blank(lines[i + 1].text) && indent_of(lines[i + 1].text) >= 2) {
                            keep.emplace_back();
                            nos.push_back(lines[i].no);
                            last_blank = true;
                            ++i;
                            continue;
                        }
                        break;
                    }
                    if (is_definition(l) || (i + 1 < n && is_definition(lines[i + 1].text))) break;  // (the next definition, or the next term)
                    if (indent_of(l) >= 2) keep.push_back(strip_cols(l, std::min(indent_of(l), 4)));
                    else if (!last_blank && !starts_block(l)) keep.emplace_back(trim(l));
                    else break;
                    nos.push_back(lines[i].no);
                    last_blank = false;
                    ++i;
                }
                std::vector<Line> sub;
                for (size_t k = 0; k < keep.size(); ++k) sub.push_back({keep[k], nos[k]});
                def.kids = parse_blocks(sub, refs);
                term.kids.push_back(std::move(def));
            }
            out.push_back(std::move(term));
            continue;
        }
        // A paragraph (or a heading underlined with === or ---).
        Block b;
        b.kind = Block::kPara;
        b.line = no;
        while (i < n) {
            const std::string_view l = lines[i].text;
            if (blank(l)) break;
            if (!b.text.empty()) {
                const std::string_view tl = trim(l);
                if (indent_of(l) <= 3 && !tl.empty() && (tl.find_first_not_of('=') == std::string_view::npos || tl.find_first_not_of('-') == std::string_view::npos)) {
                    b.kind = Block::kHeading;
                    b.level = tl[0] == '=' ? 1 : 2;
                    ++i;
                    break;
                }
                if (starts_block(l) || (indent_of(l) <= 3 && note_start(trim(l))) || (i + 1 < n && is_definition(lines[i + 1].text))) break;  // (a term's line ends it too)
                std::vector<int> align;  // (a table's head: the table starts here)
                if (l.find('|') != std::string_view::npos && i + 1 < n && table_delims(lines[i + 1].text, &align) && table_cells(l).size() == align.size()) break;
            }
            b.text.emplace_back(l);
            b.nos.push_back(lines[i].no);
            ++i;
        }
        out.push_back(std::move(b));
    }
}

std::vector<Block> parse_blocks(const std::vector<Line>& lines, Refs& refs) {
    // Nested deeper than anyone writes (a line of a thousand >): the rest is
    // a paragraph, so that the stack and the time stay small.
    static thread_local int depth = 0;
    std::vector<Block> out;
    if (depth >= kMostDepth) {
        Block b;
        b.line = lines.empty() ? 0 : lines[0].no;
        for (const Line& l : lines)
            if (!blank(l.text)) b.text.emplace_back(trim(l.text)), b.nos.push_back(l.no);
        if (!b.text.empty()) out.push_back(std::move(b));
        return out;
    }
    ++depth;
    parse_into(lines, refs, out);
    --depth;
    return out;
}

// A paragraph's lines as one text: a line that ends in two spaces or a
// backslash breaks there; the others run on.
std::string joined(const std::vector<std::string>& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        std::string_view l = text[i];
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        bool hard = false;
        if (l.ends_with("  ")) hard = true;
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.remove_suffix(1);
        if (l.ends_with('\\') && !l.ends_with("\\\\")) {
            hard = true;
            l.remove_suffix(1);
        }
        out += l;
        if (i + 1 < text.size()) out += hard ? '\n' : ' ';
    }
    return out;
}

// ---------------------------------------------------------------- layout

// The spans as lines at most `width` wide: words kept whole where they fit.
std::vector<std::vector<MdRun>> wrap(const std::vector<Span>& spans, u32 width) {
    std::vector<std::vector<MdRun>> lines(1);
    u32 at = 0;
    auto add = [&](std::string_view text, const Span& sp) {
        std::vector<MdRun>& l = lines.back();
        if (!l.empty() && l.back().ink == sp.ink && l.back().style == sp.style && l.back().tone == sp.tone && l.back().link == sp.link) l.back().text.append(text);
        else l.push_back({std::string(text), sp.ink, sp.style, sp.tone, sp.link});
        at += cols_of(text);
    };
    auto newline = [&] {
        while (!lines.back().empty() && lines.back().back().tone == kMdNoTone && lines.back().back().text.ends_with(' ')) {
            std::string& t = lines.back().back().text;
            while (!t.empty() && t.back() == ' ') t.pop_back(), --at;
            if (t.empty()) lines.back().pop_back();
        }
        lines.emplace_back();
        at = 0;
    };
    // A new line for a run's first word, `w` columns of it: when the line
    // does not end in a space, that word goes on from the run before (a
    // full stop after a link, a footnote's mark after its word), and what
    // there is of it comes to the new line with it, if all of it fits on one.
    auto carry = [&](u32 w) {
        std::vector<MdRun>& l = lines.back();
        std::vector<MdRun> tail;  // (the last first)
        u32 tail_cols = 0;
        bool space = false;
        while (!l.empty() && !space) {
            std::string& text = l.back().text;
            const size_t last = text.rfind(' ');
            space = last != std::string::npos;
            if (space && last + 1 == text.size()) break;
            MdRun part = l.back();
            if (space) part.text.erase(0, last + 1), text.resize(last + 1);
            else l.pop_back();
            tail_cols += cols_of(part.text);
            tail.push_back(std::move(part));
        }
        if (!space || tail_cols + w > width) {  // (a word as long as the line, or longer with this: cut here)
            for (size_t k = tail.size(); k-- > 0;) {
                if (!l.empty() && l.back().ink == tail[k].ink && l.back().style == tail[k].style && l.back().tone == tail[k].tone && l.back().link == tail[k].link) l.back().text += tail[k].text;
                else l.push_back(std::move(tail[k]));
            }
            newline();
            return;
        }
        newline();
        lines.back().assign(std::make_move_iterator(tail.rbegin()), std::make_move_iterator(tail.rend()));
        at = tail_cols;
    };
    for (const Span& sp : spans) {
        if (sp.brk) {
            newline();
            continue;
        }
        std::string_view t = sp.text;
        while (!t.empty()) {
            // The next word (with what follows it up to the next one), or the spaces before it.
            if (t[0] == ' ') {
                size_t k = 0;
                while (k < t.size() && t[k] == ' ') ++k;
                if (at > 0 && at + k <= width) add(t.substr(0, k), sp);
                else if (at > 0) newline();
                t.remove_prefix(k);
                continue;
            }
            size_t k = t.find(' ');
            if (k == std::string_view::npos) k = t.size();
            const std::string_view word = t.substr(0, k);
            const u32 w = cols_of(word);
            if (at + w > width && at > 0) {
                if (t.data() == sp.text.data()) carry(w);  // (it may go on from the run before)
                else newline();
            }
            if (w > width) {  // longer than a line: cut
                std::string_view rest = word;
                while (!rest.empty()) {
                    size_t cut = fit(rest, width - at);
                    if (cut == 0) cut = first_char(rest);  // (a wide character in one column: over the edge, whole)
                    add(rest.substr(0, cut), sp);
                    rest.remove_prefix(cut);
                    if (!rest.empty()) newline();
                }
            } else {
                add(word, sp);
            }
            t.remove_prefix(k);
        }
    }
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

struct Level {
    std::vector<MdRun> first, rest;
    bool used = false;
    bool list = false;  // a list item's (how deep lists are, for their bullets)
    bool details = false;  // what <details> holds (to its </details>, maybe blocks later)
};

// Whether these spans are pictures and nothing else (but space).
bool pictures_only(const std::vector<Span>& spans) {
    bool any = false;
    for (const Span& sp : spans) {
        if (!sp.image.empty()) any = true;
        else if (!sp.brk && sp.text.find_first_not_of(" \t") != std::string::npos) return false;
    }
    return any;
}

struct Layout {
    std::vector<MdRow>& rows;
    u32 width;
    const Refs& refs;
    std::vector<Level> levels;
    const MdPictureFn* picture = nullptr;
    const MdCodeFn* colour = nullptr;
    const MdDiagramFn* diagram = nullptr;
    bool named = true;  // a code block's language named over it
    size_t floor = 0;   // the levels the blocks being drawn are inside (a </details> closes none of them)
    static constexpr long kMostDetails = 8;  // <details> indented inside each other, at most
    int details_over = 0;                    // and those open inside the deepest of those

    u32 indent() const {
        u32 n = 0;
        for (const Level& l : levels) n += cols_of_runs(l.rest);
        return n;
    }
    static u32 cols_of_runs(const std::vector<MdRun>& runs) {
        u32 n = 0;
        for (const MdRun& r : runs) n += cols_of(r.text);
        return n;
    }
    u32 room() const { return width > indent() + 1 ? width - indent() : 1; }
    // A row: the levels' marks in front (each level's first mark on its first row), then these.
    MdRow& row(std::vector<MdRun> runs, u32 line) {
        MdRow r;
        r.line = line;
        for (Level& l : levels) {
            const std::vector<MdRun>& mark = l.used ? l.rest : l.first;
            r.runs.insert(r.runs.end(), mark.begin(), mark.end());
            l.used = true;
        }
        for (MdRun& run : runs)
            if (!run.text.empty()) r.runs.push_back(std::move(run));
        rows.push_back(std::move(r));
        return rows.back();
    }
    void gap(u32 line) {  // a blank row (a quote's bar goes on through it)
        if (rows.empty()) return;
        MdRow r;
        r.line = line;
        for (const Level& l : levels) {
            std::vector<MdRun> mark = l.rest;
            for (MdRun& m : mark)
                if (m.text.find_first_not_of(' ') == std::string::npos) m.text.clear();
            r.runs.insert(r.runs.end(), mark.begin(), mark.end());
        }
        rows.push_back(std::move(r));
    }

    void blocks(const std::vector<Block>& bs, bool tight, u8 ink) {
        const size_t held = levels.size();  // (<details> opened among these and not closed: closed at their end)
        const size_t was_floor = floor;
        floor = held;
        const Block* before = nullptr;  // (the one drawn before this one: a footnote's text is drawn at the end)
        for (const Block& b : bs) {
            if (b.kind == Block::kNote) continue;
            // (A heading of the top two levels is underlined: what follows
            // starts right under its line, so that it belongs to the heading,
            // with the blank row above the heading setting it off.)
            // A heading of those levels starts a part of the text: two blank
            // rows above it. (A heading right under another's line: one.)
            const bool under_rule = before && before->kind == Block::kHeading && before->level <= 2;
            const bool heading = b.kind == Block::kHeading;
            const size_t had = rows.size();
            if (before && !tight && (!under_rule || heading)) gap(b.line);
            if (before && !tight && !under_rule && heading && b.level <= 2) gap(b.line);
            const size_t gapped = rows.size();
            block(b, tight, ink);
            if (rows.size() == gapped && b.kind == Block::kHtml) {  // (HTML that draws nothing, a </details>: no room for it either)
                rows.resize(had);
                continue;
            }
            before = &b;
        }
        while (levels.size() > held && levels.back().details) levels.pop_back();
        floor = was_floor;
    }

    // The footnotes, under a short rule at the end: those referred to, in
    // the order of their numbers (one that only another's text refers to is
    // numbered as that is drawn). One nothing refers to is not drawn, as on
    // GitHub. Each has its number in front, its first row's id ("fn-1") is
    // what its marks go to, and the mark after its text goes back to the
    // first of them.
    void footnotes(const std::vector<const Block*>& defs) {
        std::map<std::string, const Block*> by_label;  // (the first of a label is the one)
        for (const Block* b : defs) by_label.emplace(b->info, b);
        if (by_label.empty() || refs.noted.empty()) return;
        const size_t from = rows.size();
        u32 line = rows.empty() ? 0 : rows.back().line;
        gap(line);
        std::string rule;
        for (u32 k = 0; k < std::min<u32>(room(), 24); ++k) rule += "\xE2\x94\x80";
        row({{rule, kMdMuted}}, line);
        const size_t numw = std::to_string(by_label.size()).size() + 1;
        size_t done = 0;
        while (done < refs.noted.size()) {  // (a note's text can refer to another: noted grows as they are drawn)
            const int n = static_cast<int>(++done);
            const Block& def = *by_label[refs.noted[done - 1]];
            const std::string num = std::to_string(n) + ".";
            levels.push_back({{{std::string(numw > num.size() ? numw - num.size() : 0, ' ') + num + " ", kMdAccent}}, {{std::string(numw + 1, ' '), kMdText}}, false});
            const size_t first = rows.size();
            blocks(def.kids, false, kMdText);
            if (rows.size() == first) row({}, def.line);  // (one that says nothing)
            rows[first].id = "fn-" + std::to_string(n);
            MdRun back("\xE2\x86\xA9", kMdLink, 0, kMdNoTone, "#fnref-" + std::to_string(n));
            MdRow& last = rows.back();
            if (!last.fill && last.pics.empty() && !last.runs.empty() && cols_of_runs(last.runs) + 2 <= width) {
                if (!last.runs.back().text.ends_with(' ')) last.runs.push_back({" "});
                last.runs.push_back(std::move(back));
            } else {
                row({std::move(back)}, last.line);
            }
            levels.pop_back();
        }
        // (Their rows' lines never go back: the preview follows the cursor by them.)
        for (size_t k = from; k < rows.size(); ++k) rows[k].line = line = std::max(line, rows[k].line);
    }

    // Pictures, side by side as far as they fit (in the middle, if
    // `centre`); one that cannot be drawn is its text, on a row of its own.
    void pictures(const std::vector<Span>& spans, u32 line, bool centre) {
        std::vector<MdPic> strip;
        u32 used = 0;
        auto flush = [&] {
            if (strip.empty()) return;
            u32 tall = 0;
            for (const MdPic& p : strip) tall = std::max(tall, p.rows);
            const u32 shift = indent() + (centre && room() > used ? (room() - used) / 2 : 0);
            for (u32 r = 0; r < tall; ++r) {
                MdRow& at = row({}, line);
                for (const MdPic& p : strip)
                    if (r < p.rows) {
                        at.pics.push_back(p);
                        at.pics.back().col += shift;
                        at.pics.back().part = r;
                    }
            }
            strip.clear();
            used = 0;
        };
        for (const Span& sp : spans) {
            if (sp.image.empty()) continue;
            MdPic p;
            if (picture && *picture && (*picture)(sp.image, room(), sp.image_w, sp.image_h, &p) && p.cols && p.rows) {
                p.src = sp.image;
                p.link = sp.link;
                if (!strip.empty() && used + 1 + p.cols > room()) flush();
                p.col = strip.empty() ? 0 : used + 1;
                used = p.col + p.cols;
                strip.push_back(std::move(p));
            } else {
                flush();
                std::vector<MdRun> runs;
                const u32 w = cols_of(sp.text);
                if (centre && room() > w) runs.push_back({std::string((room() - w) / 2, ' ')});
                runs.push_back({sp.text, sp.ink, sp.style, sp.tone, sp.link});
                row(std::move(runs), line);
            }
        }
        flush();
    }

    void paragraph(const std::vector<std::string>& text, const std::vector<u32>& nos, u32 line, u8 ink, u8 style) {
        std::vector<Span> spans;
        Span as;
        as.ink = ink;
        as.style = style;
        parse_inline(joined(text), as, refs, spans);
        if (picture && *picture && pictures_only(spans)) {
            pictures(spans, line, false);
            return;
        }
        const auto lines = wrap(spans, room());
        // (each row's source line: the paragraph's lines, as far as rows go)
        for (size_t k = 0; k < lines.size(); ++k) row(lines[k], nos.empty() ? line : nos[std::min(k * nos.size() / std::max<size_t>(1, lines.size()), nos.size() - 1)]);
    }

    void code(const Block& b) {
        const u32 room_cols = room();
        // A Mermaid diagram, drawn (a picture, in the middle); or the code,
        // if it cannot be (or is not drawn yet).
        if (lower(b.info.substr(0, b.info.find(' '))) == "mermaid") {
            std::string src;
            for (const std::string& l : b.text) src += l + "\n";
            auto line_of = [&](size_t k, size_t n) { return b.nos.empty() ? b.line : b.nos[std::min(k * b.nos.size() / std::max<size_t>(1, n), b.nos.size() - 1)]; };
            std::string note;
            MdPic pic;
            // (a line about it, wrapped to the room as a paragraph is)
            auto say = [&](const std::string& text, u8 ink) {
                Span sp;
                sp.text = text, sp.ink = ink, sp.style = kMdItalic;
                for (std::vector<MdRun>& l : wrap({sp}, room_cols)) row(std::move(l), b.line);
            };
            const u8 got = diagram && *diagram ? (*diagram)(src, room_cols, &pic, &note) : static_cast<u8>(kMdDiagramLater);
            if (got == kMdDiagramPicture && pic.cols && pic.rows) {
                pic.col = indent() + (room_cols > pic.cols ? (room_cols - pic.cols) / 2 : 0);
                for (u32 r = 0; r < pic.rows; ++r) {
                    MdRow& at = row({}, line_of(r, pic.rows));
                    at.pics.push_back(pic);
                    at.pics.back().part = r;
                }
                return;
            }
            if (got != kMdDiagramLater && !note.empty()) say("mermaid: " + note, kMdError);
        }
        const u32 from = indent();
        auto fill = [&](MdRow& r) {
            r.fill = kMdCodeTone;
            r.fill_from = from;
            r.fill_to = width;
        };
        // The language, at the top on the right.
        const std::string lang = b.info.substr(0, b.info.find(' '));
        std::vector<MdRun> top;
        if (named && !lang.empty() && room_cols > cols_of(lang) + 2) top.push_back({std::string(room_cols - cols_of(lang) - 1, ' ') + lang, kMdMuted, 0, kMdCodeTone});
        fill(row(std::move(top), b.line));
        // Its colours, when it is in a language there is a grammar for: an
        // ink for each byte of its lines (each ended by a newline).
        std::vector<u8> inks;
        std::vector<size_t> starts;
        if (colour && *colour && !lang.empty() && !b.text.empty()) {
            std::string src;
            for (const std::string& l : b.text) {
                starts.push_back(src.size());
                src += l;
                src += '\n';
            }
            if (!(*colour)(lang, src, &inks) || inks.size() != src.size()) inks.clear();
        }
        // A diff (or patch): its lines in the Git views' inks, added and
        // removed; a hunk's @@ header in the accent, a file's header quiet.
        if (const std::string l = lower(lang); (l == "diff" || l == "patch") && !b.text.empty()) {
            inks.clear();
            starts.clear();
            size_t at = 0;
            for (const std::string& t : b.text) {
                static constexpr std::string_view kHeads[] = {"+++", "---", "diff ", "index ", "new file", "deleted file", "similarity", "rename ", "old mode", "new mode", "Binary files", "\\ No newline"};
                const bool head = std::ranges::any_of(kHeads, [&](std::string_view h) { return t.starts_with(h); });
                const u8 ink = head ? kMdMuted : t.starts_with("@@") ? kMdAccent : t.starts_with('+') ? kMdAdded : t.starts_with('-') ? kMdError : kMdCodePlain;
                starts.push_back(at);
                inks.insert(inks.end(), t.size() + 1, ink);
                at += t.size() + 1;
            }
        }
        for (size_t k = 0; k < b.text.size(); ++k) {
            std::vector<u8> li;  // (the ink of each byte of l)
            std::string l = expand_tabs(b.text[k], inks.empty() ? nullptr : inks.data() + starts[k], &li);
            const u32 line = k < b.nos.size() ? b.nos[k] : b.line;
            size_t at = 0;
            do {  // (long lines wrap)
                const std::string_view rest = std::string_view(l).substr(at);
                size_t cut = room_cols > 2 ? fit(rest, room_cols - 2) : rest.size();
                if (cut == 0) cut = first_char(rest);  // (a wide character in one column: over the edge, whole)
                cut = std::min(cut, rest.size());
                if (cut < rest.size() && rest[cut] != ' ') {  // (after the last space that fits, as an editor wraps, if that is not far back)
                    const size_t space = rest.find_last_of(' ', cut - 1);
                    if (space != std::string_view::npos && space + 1 > cut / 2 && rest.find_first_not_of(' ') < space) cut = space + 1;
                }
                std::vector<MdRun> runs;
                if (li.empty()) {
                    runs.push_back({" " + std::string(rest.substr(0, cut)), kMdCode, 0, kMdCodeTone});
                } else {  // a run for each stretch of one ink
                    runs.push_back({" ", kMdCodePlain, 0, kMdCodeTone});
                    for (size_t a = at; a < at + cut;) {
                        size_t e = a + 1;
                        while (e < at + cut && li[e] == li[a]) ++e;
                        runs.push_back({l.substr(a, e - a), li[a], 0, kMdCodeTone});
                        a = e;
                    }
                }
                fill(row(std::move(runs), line));
                at += cut;
            } while (at < l.size());
        }
        fill(row({}, b.nos.empty() ? b.line : b.nos.back()));
    }

    void table(const Block& b) {
        const size_t ncols = b.align.size();
        std::vector<std::vector<std::vector<Span>>> cells(b.table.size());
        std::vector<u32> natural(ncols, 1), word(ncols, 1);  // (each column's widest cell, and its longest word)
        for (size_t r = 0; r < b.table.size(); ++r) {
            cells[r].resize(ncols);
            for (size_t c = 0; c < ncols; ++c) {
                Span as;
                if (r == 0) as.style = kMdBold;
                parse_inline(c < b.table[r].size() ? b.table[r][c] : std::string(), as, refs, cells[r][c]);
                u32 w = 0, in_word = 0;
                for (const Span& sp : cells[r][c]) {
                    w += cols_of(sp.text);
                    for (std::string_view t = sp.text; !t.empty();) {  // (a word may go on from one run to the next)
                        const size_t space = t.find(' ');
                        in_word += cols_of(t.substr(0, space));
                        word[c] = std::max(word[c], in_word);
                        if (space == std::string_view::npos) break;
                        in_word = 0;
                        t.remove_prefix(space + 1);
                    }
                }
                natural[c] = std::max(natural[c], w);
            }
        }
        // Each column as wide as its widest cell. When they do not all fit,
        // each is as wide as its longest word, and what room is left is
        // shared in proportion to what more each would take (so that a
        // column of long text gets the most of it); when even the words do
        // not fit, the widest is made narrower until they do.
        const u32 frame = static_cast<u32>(3 * ncols + 1);
        const u32 room_cols = room() > frame ? room() - frame : static_cast<u32>(ncols);
        std::vector<u32> w = natural;
        auto total = [&] {
            u32 t = 0;
            for (const u32 x : w) t += x;
            return t;
        };
        if (total() > room_cols) {
            u32 more = 0;  // (what all would take past their words)
            for (size_t c = 0; c < ncols; ++c) {
                w[c] = std::min(word[c], natural[c]);
                more += natural[c] - w[c];
            }
            if (total() < room_cols && more > 0) {
                const u32 spare = room_cols - total();
                u32 given = 0;
                for (size_t c = 0; c < ncols; ++c) {
                    const u32 share = static_cast<u32>(static_cast<u64>(spare) * (natural[c] - w[c]) / more);
                    w[c] += share;
                    given += share;
                }
                for (size_t c = 0; given < spare && c < ncols; ++c)  // (what rounding left, to the first that can take it)
                    if (w[c] < natural[c]) ++w[c], ++given;
            }
            while (total() > room_cols) {
                const auto widest = std::max_element(w.begin(), w.end());
                if (*widest <= 3) break;
                --*widest;
            }
        }
        // Its lines quiet (the theme's muted colour: the text is what matters).
        auto border = [&](const char* l, const char* m, const char* r, u32 line) {
            std::string s = l;
            for (size_t c = 0; c < ncols; ++c) {
                for (u32 k = 0; k < w[c] + 2; ++k) s += "\xE2\x94\x80";
                s += c + 1 < ncols ? m : r;
            }
            row({{s, kMdMuted}}, line);
        };
        std::vector<std::vector<std::vector<std::vector<MdRun>>>> wrapped(cells.size());  // (each row's cells, as lines)
        bool wraps = false;  // (a body cell over more than one line)
        for (size_t r = 0; r < cells.size(); ++r) {
            wrapped[r].resize(ncols);
            for (size_t c = 0; c < ncols; ++c) {
                wrapped[r][c] = wrap(cells[r][c], w[c]);
                wraps = wraps || (r > 0 && wrapped[r][c].size() > 1);
            }
        }
        const char* bar = "\xE2\x94\x82";
        border("\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90", b.line);
        for (size_t r = 0; r < cells.size(); ++r) {
            const u32 line = r < b.nos.size() ? b.nos[r] : b.line;
            size_t height = 1;
            for (size_t c = 0; c < ncols; ++c) height = std::max(height, wrapped[r][c].size());
            // The header's tint one band, from the first line to the last
            // (the lines between its cells in it too).
            const u8 tone = r == 0 ? kMdHeaderTone : kMdNoTone;
            for (size_t k = 0; k < height; ++k) {
                std::vector<MdRun> runs{{bar, kMdMuted}};
                for (size_t c = 0; c < ncols; ++c) {
                    std::vector<MdRun> cell = k < wrapped[r][c].size() ? wrapped[r][c][k] : std::vector<MdRun>{};
                    const u32 used = cols_of_runs(cell);
                    const u32 spare = w[c] > used ? w[c] - used : 0;
                    const u32 before = b.align[c] == 1 ? spare : b.align[c] == 0 ? spare / 2 : 0;
                    runs.push_back({std::string(1 + before, ' '), kMdText, 0, tone});
                    for (MdRun& run : cell) {
                        if (r == 0 && run.tone == kMdNoTone) run.tone = kMdHeaderTone;
                        runs.push_back(std::move(run));
                    }
                    runs.push_back({std::string(spare - before + 1, ' '), kMdText, 0, tone});
                    runs.push_back({bar, kMdMuted, 0, c + 1 < ncols ? tone : static_cast<u8>(kMdNoTone)});
                }
                row(std::move(runs), line);
            }
            // Under the header a line; between the other rows one too when a
            // cell wraps (its lines would run into the next row's).
            if (r == 0 || (wraps && r + 1 < cells.size())) border("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4", line);
        }
        border("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98", b.nos.empty() ? b.line : b.nos.back());
    }

    // Display math: its rows (TeX's \\ ends one) each on its own, in the
    // middle, slanted; a row wider than the room wraps at its spaces.
    void math(const Block& b, u8 ink) {
        std::string tex;
        for (const std::string& l : b.text) tex += l + " ";
        std::vector<std::string_view> parts;
        for (size_t at = 0;;) {
            const size_t end = tex.find("\\\\", at);
            parts.push_back(std::string_view(tex).substr(at, end == std::string::npos ? std::string::npos : end - at));
            if (end == std::string::npos) break;
            at = end + 2;
        }
        size_t k = 0;
        for (const std::string_view part : parts) {
            Span sp;
            sp.text = md_math_text(part, refs.glyphs);
            sp.ink = ink;
            sp.style = kMdItalic;
            const u32 line = b.nos.empty() ? b.line : b.nos[std::min(k++ * b.nos.size() / parts.size(), b.nos.size() - 1)];
            if (sp.text.empty()) continue;
            for (std::vector<MdRun>& r : wrap({sp}, room())) {
                const u32 w = cols_of_runs(r);
                if (room() > w) r.insert(r.begin(), MdRun(std::string((room() - w) / 2, ' ')));
                row(std::move(r), line);
            }
        }
    }

    // HTML: its text, without the tags (and comments), its inline tags as
    // the text says (parse_inline); its pictures drawn. A line in a <p>,
    // <div> or heading that says align="center" (as a README's top often
    // does), or in a <center>, is in the middle; one in an <h1> to <h6> is
    // that heading; <details> is open, its <summary> bold after a marker and
    // what it holds under that, indented (to its </details>, which may be
    // blocks later: Markdown in it, after a blank line); <hr> is a rule.
    void html(const Block& b, u8 ink) {
        std::string all;
        for (const std::string& l : b.text) all += l + "\n";
        // Comments, scripts and styles go, the lines they were on staying
        // (as blank ones), so that each line keeps its place in the file.
        auto cut = [&](std::string_view open, std::string_view close) {
            for (size_t c; (c = lower(all).find(open)) != std::string::npos;) {
                size_t e = lower(all).find(close, c + open.size());
                e = e == std::string::npos ? all.size() : e + close.size();
                const auto lines = std::count(all.begin() + static_cast<std::ptrdiff_t>(c), all.begin() + static_cast<std::ptrdiff_t>(e), '\n');
                all.replace(c, e - c, static_cast<size_t>(lines), '\n');
            }
        };
        cut("<!--", "-->");
        cut("<script", "</script>");
        cut("<style", "</style>");
        struct Open {
            std::string name;
            bool centre;
        };
        std::vector<Open> open;  // (the tags that change how a line is drawn, as they open and close)
        std::vector<Span> pics;  // pictures on lines of their own, gathered (a row of them across lines)
        bool pics_centre = false;
        int pending = 0;  // (<details> opened, its indent waiting for its summary)
        auto indent_details = [&] {
            for (; pending > 0; --pending) {
                const auto deep = std::count_if(levels.begin(), levels.end(), [](const Level& l) { return l.details; });
                if (deep < kMostDetails) levels.push_back({{{"  ", kMdText}}, {{"  ", kMdText}}, false, false, true});
                else ++details_over;  // (deeper than anyone nests them: at the indent of the deepest)
            }
        };
        // (each line, and what follows a </summary> on it as a line of its own)
        std::vector<std::pair<std::string_view, u32>> parts;
        size_t k = 0;
        for (size_t at = 0; at < all.size(); ++k) {
            const size_t nl = all.find('\n', at);
            std::string_view l = std::string_view(all).substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? all.size() : nl + 1;
            const u32 line = k < b.nos.size() ? b.nos[k] : b.line;
            if (const size_t end = lower(l).find("</summary>"); end != std::string::npos && !blank(l.substr(end + 10))) {
                parts.emplace_back(l.substr(0, end + 10), line);
                l.remove_prefix(end + 10);
            }
            parts.emplace_back(l, line);
        }
        for (const auto& [l, line] : parts) {
            // Its tags: what is open over it, and what it opens and closes.
            bool centre = false, summary = false, rule = false;
            int heading = 0, details = 0;  // (details: <details> opened on it, less those closed)
            for (const Open& o : open) {
                centre = centre || o.centre;
                if (o.name.size() == 2 && o.name[0] == 'h') heading = o.name[1] - '0';
            }
            bool heading_ends = false;
            for (size_t lt = l.find('<'); lt != std::string_view::npos; lt = l.find('<', lt + 1)) {
                const size_t gt = l.find('>', lt);
                if (gt == std::string_view::npos) break;
                const std::string_view tag = l.substr(lt + 1, gt - lt - 1);
                const bool closing = tag.starts_with('/');
                size_t e = closing ? 1 : 0;
                while (e < tag.size() && e < 12 && std::isalnum(static_cast<unsigned char>(tag[e]))) ++e;
                const std::string name = lower(tag.substr(closing ? 1 : 0, e - (closing ? 1 : 0)));
                lt = gt;
                const bool head = name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
                if (name == "hr" && !closing) rule = true;
                if (name == "summary" && !closing) summary = true;
                if (name != "p" && name != "div" && name != "center" && name != "details" && !head) continue;
                if (!closing) {
                    const std::string align = lower(attribute(tag, "align"));
                    open.push_back({name, name == "center" || align == "center"});
                    centre = centre || open.back().centre;
                    if (head) heading = name[1] - '0';
                    if (name == "details") ++details;
                } else {
                    for (size_t o = open.size(); o-- > 0;)
                        if (open[o].name == name) {
                            open.erase(open.begin() + static_cast<std::ptrdiff_t>(o), open.end());
                            break;
                        }
                    if (head) heading_ends = true;
                    if (name == "details") --details;
                }
            }
            if (rule) {
                if (!pics.empty()) pictures(pics, line, pics_centre), pics.clear();
                std::string r;
                for (u32 c = 0; c < room(); ++c) r += "\xE2\x94\x80";
                row({{r, kMdMuted}}, line);
            }
            Span as;
            as.ink = heading ? static_cast<u8>(heading == 1 ? kMdHeading1 : heading == 2 ? kMdHeading2 : heading == 3 ? kMdHeading3 : kMdHeading) : ink;
            as.style = heading ? (heading <= 4 ? kMdBold : heading == 5 ? kMdBold | kMdItalic : kMdItalic) : summary ? kMdBold : 0;
            std::vector<Span> spans;
            parse_inline(l, as, refs, spans);
            if (pictures_only(spans)) {
                for (Span& sp : spans)
                    if (!sp.image.empty()) pics.push_back(std::move(sp));
                pics_centre = centre;
            } else {
                bool text = false;
                for (const Span& sp : spans) text = text || (!sp.brk && sp.text.find_first_not_of(" \t") != std::string::npos);
                if (text) {  // (not nothing but tags)
                    if (!pics.empty()) pictures(pics, line, pics_centre), pics.clear();
                    if (!summary) indent_details();
                    if (summary) {  // (always open: the marker says so)
                        Span mark;
                        mark.text = "\xE2\x96\xBE ";
                        mark.ink = kMdAccent;
                        while (!spans.empty() && !spans[0].brk && spans[0].image.empty() && spans[0].text.find_first_not_of(" \t") != 0) {  // (from its first word)
                            const size_t lead = spans[0].text.find_first_not_of(" \t");
                            if (lead == std::string::npos) spans.erase(spans.begin());
                            else spans[0].text.erase(0, lead);
                        }
                        spans.insert(spans.begin(), mark);
                    }
                    for (std::vector<MdRun>& r : wrap(spans, room())) {
                        if (centre) {
                            const u32 w = cols_of_runs(r);
                            if (room() > w) r.insert(r.begin(), MdRun(std::string((room() - w) / 2, ' ')));
                        }
                        row(std::move(r), line);
                    }
                    if (heading && heading <= 2 && heading_ends) {  // (a line under the top two, as under a Markdown heading)
                        std::string r;
                        for (u32 c = 0; c < room(); ++c) r += heading == 1 ? "\xE2\x94\x81" : "\xE2\x94\x80";
                        row({{r, heading == 1 ? kMdHeading1 : kMdMuted}}, line);
                    }
                }
            }
            // What <details> holds, indented under its summary; </details> ends that.
            pending += details;
            if (summary) indent_details();
            for (; pending < 0 && details_over > 0; ++pending) --details_over;
            for (; pending < 0 && levels.size() > floor && levels.back().details; ++pending) levels.pop_back();
            pending = std::max(pending, 0);
        }
        if (!pics.empty()) pictures(pics, b.nos.empty() ? b.line : b.nos.back(), pics_centre);
        indent_details();
    }

    void block(const Block& b, bool tight, u8 ink) {
        switch (b.kind) {
            case Block::kPara: paragraph(b.text, b.nos, b.line, ink, 0); break;
            case Block::kHeading: {
                // Each level its own: a colour for each of the top three (and
                // one for the rest), a heavy line under the first and a light
                // one under the second, and the last three told apart by
                // weight and slant (bold, bold and slanted, slanted).
                const u8 hink = b.level == 1 ? kMdHeading1 : b.level == 2 ? kMdHeading2 : b.level == 3 ? kMdHeading3 : kMdHeading;
                paragraph(b.text, {}, b.line, hink, b.level <= 4 ? kMdBold : b.level == 5 ? kMdBold | kMdItalic : kMdItalic);
                if (b.level <= 2) {  // a line under the top two levels
                    std::string rule;
                    for (u32 k = 0; k < room(); ++k) rule += b.level == 1 ? "\xE2\x94\x81" : "\xE2\x94\x80";
                    row({{rule, b.level == 1 ? kMdHeading1 : kMdMuted}}, b.line);
                }
                break;
            }
            case Block::kCode: code(b); break;
            case Block::kRule: {
                std::string rule;
                for (u32 k = 0; k < room(); ++k) rule += "\xE2\x94\x80";
                row({{rule, kMdMuted}}, b.line);
                break;
            }
            case Block::kHtml: html(b, ink); break;
            case Block::kQuote: {
                // An alert is a callout: a heavier bar and a title in its kind's
                // colour, and what it says under that, in the text's own.
                static const struct {
                    const char* title;
                    u8 ink;
                } kAlerts[] = {{"Note", kMdAnsiBlue}, {"Tip", kMdAnsiGreen}, {"Important", kMdAnsiMagenta}, {"Warning", kMdAnsiYellow}, {"Caution", kMdError}};
                const bool alert = b.level >= 1 && b.level <= 5;
                const u8 bar_ink = alert ? kAlerts[b.level - 1].ink : static_cast<u8>(kMdAccent);
                const char* bar = alert ? "\xE2\x96\x8C " : "\xE2\x96\x8E ";
                levels.push_back({{{bar, bar_ink}}, {{bar, bar_ink}}, false});
                if (alert) row({{kAlerts[b.level - 1].title, bar_ink, kMdBold}}, b.line);
                blocks(b.kids, false, alert ? kMdText : kMdQuote);
                levels.pop_back();
                break;
            }
            case Block::kList: {
                int depth = 0;
                for (const Level& l : levels) depth += l.list ? 1 : 0;
                static const char* const kBullets[] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};
                const int last = b.start + static_cast<int>(b.kids.size()) - 1;
                const u32 numw = b.ordered ? static_cast<u32>(std::to_string(std::max(last, b.start)).size()) + 1 : 1;
                for (size_t k = 0; k < b.kids.size(); ++k) {
                    const Block& item = b.kids[k];
                    if (k > 0 && b.loose) gap(item.line);
                    std::string mark;
                    u8 mark_ink = kMdAccent;
                    if (item.task >= 0) {  // (a square, crossed when done: ☐ and ☑ are small in most fonts, their tick hard to see)
                        mark = item.task ? "\xE2\x8A\xA0" : "\xE2\x96\xA1";
                        mark_ink = item.task ? kMdAccent : kMdMuted;
                    } else if (b.ordered) {
                        const std::string num = std::to_string(b.start + static_cast<int>(k)) + ".";
                        mark = std::string(numw > num.size() ? numw - num.size() : 0, ' ') + num;
                    } else {
                        mark = kBullets[depth % 3];
                    }
                    const u32 mw = cols_of(mark);
                    levels.push_back({{{mark + " ", mark_ink}}, {{std::string(mw + 1, ' '), kMdText}}, false, true});
                    if (item.kids.empty()) row({}, item.line);
                    blocks(item.kids, !b.loose, item.task == 1 ? static_cast<u8>(kMdMuted) : ink);
                    levels.pop_back();
                }
                break;
            }
            case Block::kItem: blocks(b.kids, tight, ink); break;
            case Block::kTable: table(b); break;
            case Block::kNote: break;  // (at the end: footnotes())
            case Block::kMath: math(b, ink); break;
            case Block::kTerm: {  // the term in bold, each definition indented under it (numbered, quietly, when there are more than one)
                paragraph(b.text, {}, b.line, ink, kMdBold);
                const size_t numw = std::max<size_t>(std::to_string(b.kids.size()).size() + 3, 4);
                for (size_t k = 0; k < b.kids.size(); ++k) {
                    const Block& def = b.kids[k];
                    const std::string num = b.kids.size() > 1 ? std::to_string(k + 1) + ". " : "";
                    levels.push_back({{{std::string(numw - num.size(), ' ') + num, num.empty() ? kMdText : kMdMuted}}, {{std::string(numw, ' '), kMdText}}, false});
                    if (def.kids.empty()) row({}, def.line);
                    blocks(def.kids, false, ink);
                    levels.pop_back();
                }
                break;
            }
        }
    }
};

// The footnotes' texts among these blocks and those inside them, in the file's order.
void gather_notes(const std::vector<Block>& bs, std::vector<const Block*>& out) {
    for (const Block& b : bs) {
        if (b.kind == Block::kNote) out.push_back(&b);
        gather_notes(b.kids, out);
    }
}

}  // namespace

std::vector<MdRow> md_render(std::string_view text, u32 width, const MdPictureFn& picture, const MdCodeFn& colour, const MdDiagramFn& diagram, u8 glyphs) {
    width = std::max<u32>(width, 12);
    std::vector<Line> lines;
    u32 no = 0;
    for (size_t at = 0; at <= text.size();) {
        const size_t nl = text.find('\n', at);
        std::string_view l = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (l.ends_with('\r')) l.remove_suffix(1);
        lines.push_back({l, no++});
        if (nl == std::string_view::npos) break;
        at = nl + 1;
    }
    // Front matter (--- at the very top, to the next ---) is not shown: when
    // what is between looks like it (keys and values, list items, comments),
    // not a rule at the top and another further down.
    if (!lines.empty() && trim(lines[0].text) == "---")
        for (size_t k = 1; k < lines.size(); ++k) {
            const std::string_view t = trim(lines[k].text);
            if (t == "---" || t == "...") {
                lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(k + 1));
                break;
            }
            const bool yaml = t.empty() || t.starts_with('#') || t.starts_with("- ") || t.find(':') != std::string_view::npos || indent_of(lines[k].text) > 0;
            if (!yaml) break;
        }
    Refs refs;
    refs.glyphs = glyphs;
    // (links can be defined after they are used: their definitions are gathered first)
    for (const Line& l : lines) {
        const std::string_view s = trim(l.text);
        const size_t close = s.find("]:");
        if (s.starts_with('[') && close != std::string_view::npos && close > 1 && s[1] != '^') {
            std::string_view dest = trim(s.substr(close + 2));
            if (const size_t sp = dest.find_first_of(" \t"); sp != std::string_view::npos) dest = dest.substr(0, sp);
            if (!dest.empty()) refs.set(lower(s.substr(1, close - 1)), std::string(dest));
        }
    }
    const std::vector<Block> blocks = parse_blocks(lines, refs);
    std::vector<MdRow> rows;
    Layout layout{rows, width, refs, {}, &picture, &colour, &diagram};
    layout.blocks(blocks, false, kMdText);
    std::vector<const Block*> notes;
    if (!refs.notes.empty()) gather_notes(blocks, notes);
    layout.footnotes(notes);
    return rows;
}

std::vector<MdRow> md_code(std::string_view lang, std::string_view code, u32 width, const MdCodeFn& colour) {
    Block b;
    b.kind = Block::kCode;
    b.info = std::string(lang);
    for (size_t at = 0; at < code.size();) {
        size_t nl = code.find('\n', at);
        if (nl == std::string_view::npos) nl = code.size();
        std::string_view l = code.substr(at, nl - at);
        if (l.ends_with('\r')) l.remove_suffix(1);
        b.nos.push_back(static_cast<u32>(b.text.size()));
        b.text.emplace_back(l);
        at = nl + 1;
    }
    std::vector<MdRow> rows;
    const Refs refs;
    const MdPictureFn none;
    Layout layout{rows, std::max<u32>(width, 12), refs, {}, &none, &colour};
    layout.named = false;
    layout.code(b);
    return rows;
}

std::vector<MdRow> md_table(const std::vector<std::vector<std::string>>& cells, const std::vector<int>& align, u32 width) {
    Block b;
    b.kind = Block::kTable;
    b.table = cells;
    b.align = align;
    size_t cols = 0;
    for (const std::vector<std::string>& r : cells) cols = std::max(cols, r.size());
    b.align.resize(std::max<size_t>(cols, 1), -1);
    for (u32 r = 0; r < cells.size(); ++r) b.nos.push_back(r);
    std::vector<MdRow> rows;
    const Refs refs;
    Layout layout{rows, std::max<u32>(width, 12), refs, {}, nullptr, nullptr};
    layout.table(b);
    return rows;
}

std::string md_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (std::ispunct(static_cast<unsigned char>(c))) out += '\\';
        out += c == '\n' || c == '\t' || c == '\r' ? ' ' : c;
    }
    return out;
}

std::string md_math_text(std::string_view tex, u8 glyphs) {
    std::string out;
    tex_into(tex, glyphs, 0, out);
    return std::string(trim(out));
}

std::string md_row_text(const MdRow& r) {
    std::string out;
    for (const MdRun& run : r.runs) out += run.text;
    for (const MdPic& p : r.pics)
        if (p.part == 0) out += "picture: " + p.src + " " + std::to_string(p.cols) + "x" + std::to_string(p.rows);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace blit
