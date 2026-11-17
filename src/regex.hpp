#pragma once
// Regular expressions as Vim writes them (Neovim's documentation of them:
// "magic" unless \v, \m, \M or \V in the pattern says otherwise), looked
// for where the text lies: in a Buffer's pieces, or in a string.
//
// A pattern is compiled to a program for a Pike VM, which runs all the ways
// a match could go at once, a character at a time, so no pattern takes time
// exponential in the text (there is no backtracking): at worst the text's
// length times the program's. Three things make the usual cases fast. A
// pattern with nothing special in it is not run here at all: plain() says
// so, and the caller looks for it as text, as it always has. A pattern that
// cannot take in a line break and has a piece of plain text that every
// match must contain has that looked for first, with memchr, and only the
// lines it is on go through the VM. Any other pattern that stays on one line
// is passed over the text by a DFA built as it goes (a state for each set
// of the VM's places that turns up), and only a line it says has a match is
// run through the VM, to find where.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.hpp"

namespace blit {

// A match: where it is, as Vim has it (\zs and \ze move its ends), where it
// began (before a \zs), and the groups \( \) caught. Byte offsets.
struct RegexMatch {
    static constexpr u64 kUnset = ~0ull;
    u64 start = 0, end = 0;
    u64 anchor = 0;
    u64 group[10][2] = {};  // [0]: the match; [1] to [9]: \1 to \9 (kUnset: the group took part in nothing)
};

struct RegexProgram;

class VimRegex {
public:
    struct Options {
        bool ignore_case = false;  // as Vim's 'ignorecase' ("\c" and "\C" in the pattern still decide)
        bool smart_case = false;   // with ignore_case: a capital in the pattern makes it exact ('smartcase')
        std::string last_sub;      // what "~" stands for: the last substitute string
    };
    // Null, with Vim's message in *error, for a pattern that is not one.
    static std::shared_ptr<const VimRegex> compile(std::string_view pattern, const Options& o, std::string* error);

    // Nothing special in it: the text to look for (already without \c and
    // the like), and whether as a whole word (it was \<text\>).
    bool plain() const { return plain_; }
    const std::string& plain_text() const { return plain_text_; }
    bool plain_word() const { return plain_word_; }
    bool ignore_case() const { return icase_; }
    bool multiline() const;     // a match may take in a line break
    bool has_zs() const;        // a match may start after where it began (\zs)
    bool can_be_empty() const;  // something matches nothing (x*, ^)
    // A piece of plain text every match has (for a search elsewhere to
    // narrow the files down): empty if there is none.
    const std::string& required() const;

    // Looks for matches: the first whose start (before any \zs) is in [from,
    // to), the leftmost and, of those that start there, the one Vim's rules
    // choose (the first alternative that matches; the longest of a "*", the
    // shortest of a "\{-}"). A Searcher keeps what it has worked out (the
    // DFA's states) from one call to the next, for a run of searches; it is
    // for one thread.
    class Searcher {
    public:
        explicit Searcher(const VimRegex& re);
        ~Searcher();
        Searcher(const Searcher&) = delete;
        Searcher& operator=(const Searcher&) = delete;
        bool next(const Buffer& buf, u64 from, u64 to, RegexMatch* m);
        bool next(std::string_view text, u64 from, u64 to, RegexMatch* m);

        struct State;

    private:
        const VimRegex& re_;
        std::unique_ptr<State> st_;
    };

    // One search, without a Searcher.
    bool search(const Buffer& buf, u64 from, u64 to, RegexMatch* m) const;
    bool search(std::string_view text, u64 from, u64 to, RegexMatch* m) const;

    ~VimRegex();

private:
    VimRegex() = default;
    bool plain_ = false, plain_word_ = false, icase_ = false;
    std::string plain_text_;
    std::unique_ptr<RegexProgram> prog_;
};

// What a match becomes, by the rules of Vim's :s: "&" and "\0" are the
// match, "\1" to "\9" the groups, "\r" and "\n" a new line, "\t" a tab, "\u"
// and "\l" the next character in upper or lower case, "\U" and "\L" all that
// follows until "\E" or "\e"; "\&" and "\\" are themselves. (A "~" for the
// last substitute string is put in beforehand, by whoever keeps that.)
// `text(a, b)` gives the bytes [a, b) of what was searched.
void regex_expand(std::string_view replacement, const RegexMatch& m, const std::function<std::string(u64, u64)>& text, std::string* out);
// Whether, by those rules, every match becomes the same (there is no
// group, no "&" and no change of case in it).
bool regex_replacement_plain(std::string_view replacement);

// A pattern for text as it stands (escaped so that Vim's magic reads it as
// that text): what Cmd+F puts on the command line.
std::string regex_escape(std::string_view text, char delimiter = '/');

}  // namespace blit
