#include "usb.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

namespace blit {

namespace {

// A small file's text, without its line's end ("" if there is none).
std::string read_small(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    char buf[512];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    ::close(fd);
    std::string s(buf, n > 0 ? static_cast<size_t>(n) : 0);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::vector<std::string> names_in(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (const dirent* e = ::readdir(d))
        if (e->d_name[0] != '.') out.emplace_back(e->d_name);
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

unsigned hex_of(const std::string& s) { return static_cast<unsigned>(std::strtoul(s.c_str(), nullptr, 16)); }

void by_location(std::vector<UsbDevice>& v) {
    std::stable_sort(v.begin(), v.end(), [](const UsbDevice& a, const UsbDevice& b) { return a.location < b.location; });
    for (UsbDevice& d : v) std::stable_sort(d.ports.begin(), d.ports.end(), [](const UsbPort& a, const UsbPort& b) { return a.iface < b.iface; });
}

#if defined(__APPLE__)
std::string cf_text(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
    const CFStringRef s = static_cast<CFStringRef>(v);
    if (const char* p = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return p;
    const CFIndex most = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(most), '\0');
    if (!CFStringGetCString(s, out.data(), most, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

// A property of an entry: its text, or (number) its value.
std::string prop_text(io_registry_entry_t e, const char* key) {
    const CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    const CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    std::string out = cf_text(v);
    if (v) CFRelease(v);
    return out;
}
bool prop_number(io_registry_entry_t e, const char* key, long long* out) {
    const CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    const CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    bool ok = false;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) ok = CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberLongLongType, out);
    if (v) CFRelease(v);
    return ok;
}
// (The names macOS gives a device's strings changed with IOUSBHostDevice;
// both are asked for.)
std::string prop_either(io_registry_entry_t e, const char* key, const char* other) {
    std::string s = prop_text(e, key);
    return s.empty() ? prop_text(e, other) : s;
}

// Each IOUSBHostDevice; under each, its IOUSBHostInterfaces (their number
// and kUSBString), and under each of those the IOSerialBSDClient a serial
// driver made (its IOCalloutDevice, the /dev/cu.* path): the route Apple's
// SerialPortSample and pyserial's list_ports_osx take, from the device down
// rather than from the port up.
std::vector<UsbDevice> usb_list_iokit() {
    std::vector<UsbDevice> out;
    io_iterator_t devices = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &devices) != KERN_SUCCESS) return out;
    while (const io_object_t dev = IOIteratorNext(devices)) {
        UsbDevice d;
        long long n = 0;
        if (prop_number(dev, "idVendor", &n)) d.vid = static_cast<u16>(n);
        if (prop_number(dev, "idProduct", &n)) d.pid = static_cast<u16>(n);
        if (prop_number(dev, "bDeviceClass", &n)) d.cls = static_cast<u8>(n);
        if (prop_number(dev, "locationID", &n)) {
            char at[24];
            std::snprintf(at, sizeof at, "0x%08llx", static_cast<unsigned long long>(n) & 0xffffffffull);
            d.location = at;
        }
        d.manufacturer = prop_either(dev, "kUSBVendorString", "USB Vendor Name");
        d.product = prop_either(dev, "kUSBProductString", "USB Product Name");
        d.serial = prop_either(dev, "kUSBSerialNumberString", "USB Serial Number");
        struct If {
            long long num = -1, cls = -1;
            std::string name;
        };
        std::vector<If> ifs;
        io_iterator_t kids = 0;
        if (IORegistryEntryGetChildIterator(dev, kIOServicePlane, &kids) == KERN_SUCCESS) {
            while (const io_object_t iface = IOIteratorNext(kids)) {
                if (IOObjectConformsTo(iface, "IOUSBHostInterface")) {
                    long long num = -1, cls = -1;
                    prop_number(iface, "bInterfaceNumber", &num);
                    prop_number(iface, "bInterfaceClass", &cls);
                    const std::string name = prop_text(iface, "kUSBString");
                    if (!name.empty()) d.interfaces.push_back(name);
                    ifs.push_back({num, cls, name});
                    io_iterator_t under = 0;
                    if (IORegistryEntryCreateIterator(iface, kIOServicePlane, kIORegistryIterateRecursively, &under) == KERN_SUCCESS) {
                        while (const io_object_t e = IOIteratorNext(under)) {
                            if (IOObjectConformsTo(e, "IOSerialBSDClient")) {
                                const std::string path = prop_text(e, "IOCalloutDevice");
                                if (!path.empty()) d.ports.push_back({path, static_cast<int>(num), name});
                            }
                            IOObjectRelease(e);
                        }
                        IOObjectRelease(under);
                    }
                }
                IOObjectRelease(iface);
            }
            IOObjectRelease(kids);
        }
        // A CDC port's driver sits on its data interface (class 0x0a),
        // which has no name; Linux puts the tty on the control interface
        // (class 0x02) before it, which has the name ("Black Magic GDB
        // Server"). Named as Linux names it.
        for (UsbPort& p : d.ports)
            for (const If& data : ifs)
                if (data.num == p.iface && data.cls == 0x0a)
                    for (const If& control : ifs)
                        if (control.num == data.num - 1 && control.cls == 0x02) p.iface = static_cast<int>(control.num), p.name = control.name;
        IOObjectRelease(dev);
        out.push_back(std::move(d));
    }
    IOObjectRelease(devices);
    return out;
}
#endif

void clean(std::string& out, const std::string& s) {
    for (const char c : s) out += c == '\t' || c == '\n' || c == '\r' ? ' ' : c;
}

}  // namespace

std::vector<UsbDevice> usb_list_sysfs(const std::string& root) {
    std::vector<UsbDevice> out;
    const std::vector<std::string> names = names_in(root);
    for (const std::string& name : names) {
        if (name.find(':') != std::string::npos) continue;
        const std::string at = root + "/" + name + "/";
        const std::string vid = read_small(at + "idVendor");
        if (vid.empty()) continue;
        UsbDevice d;
        d.vid = static_cast<u16>(hex_of(vid));
        d.pid = static_cast<u16>(hex_of(read_small(at + "idProduct")));
        d.cls = static_cast<u8>(hex_of(read_small(at + "bDeviceClass")));
        d.manufacturer = read_small(at + "manufacturer");
        d.product = read_small(at + "product");
        d.serial = read_small(at + "serial");
        d.location = name;
        // Its interfaces: "1-2.3:1.0" (configuration 1, interface 0).
        for (const std::string& in : names) {
            if (!in.starts_with(name + ":")) continue;
            const std::string i = root + "/" + in + "/";
            const std::string num = read_small(i + "bInterfaceNumber");
            const int iface = num.empty() ? -1 : static_cast<int>(hex_of(num));
            const std::string label = read_small(i + "interface");
            if (!label.empty()) d.interfaces.push_back(label);
            for (const std::string& t : names_in(i)) {
                if (t == "tty") {  // (cdc-acm: tty/ttyACM0)
                    for (const std::string& tty : names_in(i + "tty")) d.ports.push_back({"/dev/" + tty, iface, label});
                } else if (t.starts_with("tty")) {  // (a usb-serial driver: ttyUSB0/)
                    d.ports.push_back({"/dev/" + t, iface, label});
                }
            }
        }
        out.push_back(std::move(d));
    }
    by_location(out);
    return out;
}

std::vector<UsbDevice> usb_list() {
    if (const char* root = std::getenv("BLIT_USB_ROOT"); root && *root) return usb_list_sysfs(root);
#if defined(__APPLE__)
    std::vector<UsbDevice> out = usb_list_iokit();
    by_location(out);
    return out;
#elif defined(__linux__)
    return usb_list_sysfs("/sys/bus/usb/devices");
#else
    return {};
#endif
}

std::string usb_text(const std::vector<UsbDevice>& devices) {
    std::string out = "blit-usb 1\n";
    for (const UsbDevice& d : devices) {
        char ids[40];
        std::snprintf(ids, sizeof ids, "d\t%04x\t%04x\t%02x\t", d.vid, d.pid, d.cls);
        out += ids;
        clean(out, d.location);
        out += '\t';
        clean(out, d.manufacturer);
        out += '\t';
        clean(out, d.product);
        out += '\t';
        clean(out, d.serial);
        out += '\n';
        for (const std::string& i : d.interfaces) {
            out += "i\t";
            clean(out, i);
            out += '\n';
        }
        for (const UsbPort& p : d.ports) {
            out += "p\t" + std::to_string(p.iface) + "\t";
            clean(out, p.path);
            out += '\t';
            clean(out, p.name);
            out += '\n';
        }
    }
    return out;
}

bool usb_parse(std::string_view text, std::vector<UsbDevice>* out) {
    out->clear();
    if (!text.starts_with("blit-usb 1\n")) return false;
    text.remove_prefix(11);
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        std::vector<std::string> f;
        for (;;) {
            const size_t tab = line.find('\t');
            f.emplace_back(line.substr(0, tab));
            if (tab == std::string_view::npos) break;
            line.remove_prefix(tab + 1);
        }
        if (f[0] == "d" && f.size() >= 8) {
            UsbDevice d;
            d.vid = static_cast<u16>(hex_of(f[1]));
            d.pid = static_cast<u16>(hex_of(f[2]));
            d.cls = static_cast<u8>(hex_of(f[3]));
            d.location = f[4];
            d.manufacturer = f[5];
            d.product = f[6];
            d.serial = f[7];
            out->push_back(std::move(d));
        } else if (f[0] == "i" && f.size() >= 2 && !out->empty()) {
            out->back().interfaces.push_back(f[1]);
        } else if (f[0] == "p" && f.size() >= 4 && !out->empty()) {
            out->back().ports.push_back({f[2], std::atoi(f[1].c_str()), f[3]});
        } else if (!f[0].empty()) {
            return false;
        }
    }
    return true;
}

}  // namespace blit
