#pragma once
// A file's assembly as its compiler makes it (-S -g), read for showing
// beside the source: the code and its labels, without the directives and
// the debug information's sections, each line with the source line it is
// of (from the .file and .loc directives GCC and Clang write).
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

struct AsmView {
    std::string text;           // a line each, the last without its end
    std::vector<u32> line;      // for each of its lines: the source line (from 1; 0: none, or another file's)
    std::vector<u32> off;       // its colours: from each offset on, a style (3 a label, 4 a comment)
    std::vector<u8> style;
};
// `source`: the file compiled, as a full path (its lines are the ones known).
AsmView read_asm(std::string_view text, std::string_view source);

// A compile_commands.json's command for `file` turned into one that writes
// the file's assembly with line information to its standard output: the
// words to run and the folder to run them in. False: no entry for it.
bool asm_command(std::string_view compile_commands, std::string_view file, std::vector<std::string>* argv, std::string* dir);

}  // namespace blit
