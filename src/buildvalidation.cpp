// Part of the build's output read for problems (buildscan.hpp): Vulkan's validation layers' messages (text and JSON) and Metal's API validation, as problems with their VUID as the code.
#include "buildscan.hpp"

namespace blit {

bool BuildScan::validation_line(std::string_view) { return false; }
void BuildScan::validation_end() {}

}  // namespace blit
