#pragma once
// Snippets, as language servers write them (and as one's own are written
// in Lua): text with tab stops ($1, ${1}, ${1:what goes there, perhaps
// with ${2:more} in it}, ${1|one,two|}, and $0 where the cursor ends),
// variables ($TM_FILENAME, ${TM_SELECTED_TEXT:what if none}) and escapes
// (\$, \}, \\). A transformation (${1/re/fmt/}) is read as its stop, the
// transformation left out.
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct Snippet {
    std::string text;  // what goes in, each stop's first placeholder (or choice) in it
    struct Span {
        u32 start = 0, end = 0;  // bytes of text
    };
    struct Stop {
        u32 index = 0;                     // its number ($0 last of all)
        std::vector<Span> spans;           // where it is: more than one place mirrors the first
        std::vector<std::string> choices;  // ${1|a,b|}'s
    };
    std::vector<Stop> stops;  // in the order Tab goes through them: 1, 2 ... and $0 (or the end) last
};

// A variable's value (false: it has none, and its default, or nothing, goes
// in its place).
using SnippetVars = std::function<bool(std::string_view name, std::string* value)>;
Snippet parse_snippet(std::string_view body, const SnippetVars& vars = {});

// The snippet as it goes in at a line: a tab in it is one level of
// indentation (`indent`), and each line after the first starts with the
// line's own leading blanks (`lead`). The stops move with the text.
void indent_snippet(Snippet& s, std::string_view lead, std::string_view indent);

}  // namespace blit
