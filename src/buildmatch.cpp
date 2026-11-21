// Part of the build's output read for problems (buildscan.hpp): init.lua's own patterns (blit.build.matcher), compiled into a table of native patterns, tried before the built-in kinds.
#include "buildscan.hpp"

namespace blit {

bool BuildScan::matcher_line(std::string_view) { return false; }
void BuildScan::matcher_end() {}

}  // namespace blit
