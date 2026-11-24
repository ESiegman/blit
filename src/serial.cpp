#include "serial.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace blit {

#if defined(__linux__)
bool serial_linux_speed(int fd, u32 baud);  // (serial_linux.cpp: termios2, whose header cannot be had with termios.h)
#endif

namespace {

struct Speed {
    u32 baud;
    speed_t code;
};
// The speeds termios names on both systems (and those Linux has beyond).
constexpr Speed kSpeeds[] = {
    {50, B50},     {75, B75},     {110, B110},     {134, B134},     {150, B150},     {200, B200},       {300, B300},       {600, B600},
    {1200, B1200}, {1800, B1800}, {2400, B2400},   {4800, B4800},   {9600, B9600},   {19200, B19200},   {38400, B38400},   {57600, B57600},
    {115200, B115200}, {230400, B230400},
#if defined(__linux__)
    {460800, B460800}, {500000, B500000}, {576000, B576000}, {921600, B921600}, {1000000, B1000000}, {1152000, B1152000},
    {1500000, B1500000}, {2000000, B2000000}, {2500000, B2500000}, {3000000, B3000000}, {3500000, B3500000}, {4000000, B4000000},
#endif
};

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Why a port could not be opened, in words (the group the device belongs
// to, when this user is not in it).
std::string why_not(const std::string& path, int e) {
    const std::string name = base_name(path);
    if (e == EBUSY || would_block(e)) return name + " is in use by another program";
    if (e == ENOENT || e == ENXIO || e == ENODEV) return name + " is not there";
    if (e == EACCES || e == EPERM) {
        struct stat st;
        std::string group;
        if (::stat(path.c_str(), &st) == 0)
            if (const struct group* g = ::getgrgid(st.st_gid)) group = g->gr_name;
        const passwd* me = ::getpwuid(::getuid());
        const std::string who = me ? me->pw_name : "this user";
        if (!group.empty() && group != "root") return "Not allowed to open " + name + ": it belongs to the " + group + " group, and " + who + " is not in it";
        return "Not allowed to open " + name;
    }
    return "Cannot open " + name + ": " + std::strerror(e);
}

}  // namespace

std::string serial_framing(const SerialConfig& c) {
    std::string out = std::to_string(c.data) + c.parity + std::to_string(c.stop);
    if (c.flow == 'R') out += " RTS/CTS";
    else if (c.flow == 'X') out += " XON/XOFF";
    return out;
}

std::string serial_text(const SerialConfig& c) { return std::to_string(c.baud) + " " + serial_framing(c); }

bool serial_parse(std::string_view text, SerialConfig* c) {
    SerialConfig out = *c;
    bool any = false;
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ' ' || text[at] == ',' || text[at] == '\t')) ++at;
        size_t end = at;
        while (end < text.size() && text[end] != ' ' && text[end] != ',' && text[end] != '\t') ++end;
        if (end == at) break;
        const std::string w = lower(text.substr(at, end - at));
        at = end;
        any = true;
        if (std::all_of(w.begin(), w.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            if (w.size() > 8) return false;
            const unsigned long baud = std::strtoul(w.c_str(), nullptr, 10);
            if (baud < 50 || baud > 20'000'000) return false;
            out.baud = static_cast<u32>(baud);
        } else if (w.size() == 3 && w[0] >= '5' && w[0] <= '8' && (w[1] == 'n' || w[1] == 'e' || w[1] == 'o') && (w[2] == '1' || w[2] == '2')) {
            out.data = static_cast<u8>(w[0] - '0');
            out.parity = static_cast<char>(std::toupper(static_cast<unsigned char>(w[1])));
            out.stop = static_cast<u8>(w[2] - '0');
        } else if (w == "rts/cts" || w == "rtscts" || w == "hardware") {
            out.flow = 'R';
        } else if (w == "xon/xoff" || w == "xonxoff" || w == "software") {
            out.flow = 'X';
        } else if (w == "none" || w == "no") {
            out.flow = 'N';
        } else {
            return false;
        }
    }
    if (!any) return false;
    *c = out;
    return true;
}

bool serial_open(const std::string& path, const SerialConfig& c, int* fd, std::string* err) {
    const int f = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (f < 0) {
        *err = why_not(path, errno);
        return false;
    }
    // Another program that opens it now is refused (but root); one that
    // takes pyserial's lock (esptool, miniterm) is refused too, and this
    // one is when such a program has it.
    if (::flock(f, LOCK_EX | LOCK_NB) != 0 && would_block(errno)) {
        ::close(f);
        *err = why_not(path, EBUSY);
        return false;
    }
    ::ioctl(f, TIOCEXCL);
    if (!serial_configure(f, c, err)) {
        ::close(f);
        return false;
    }
    ::tcflush(f, TCIOFLUSH);  // (what was waiting before it was opened is not the program's now)
    *fd = f;
    return true;
}

bool serial_configure(int fd, const SerialConfig& c, std::string* err) {
    termios t{};
    if (::tcgetattr(fd, &t) != 0) {
        *err = std::string("Not a serial port: ") + std::strerror(errno);
        return false;
    }
    ::cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    t.c_cflag |= c.data == 5 ? CS5 : c.data == 6 ? CS6 : c.data == 7 ? CS7 : CS8;
    if (c.parity != 'N') t.c_cflag |= PARENB | (c.parity == 'O' ? PARODD : 0);
    if (c.stop == 2) t.c_cflag |= CSTOPB;
    if (c.flow == 'R') t.c_cflag |= CRTSCTS;
    t.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    if (c.flow == 'X') t.c_iflag |= IXON | IXOFF;
    // A read waits for a byte (with O_NONBLOCK: says EAGAIN when none has
    // come): VMIN 0 would make an empty read look like the port's end.
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    const Speed* known = nullptr;
    for (const Speed& s : kSpeeds)
        if (s.baud == c.baud) known = &s;
    ::cfsetispeed(&t, known ? known->code : B9600);
    ::cfsetospeed(&t, known ? known->code : B9600);
    if (::tcsetattr(fd, TCSANOW, &t) != 0) {
        *err = "Cannot set " + serial_text(c) + ": " + std::strerror(errno);
        return false;
    }
    if (!known) {  // (a speed termios has no name for: the system's own way)
#if defined(__APPLE__)
        speed_t s = c.baud;
        if (::ioctl(fd, IOSSIOSPEED, &s) != 0) {
            *err = "Cannot set the speed " + std::to_string(c.baud) + ": " + std::strerror(errno);
            return false;
        }
#elif defined(__linux__)
        if (!serial_linux_speed(fd, c.baud)) {
            *err = "Cannot set the speed " + std::to_string(c.baud) + ": " + std::strerror(errno);
            return false;
        }
#else
        *err = "No speed " + std::to_string(c.baud) + " here";
        return false;
#endif
    }
    return true;
}

bool serial_set_lines(int fd, int dtr, int rts, std::string* err) {
    int bits = 0;
    if (::ioctl(fd, TIOCMGET, &bits) != 0) {
        *err = std::strerror(errno);
        return false;
    }
    if (dtr >= 0) bits = dtr ? bits | TIOCM_DTR : bits & ~TIOCM_DTR;
    if (rts >= 0) bits = rts ? bits | TIOCM_RTS : bits & ~TIOCM_RTS;
    if (::ioctl(fd, TIOCMSET, &bits) != 0) {
        *err = std::strerror(errno);
        return false;
    }
    return true;
}

bool serial_get_lines(int fd, bool* dtr, bool* rts) {
    int bits = 0;
    if (::ioctl(fd, TIOCMGET, &bits) != 0) return false;
    *dtr = bits & TIOCM_DTR;
    *rts = bits & TIOCM_RTS;
    return true;
}

bool serial_break(int fd, std::string* err) {
    if (::tcsendbreak(fd, 0) != 0) {
        *err = std::strerror(errno);
        return false;
    }
    return true;
}

std::string serial_device(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf)) return buf;
    return path;
}

std::string serial_by_id(const std::string& dev) {
#if defined(__linux__)
    const std::string real = serial_device(dev);
    DIR* d = ::opendir("/dev/serial/by-id");
    if (!d) return {};
    std::string found;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string link = std::string("/dev/serial/by-id/") + e->d_name;
        if (serial_device(link) == real) {
            found = link;
            break;
        }
    }
    ::closedir(d);
    return found;
#else
    (void)dev;
    return {};
#endif
}

}  // namespace blit
