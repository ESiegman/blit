// Part of the build's output read for problems (buildscan.hpp): Test frameworks' results (GoogleTest, Catch2, doctest, Unity), and what a run comes to for its task's row: the tests' count, a firmware's size, timing's slack.
#include "buildscan.hpp"

namespace blit {

bool BuildScan::test_line(std::string_view) { return false; }
void BuildScan::test_end() {}

}  // namespace blit
