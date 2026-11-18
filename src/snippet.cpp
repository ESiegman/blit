#include "snippet.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace blit {

namespace {

struct Parser {
    std::string_view s;
    size_t i = 0;
    const SnippetVars& vars;
    std::string out;
    std::map<u32, Snippet::Stop> stops;

    bool digit(size_t at) const { return at < s.size() && s[at] >= '0' && s[at] <= '9'; }
    bool name_char(size_t at) const { return at < s.size() && (std::isalnum(static_cast<unsigned char>(s[at])) || s[at] == '_'); }
    u32 number() {
        u32 n = 0;
        while (digit(i)) n = n * 10 + static_cast<u32>(s[i++] - '0');
        return n;
    }
    void stop_at(u32 index, u32 start, u32 end) {
        Snippet::Stop& st = stops[index];
        st.index = index;
        st.spans.push_back({start, end});
    }
    // Text until one of `ends` (not taken), the escapes read: inside a
    // placeholder "}" ends it (and "$" and "\" can be escaped); a choice's
    // text ends at "," or "|".
    void until(std::string_view ends) {
        while (i < s.size() && ends.find(s[i]) == std::string_view::npos) {
            if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '$' || s[i + 1] == '}' || s[i + 1] == '\\' || s[i + 1] == ',' || s[i + 1] == '|')) {
                out += s[i + 1];
                i += 2;
            } else if (s[i] == '$') {
                dollar();
            } else {
                out += s[i++];
            }
        }
    }
    // Past a "${...}" whose rest is not understood (its braces matched).
    void skip_braces() {
        int depth = 1;
        while (i < s.size() && depth > 0) {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;
            else if (s[i] == '{') ++depth;
            else if (s[i] == '}') --depth;
            ++i;
        }
    }
    void dollar() {
        ++i;  // ($)
        if (digit(i)) {  // $1
            const u32 at = static_cast<u32>(out.size());
            stop_at(number(), at, at);
            return;
        }
        if (name_char(i)) {  // $NAME
            const size_t from = i;
            while (name_char(i)) ++i;
            std::string value;
            if (vars && vars(s.substr(from, i - from), &value)) out += value;
            return;
        }
        if (i < s.size() && s[i] == '{') {
            ++i;
            if (digit(i)) {
                const u32 index = number();
                const u32 start = static_cast<u32>(out.size());
                if (i < s.size() && s[i] == ':') {  // ${1:placeholder}
                    ++i;
                    until("}");
                    if (i < s.size()) ++i;
                    stop_at(index, start, static_cast<u32>(out.size()));
                } else if (i < s.size() && s[i] == '|') {  // ${1|a,b|}
                    ++i;
                    std::vector<std::string> choices;
                    for (;;) {
                        const size_t was = out.size();
                        until(",|");
                        choices.push_back(out.substr(was));
                        out.resize(was);
                        if (i >= s.size() || s[i] == '|') break;
                        ++i;  // (,)
                    }
                    if (i < s.size()) ++i;  // (|)
                    if (i < s.size() && s[i] == '}') ++i;
                    if (!choices.empty()) out += choices[0];
                    stop_at(index, start, static_cast<u32>(out.size()));
                    Snippet::Stop& st = stops[index];
                    if (st.choices.empty()) st.choices = std::move(choices);
                } else {  // ${1}, or ${1/re/fmt/opts} (the transformation left out)
                    skip_braces();
                    stop_at(index, start, start);
                }
                return;
            }
            if (name_char(i)) {  // ${NAME} or ${NAME:default}
                const size_t from = i;
                while (name_char(i)) ++i;
                std::string value;
                const bool known = vars && vars(s.substr(from, i - from), &value);
                if (i < s.size() && s[i] == ':') {
                    ++i;
                    const size_t was = out.size();
                    until("}");
                    if (i < s.size()) ++i;
                    if (known && !value.empty()) out.resize(was), out += value;
                } else {
                    skip_braces();
                    if (known) out += value;
                }
                return;
            }
            out += "${";
            return;
        }
        out += '$';
    }
};

}  // namespace

Snippet parse_snippet(std::string_view body, const SnippetVars& vars) {
    Parser p{body, 0, vars, {}, {}};
    while (p.i < body.size()) {
        if (body[p.i] == '\\' && p.i + 1 < body.size() && (body[p.i + 1] == '$' || body[p.i + 1] == '}' || body[p.i + 1] == '\\')) {
            p.out += body[p.i + 1];
            p.i += 2;
        } else if (body[p.i] == '$') {
            p.dollar();
        } else {
            p.out += body[p.i++];
        }
    }
    Snippet s;
    s.text = std::move(p.out);
    // 1, 2 ... in order; then $0, or (none) the end.
    for (auto& [index, st] : p.stops)
        if (index != 0) s.stops.push_back(std::move(st));
    if (const auto zero = p.stops.find(0); zero != p.stops.end()) s.stops.push_back(std::move(zero->second));
    else s.stops.push_back({0, {{static_cast<u32>(s.text.size()), static_cast<u32>(s.text.size())}}, {}});
    return s;
}

void indent_snippet(Snippet& s, std::string_view lead, std::string_view indent) {
    std::string text;
    std::vector<u32> moved(s.text.size() + 1);  // each byte's place in the new text
    for (size_t k = 0; k < s.text.size(); ++k) {
        moved[k] = static_cast<u32>(text.size());
        const char c = s.text[k];
        if (c == '\t') {
            text += indent;
        } else {
            text += c;
            if (c == '\n') text += lead;
        }
    }
    moved[s.text.size()] = static_cast<u32>(text.size());
    for (Snippet::Stop& st : s.stops)
        for (Snippet::Span& sp : st.spans) {
            // (A stop at a line's start comes after its leading blanks: the byte after a new line goes there.)
            const u32 a = moved[sp.start];
            sp.end = sp.end == sp.start ? a : moved[sp.end];
            sp.start = a;
        }
    s.text = std::move(text);
}

}  // namespace blit
