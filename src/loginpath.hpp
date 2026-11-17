#pragma once

namespace blit {

// Started from the Finder or the Dock, blit has launchd's PATH
// (/usr/bin:/bin:/usr/sbin:/sbin), which has none of Homebrew's programs,
// nor clangd, pyright, cmake or uv. It takes the PATH a login shell has
// instead: the one kept from the last time at once, and the shell asked
// again in a process of its own for the next time (only the first time is
// waited for, up to three seconds). Started from a terminal, blit keeps the
// terminal's. Call before any thread starts.
void adopt_login_path();

}  // namespace blit
