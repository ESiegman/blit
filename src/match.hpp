#pragma once
// Two small pattern matchers, used to evaluate predicates in tree-sitter
// highlight queries (#lua-match? and #match?). Both search: the pattern may
// match anywhere in the text unless it is anchored with ^ or $.
#include <memory>
#include <string_view>

namespace blit {

// Lua patterns, as string.find understands them: %a %d %l %s %u %w %x %p,
// sets, the quantifiers * + - ?, and anchors. %b and %f are not supported
// and never match.
bool lua_match(std::string_view text, std::string_view pattern);

// A regular expression in the common subset highlight queries use:
// alternation, groups, sets, . * + ?, anchors, and \d \w \s. A leading \c
// (as in Vim) makes it ignore case. Compile once, search many times.
class Regex {
public:
    explicit Regex(std::string_view pattern);
    Regex(Regex&&) noexcept;
    ~Regex();
    bool ok() const { return ok_; }  // false: the pattern did not parse, and nothing matches
    bool search(std::string_view text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ok_ = false;
};

bool regex_match(std::string_view text, std::string_view pattern);

}  // namespace blit
