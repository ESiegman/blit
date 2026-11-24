// A serial port's speed when termios has no name for it, on Linux: through
// termios2 (BOTHER), whose header cannot be had with termios.h, so here
// alone.
#include <asm/termbits.h>

#include "common.hpp"

extern "C" int ioctl(int fd, unsigned long request, ...);

namespace blit {

bool serial_linux_speed(int fd, u32 baud) {
    termios2 t{};
    if (ioctl(fd, TCGETS2, &t) != 0) return false;
    t.c_cflag &= ~static_cast<tcflag_t>(CBAUD);
    t.c_cflag |= BOTHER;
    t.c_ispeed = baud;
    t.c_ospeed = baud;
    return ioctl(fd, TCSETS2, &t) == 0;
}

}  // namespace blit
