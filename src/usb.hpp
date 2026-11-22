#pragma once
// The USB devices plugged into this machine, read natively: on macOS
// through IOKit (each IOUSBHostDevice, its interfaces, and the serial ports
// under them), on Linux from /sys/bus/usb/devices (each device, its
// interfaces, and the ttys under them). No tool is run. What a device is
// (a debug probe, a serial bridge) is probes.cpp's to say; this only reads.
//
// blit's agent has it too (blit-agent --usb prints the list as usb_text
// has it), so that an ssh project sees the far machine's devices.
// BLIT_USB_ROOT names a folder laid out as /sys/bus/usb/devices is, read in
// place of the machine's own on either system (the tests' stand-in for
// plugged-in hardware).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

// A serial port of a device: its path (/dev/cu.usbmodem14203,
// /dev/ttyACM0) and the interface it is on (its number, and the name the
// device gives that interface, "Black Magic UART Port", if any).
struct UsbPort {
    std::string path;
    int iface = -1;
    std::string name;
};

struct UsbDevice {
    u16 vid = 0, pid = 0;
    u8 cls = 0;  // bDeviceClass (9: a hub)
    std::string manufacturer, product, serial;
    // Where it is plugged in: its sysfs name on Linux ("1-2.3"), its
    // locationID on macOS ("0x14200000").
    std::string location;
    std::vector<std::string> interfaces;  // the names its interfaces give ("CMSIS-DAP v2 Interface"), those that have one
    std::vector<UsbPort> ports;           // by interface
};

// Every device plugged in, by location (hubs included: probes.cpp leaves
// them out). Slow enough to be a worker's call (IOKit and sysfs both read
// many small things).
std::vector<UsbDevice> usb_list();
// Those of a folder laid out as /sys/bus/usb/devices: a device's folder
// (named for where it is, "1-2.3") with idVendor, idProduct, bDeviceClass,
// manufacturer, product and serial; an interface's ("1-2.3:1.0") with
// bInterfaceNumber, interface, and its tty: tty/ttyACM0 (cdc-acm) or
// ttyUSB0 (a usb-serial driver: FTDI, CP210x, CH340).
std::vector<UsbDevice> usb_list_sysfs(const std::string& root);

// The list as text, a line each (blit-agent --usb prints it): a first line
// "blit-usb 1"; a device "d VID PID CLASS LOCATION MANUFACTURER PRODUCT
// SERIAL" (ids in hex), each field after a tab; then its interfaces'
// names, "i NAME", and its ports, "p IFACE PATH NAME". (A tab or a line's
// end in a string becomes a space.)
std::string usb_text(const std::vector<UsbDevice>& devices);
// And back: false if it is not such a list.
bool usb_parse(std::string_view text, std::vector<UsbDevice>* out);

}  // namespace blit
