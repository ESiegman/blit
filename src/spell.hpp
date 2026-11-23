#pragma once
// Spelling, in English: the system's checker (NSSpellChecker on a Mac,
// spell_mac.m; elsewhere Hunspell's en_US, loaded when first wanted). Safe
// to call from any thread.
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common.hpp"

namespace blit {

// A checker and its words are there (false: spelling is not checked).
bool spell_ready();
// Why nothing is checked, in words, once spell_ready() has said no
// ("hunspell has no English dictionary on this machine"); "" if it can be.
std::string spell_missing();
bool spell_ok(std::string_view word);
std::vector<std::string> spell_suggest(std::string_view word);
// Not wrong from now on: the system's own list on a Mac; elsewhere blit's
// (a file of words in its data folder, read at the start).
void spell_learn(std::string_view word);

// The words worth checking in some text: (offset, length) of each run of
// letters (with apostrophes inside) of three or more, that is not a name
// in code (a digit, an underscore, a capital past the first letter, a
// letter run on from a dot or a slash: a path, an address).
std::vector<std::pair<u32, u32>> spell_words(std::string_view text);

}  // namespace blit
