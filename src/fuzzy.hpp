#pragma once
// Fuzzy matching of the kind fzf does: the letters typed must all be in the
// candidate, in that order, but not necessarily next to one another. What
// was typed is split at spaces into terms, and every term has to match.
// Letters that follow one another, and letters at the start of a word
// (after a slash, an underscore, a capital after a small letter), count for
// more than letters scattered about.
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

// One word of what was typed, made ready for matching. A term with no
// capital in it matches either case; one with a capital matches as typed.
struct FuzzyTerm {
    std::string text;  // in lower case unless exact_case
    bool exact_case = false;
};

std::vector<FuzzyTerm> fuzzy_terms(std::string_view typed);

// How well `text` matches one term: -1 if it does not, otherwise the higher
// the better. `at`, if given, has the byte position in `text` of each
// matched letter added to it.
int fuzzy_score(const FuzzyTerm& term, std::string_view text, std::vector<u32>* at = nullptr);

// A closer match than fuzzy_score() asks for: the term whole somewhere in
// the text, or each letter just after the one before it or at the start of
// a word ("lnum" in "line numbers"; not "tot" in "editor.tab_width").
bool fuzzy_word_starts(const FuzzyTerm& term, std::string_view text);

// The same for a path and all the terms. A term found in the file's name
// alone counts for more than one that needs the folders too, and of two
// equally good matches the shorter path wins.
int fuzzy_path_score(std::span<const FuzzyTerm> terms, std::string_view path, std::vector<u32>* at = nullptr);

// Where a term is in a piece of text as it stands (no gaps): the first
// place at or after `from`, or npos. For searching in files.
size_t find_term(std::string_view hay, const FuzzyTerm& term, size_t from = 0);

// Where `needle` is in `hay` as it stands: the first place at or after
// `from`, and before `until`, where it starts (and all of it is in `hay`);
// npos if there is none. With any_case a letter matches in either case.
size_t find_text(std::string_view hay, std::string_view needle, bool any_case, size_t from = 0, size_t until = std::string_view::npos);

}  // namespace blit
