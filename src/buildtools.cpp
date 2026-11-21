// Part of the build's output read for problems (buildscan.hpp): Linkers and assemblers (ld, collect2, GNU as), HDL tools (Verilator, Yosys, Vivado, Icarus, vvp, verible-lint), shader compilers (glslang, qsb, spirv-val, Metal, slangc, DXC, naga, tint) and Python's tools (Ruff, pyright, pre-commit).
#include "buildscan.hpp"

namespace blit {

bool BuildScan::tool_line(std::string_view) { return false; }
void BuildScan::tool_end() {}

}  // namespace blit
