#pragma once
// Two small pattern matchers, used to evaluate predicates in tree-sitter
// highlight queries (#lua-match? and #match?), and the Lua one for
// init.lua's patterns for a build's output (blit.build.matcher). Both
// search: the pattern may match anywhere in the text unless it is anchored
// with ^ or $.
#include <memory>
#include <string>
#include <string_view>

namespace blit {

// Lua patterns, as string.find understands them: %a %d %l %s %u %w %x %p,
// sets, the quantifiers * + - ?, and anchors. %b and %f are not supported
// and never match.
bool lua_match(std::string_view text, std::string_view pattern);

// As lua_match, and what its captures took ("()" takes nothing, at its
// place): how many there are (caps has room for kMaxLuaCaptures), or -1
// when it does not match.
inline constexpr int kMaxLuaCaptures = 16;
int lua_find(std::string_view text, std::string_view pattern, std::string_view* caps);
// How many captures a pattern has, or -1 and why when it is not one
// lua_find takes (a ( or [ not closed, a lone % at its end, %b or %f).
int lua_pattern_check(std::string_view pattern, std::string* why);

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
