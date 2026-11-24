// Part of the build's output read for problems (buildscan.hpp): how far an engine that flashes a board or programs an FPGA has got, read as its output comes, the line it is redrawing included.
//
// Each engine's lines are a table of shapes (below): a line that starts so
// (after the spaces probe-rs right-aligns its names with), and has a word
// after that, is a stage beginning, a stage's bar (its percent, and the bytes
// where the tool prints them), a stage done, what was written and how long
// it took, that it was verified, or that it failed (a problem). A line no
// engine starts so costs a look at its first character. The engines:
// OpenOCD's `program` (and Espressif's `program_esp`), probe-rs's download
// bars, SEGGER J-Link Commander's loadfile, esptool's write-flash (v4's and
// v5's), openFPGALoader's loads into SRAM and flash, picotool's load (and
// blit's own copy of a .uf2 to a boot ROM's volume), dfu-util's download,
// pyOCD's load, STM32CubeProgrammer's, Vivado's hardware manager's into
// SRAM and flash, iceprog, ecpprog and Lattice's pgrcmd.
// Where each shape is from (a tool's source at a tag, or its output) is
// said beside it; tests/progress.sh replays the outputs.
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>

#include "buildscan.hpp"

namespace blit {

namespace {

enum Engine : u8 { kAnyEngine, kOpenOcd, kEsptool, kProbeRs, kJLink, kOpenFpgaLoader, kPicotool, kVivado, kUf2, kDfuUtil, kPyOcd, kCubeProg, kIceprog, kEcpprog, kPgrcmd, kEngines };
// Their names, and the program a problem with no file of its own is placed at.
constexpr std::string_view kEngineName[kEngines] = {"",         "OpenOCD",  "esptool", "probe-rs",            "J-Link",  "openFPGALoader", "picotool", "Vivado",
                                                    "UF2 copy", "dfu-util", "pyOCD",   "STM32CubeProgrammer", "iceprog", "ecpprog",        "pgrcmd"};
constexpr std::string_view kProgram[kEngines] = {"",    "openocd",  "esptool", "probe-rs",             "JLinkExe", "openFPGALoader", "picotool", "vivado",
                                                 "uf2", "dfu-util", "pyocd",   "STM32_Programmer_CLI", "iceprog",  "ecpprog",        "pgrcmd"};

enum Kind : u8 {
    kHello,       // the engine names itself: its run begins
    kStage,       // a stage begins (the one before it is done)
    kPart,        // the next part of what is written begins: its stages are under way again (esptool's next file)
    kBar,         // a stage's bar: its percent, and the bytes where given, after the prefix
    kDone,        // the stage named is done
    kBarDone,     // a bar whose line ends "Done." is done (J-Link's)
    kOk,          // the stage under way is done, and what was written matched (picotool's "  OK")
    kWrote,       // "wrote N bytes ... in S s": the bytes written, and how long that took
    kBytes,       // "(N bytes)": the bytes written
    kTook,        // the run's own count of its time: "Finished in 1.80s"
    kVerified,    // what was written was read back and matched
    kUnchanged,   // nothing needed writing: it was already there
    kFlash,       // what is written goes to flash (not SRAM: openFPGALoader's -f)
    kError,       // an error, kept for the failure that follows it (OpenOCD's "Error: ")
    kFailed,      // the run failed (the error kept before it, else this line, is the problem)
    kFailedHere,  // the run failed: what follows the prefix is the problem
    kFailedLine,  // the run failed: the line is the problem
    kFailedThen,  // the run failed: the words matched after the prefix (`then`) are the problem
    kHeld,        // the run failed: the line begins the problem, whose words may go on over the next lines; the lines after it are notes on it
    kWrite,       // the writing begins: Program where the flash is written (kFlash said so before), else Load (SRAM)
    kCount,       // a stage's count, "N/M" after the prefix: its bytes done, and in all
    kSize,        // "N" after the prefix: the bytes to be written
    kProgrammed,  // "..., programmed N bytes" (the number after `then`): the bytes written; none, and nothing needed writing
    kDoneBit,     // a status word in hex after the prefix, read while Load is under way: its DONE bit (8) set, Load is done, else it failed
    kLeft,        // the board has been told to run what was written, and goes: what the tool says after is not the flash's (dfu-util's leave)
};

struct Shape {
    Engine engine;
    Kind kind;
    std::string_view prefix;  // the line starts so
    std::string_view then;    // and has this after it (empty: anything; "$": nothing)
    std::string_view stage;   // the stage, in blit's words
    bool needs = false;       // only while that engine's output is being read (its words are others' too); else the line says whose it is
};

// OpenOCD 0.12.0's `program` (src/flash/startup.tcl, as the Homebrew build
// 0.12.0_1 carries it) prints the ** lines; `flash write_image` the wrote
// line (src/flash/nor/tcl.c, "wrote %u bytes from file %s in %fs (%0.3f
// KiB/s)"), verify_image the verified line (src/target/target.c). Espressif's
// OpenOCD v0.12.0-esp32-20260831's `program_esp` (target/esp_common.cfg
// lines 535-707) says "Programming Finished in N ms" and "Verify OK".
// esptool v4.8.1 (esptool/cmds.py lines 585, 627, 700, 726; util.py
// print_overwrite: "\r" before each redraw on a terminal) and v5.6.0
// (esptool/cmds.py lines 1808, 1865, 1921, 1935, 1958; esp-pylib 1.2.0's
// logger.py progress_bar: "\r\e[2K" before each redraw, then the bar, the
// percent and "4.00kB/22.00kB"). probe-rs 0.32.0
// (probe-rs-tools/src/bin/probe-rs/util/flash.rs lines 101, 130-133,
// 155-190: "{msg:>13} {spinner} {percent:>3}% [{bar:20}] {bytes:>10} @
// {bytes_per_sec:>12} (ETA {eta})", "(took {elapsed})" once done; idle bars
// have no bytes). J-Link Commander V6.14c's loadfile, as posted on SEGGER's
// forum (thread 4031). openFPGALoader v1.2.0 (src/progressBar.cpp: "\r" +
// "<what>: [====  ] 45.00%", then "Done"; src/xilinx.cpp line 1015 "Load
// SRAM", src/spiFlash.cpp lines 224, 498, 573 "Erasing", "Writing",
// "Reading"; its errors as it printed them with no board). picotool 2.3.1
// (main.cpp lines 4877-4910: "<what>[====  ]  62%\r", padded to "Loading
// into XIP RAM: "; lines 5298, 5344, 5371).
constexpr Shape kShapes[] = {
    // OpenOCD
    {kOpenOcd, kHello, "Open On-Chip Debugger ", "", ""},
    {kOpenOcd, kStage, "** Programming Started **", "$", "Program"},
    {kOpenOcd, kDone, "** Programming Finished", "", "Program"},
    {kOpenOcd, kStage, "** Verify Started **", "$", "Verify"},
    {kOpenOcd, kVerified, "** Verified OK **", "$", "Verify"},
    {kOpenOcd, kVerified, "** Verify OK **", "$", "Verify"},
    {kOpenOcd, kUnchanged, "**Verified OK - No flashing**", "$", ""},
    {kOpenOcd, kUnchanged, "** Existing flash content matches **", "$", ""},
    {kOpenOcd, kStage, "** Resetting Target **", "$", "Reset"},
    {kOpenOcd, kFailed, "** Programming Failed **", "$", ""},
    {kOpenOcd, kFailed, "** Verify Failed **", "$", ""},
    {kOpenOcd, kFailed, "** OpenOCD init failed **", "$", ""},
    {kOpenOcd, kFailed, "** Unable to reset target **", "$", ""},
    {kOpenOcd, kFailed, "** ESP flash programming is not supported! **", "$", ""},
    {kOpenOcd, kWrote, "wrote ", " bytes from file ", "Program"},
    {kOpenOcd, kVerified, "verified ", " bytes ", "Verify"},
    {kOpenOcd, kError, "Error: ", "", "", true},
    // esptool
    {kEsptool, kHello, "esptool v", "", ""},
    {kEsptool, kHello, "esptool.py v", "", ""},
    {kEsptool, kStage, "Erasing flash...", "$", "Erase"},
    // (Each file write-flash writes begins so, v4's and v5's: ESP-IDF's
    // bootloader, partition table and app are three. v4.8.1 cmds.py line
    // 528; v5.6.0 as it printed it, writing hello_world on 2026-11-23.)
    {kEsptool, kPart, "Flash will be erased from ", "", "Program"},
    {kEsptool, kBar, "Writing at 0x", "%", "Program"},
    {kEsptool, kWrote, "Wrote ", " bytes ", "Program"},
    {kEsptool, kStage, "Verifying written data...", "$", "Verify"},
    {kEsptool, kVerified, "Hash of data verified.", "$", "Verify"},
    {kEsptool, kFailedHere, "A fatal error occurred: ", "", ""},
    {kEsptool, kFailedHere, "ERROR: A fatal error occurred: ", "", ""},
    // J-Link Commander
    {kJLink, kHello, "SEGGER J-Link Commander V", "", ""},
    {kJLink, kHello, "Downloading file [", "", ""},
    {kJLink, kBarDone, "Comparing flash", "%]", "Compare"},
    {kJLink, kBarDone, "Erasing flash", "%]", "Erase"},
    {kJLink, kBarDone, "Programming flash", "%]", "Program"},
    {kJLink, kBarDone, "Verifying flash", "%]", "Verify"},
    {kJLink, kBytes, "J-Link: Flash download: Flash programming performed for ", " bytes)", ""},
    {kJLink, kTook, "J-Link: Flash download: Total time needed: ", "", ""},
    {kJLink, kFailedLine, "Writing target memory failed.", "$", ""},
    // probe-rs (its names right-aligned: the spaces before them are not looked at)
    {kProbeRs, kBar, "Erasing ", "% [", "Erase"},
    {kProbeRs, kBar, "Programming ", "% [", "Program"},
    {kProbeRs, kBar, "Verifying ", "% [", "Verify"},
    {kProbeRs, kBar, "Reading flash ", "% [", "Read"},
    {kProbeRs, kTook, "Finished in ", "", "", true},
    {kProbeRs, kFailedHere, "Error: ", "", "", true},
    // openFPGALoader
    {kOpenFpgaLoader, kFlash, "write to flash", "$", ""},
    {kOpenFpgaLoader, kBar, "Load SRAM: [", "%", "Load"},
    {kOpenFpgaLoader, kBar, "Erasing: [", "%", "Erase"},
    {kOpenFpgaLoader, kBar, "Writing: [", "%", "Program"},
    {kOpenFpgaLoader, kBar, "Reading: [", "%", "Verify"},
    {kOpenFpgaLoader, kBar, "Write Flash: [", "%", "Program"},
    {kOpenFpgaLoader, kBar, "Verify Flash: [", "%", "Verify"},
    {kOpenFpgaLoader, kBar, "Write PROM: [", "%", "Program"},
    {kOpenFpgaLoader, kStage, "Verifying write", "", "Verify"},
    {kOpenFpgaLoader, kDone, "Done", "$", "", true},
    {kOpenFpgaLoader, kFailedLine, "Failed to read flash", "$", "", true},
    {kOpenFpgaLoader, kFailedLine, "JTAG init failed with: ", "", ""},
    {kOpenFpgaLoader, kFailedLine, "Verification failed at ", "", ""},
    {kOpenFpgaLoader, kFailedLine, "Error parsing options: ", "", "", true},
    {kOpenFpgaLoader, kFailedHere, "Error: ", "", "", true},
    // picotool
    {kPicotool, kBar, "Loading into Flash:", "%", "Program"},
    {kPicotool, kBar, "Loading into RAM:", "%", "Program"},
    {kPicotool, kBar, "Loading into XIP RAM:", "%", "Program"},
    {kPicotool, kBar, "Verifying Flash:", "%", "Verify"},
    {kPicotool, kBar, "Verifying RAM:", "%", "Verify"},
    {kPicotool, kBar, "Verifying XIP RAM:", "%", "Verify"},
    {kPicotool, kBar, "Erasing:", "%", "Erase"},  // (openFPGALoader's "Erasing: [" comes first)
    {kPicotool, kOk, "OK", "$", "", true},
    {kPicotool, kFailedHere, "ERROR: ", "", "", true},
    // (With -f, a board running the Pico SDK's USB serial rebooted into
    // BOOTSEL first: main.cpp lines 10378, 10383, 10385, as it printed them
    // flashing the owner's Pico. No board in BOOTSEL: its words, wrapped at
    // 80 columns on a pipe (lines 10168-10200), then "but:" and each device
    // it saw, its advice indented under it (lines 10270-10320), held as the
    // problem and its notes.)
    {kPicotool, kStage, "Tracking device serial number ", "", "Reboot"},
    {kPicotool, kStage, "The device was asked to reboot into BOOTSEL mode", "", "Reboot"},
    {kPicotool, kStage, "Waiting for device to reboot", "", "Reboot", true},
    {kPicotool, kHeld, "No accessible ", " in BOOTSEL mode w", ""},
    {kPicotool, kHeld, "Despite the reboot attempt, no accessible ", "", ""},
    // blit's copy of a .uf2 to a boot ROM's volume (flash.cpp's kUf2Script):
    // its bytes after each piece, then the board restarting (the volume gone).
    {kUf2, kHello, "UF2 copy of ", " to ", ""},
    {kUf2, kBar, "UF2 copied ", "%", "Program", true},
    {kUf2, kStage, "UF2 waiting for the board to restart", "$", "Restart", true},
    {kUf2, kWrote, "UF2 wrote ", " bytes", "Restart", true},
    {kUf2, kFailedHere, "UF2 copy failed: ", "", "", true},
    // dfu-util 0.11: its banner (src/main.c lines 204-210); its bars,
    // "\r%s\t[%s] %3llu%% %12llu bytes", then "%s done." (src/dfu_file.c
    // lines 156 and 160; "Erase   " and "Download": src/dfuse.c lines 457
    // and 511, src/dfu_load.c line 116); its warnings and errors, warnx's
    // and errx's, "dfu-util: " before them (src/portable.h lines 38-45;
    // main.c lines 434-455, 583, 645; dfu_file.c lines 337-338). It reads
    // nothing back.
    {kDfuUtil, kHello, "dfu-util 0.", "", ""},
    {kDfuUtil, kBar, "Erase ", "%", "Erase", true},
    {kDfuUtil, kBar, "Download ", "%", "Program", true},
    {kDfuUtil, kDone, "Erase    done.", "$", "Erase", true},
    {kDfuUtil, kDone, "Download done.", "$", "Program", true},
    // Its leave (DfuSe's :leave, after the download: dfuse.c lines 627-645):
    // the boot ROM runs what was written at once and is gone from the USB
    // before it answers dfu-util's last get_status, which dfu-util then
    // says is an error, exiting 74 (the STM32H743's, 2026-11-24).
    {kDfuUtil, kLeft, "Submitting leave request...", "$", "", true},
    {kDfuUtil, kHello, "dfu-util: Warning: ", "", "", true},
    {kDfuUtil, kHello, "dfu-util: A valid DFU suffix will be required", "", "", true},
    {kDfuUtil, kFailedHere, "dfu-util: ", "", "", true},
    // pyOCD v0.46.1's load: its log lines, once the time and the module
    // around them are taken off (pyocd/utility/color_log.py line 28:
    // "0001234 I message [module]"): "Erasing...", "Programming..." and
    // the summary (pyocd/flash/loader.py lines 293, 310, 354, 362); its bar
    // on a terminal, "\r[%-50s] %3d%%", the same one for each stage
    // (pyocd/utility/progress.py lines 46-47, 89-92); no probe, with -W
    // (pyocd/core/helpers.py lines 178-180); an error, logged E or C
    // (pyocd/subcommands/load_cmd.py line 107, pyocd/__main__.py lines
    // 171-176). It reads nothing back.
    {kPyOcd, kStage, "I Erasing...", "", "Erase", true},
    {kPyOcd, kStage, "I Programming...", "", "Program", true},
    {kPyOcd, kProgrammed, "I Erased ", ", programmed ", "", true},
    {kPyOcd, kBar, "[", "%", "", true},
    {kPyOcd, kFailedLine, "No connected debug probe", "", "", true},
    {kPyOcd, kFailedHere, "E ", "", "", true},
    {kPyOcd, kFailedHere, "C ", "", "", true},
    // STM32CubeProgrammer's CLI: only the lines others' code reads it by
    // (rusefi 7a0681f9b6 java_console/ecu_shared/src/main/java/com/rusefi/
    // maintenance/DfuFlasher.java line 438, "Download verified
    // successfully" after -v; adafruit/tinyuf2 b3d228897e
    // .claude/skills/hil-contract/SKILL.md, "File download complete" and
    // "Error: Serial number not found"; qMonstatek 3066b985c2
    // src/backend/updater/swd_recovery.cpp, "No debug probe detected"). ST's
    // UM2237 could not be reached: its bar is not read.
    {kCubeProg, kDone, "File download complete", "$", "Program", true},
    {kCubeProg, kVerified, "Download verified successfully", "", "Verify", true},
    {kCubeProg, kFailedHere, "Error: ", "", "", true},
    // iceprog (icestorm v1.1, f31c39c, iceprog/iceprog.c): "flash ID:" (the
    // flash is being written, lines 195-198), "file size: %ld" (971), the
    // erases (311, 321, 343), "programming.." (914 into SRAM, 1002 the
    // flash), "addr 0x%06X %3ld%%\r" (1011), "done." (1017), "reading.."
    // (1040), "VERIFY OK" (1060), "cdone: high" (864: the FPGA configured);
    // a difference read back (1050), no FTDI chip (mpsse.c lines 300-305).
    {kIceprog, kFlash, "flash ID:", "", "", true},
    {kIceprog, kSize, "file size: ", "", "", true},
    {kIceprog, kStage, "erase ", " sector at 0x", "Erase", true},
    {kIceprog, kStage, "bulk erase..", "$", "Erase", true},
    {kIceprog, kWrite, "programming..", "$", "", true},
    {kIceprog, kBar, "addr 0x", "%", "", true},
    {kIceprog, kDone, "done.", "$", "", true},
    {kIceprog, kStage, "reading..", "$", "Verify", true},
    {kIceprog, kVerified, "VERIFY OK", "$", "Verify", true},
    {kIceprog, kDone, "cdone: high", "$", "", true},
    {kIceprog, kFailedLine, "Found difference between flash and file!", "$", "", true},
    {kIceprog, kFailedLine, "Can't find iCE FTDI USB device", "", "", true},
    // ecpprog (1931c3e, ecpprog/ecpprog.c): as iceprog, but with counts,
    // "\r\e[0Kprogramming..  %04u/%04lu" and "verify..       %04u/%04lu"
    // (lines 1245, 1293), "  VERIFY OK" or the difference after the last
    // (1300, 1295); into SRAM, "programming.." (1146), then the ECP5's
    // status register, its DONE bit 8 (lines 501, 509).
    {kEcpprog, kFlash, "flash ID:", "", "", true},
    {kEcpprog, kSize, "file size: ", "", "", true},
    {kEcpprog, kStage, "erase ", " sector at 0x", "Erase", true},
    {kEcpprog, kStage, "bulk erase..", "$", "Erase", true},
    {kEcpprog, kWrite, "programming..", "$", "", true},
    {kEcpprog, kCount, "programming..", "/", "Program", true},
    {kEcpprog, kFailedThen, "verify..", "Found difference between flash and file!", "", true},
    {kEcpprog, kVerified, "verify..", "VERIFY OK", "Verify", true},
    {kEcpprog, kCount, "verify..", "/", "Verify", true},
    {kEcpprog, kDoneBit, "ECP5 Status Register: 0x", "", "", true},
    {kEcpprog, kFailedLine, "Can't find iCE FTDI USB device", "", "", true},
    // pgrcmd: its return code in words, as blit prints it after it
    // (flash.cpp's kPgrcmdCodes).
    {kPgrcmd, kFailedHere, "pgrcmd: ", "", "", true},
    // Vivado 2026.2's hardware manager in batch mode, as it printed it on
    // the lab box loading the owner's Basys 3 (2026-11-23): no percent, a
    // line as each step begins, the DONE pin's state after the load; a
    // failure "ERROR: [Labtools 27-2149] File top.bit not found. ...", whose
    // "[...]" is left out of the problem.
    {kVivado, kHello, "****** Vivado v", "", ""},
    {kVivado, kStage, "INFO: [Labtools 27-2285] Connecting to hw_server", "", "Connect"},
    {kVivado, kStage, "INFO: [Labtoolstcl 44-466] Opening hw_target ", "", "Load"},
    {kVivado, kDone, "INFO: [Labtools 27-3164] End of startup status: HIGH", "$", "Load"},
    {kVivado, kFailedLine, "INFO: [Labtools 27-3164] End of startup status: LOW", "$", ""},
    {kVivado, kFailedHere, "ERROR: ", "", "", true},
    // Its program_hw_cfgmem writing the FPGA's flash (the board's SRAM
    // loaded with Vivado's design for it first: the Load above), and
    // boot_hw_device after, as it printed them (same box and board): the
    // stages, the time it gives itself ("elapsed = 00:00:54"); what it
    // prints when only verifying, as when what is there differs.
    {kVivado, kStage, "Performing Erase Operation...", "$", "Erase", true},
    {kVivado, kDone, "Erase Operation successful.", "$", "Erase", true},
    {kVivado, kStage, "Performing Program and Verify Operations...", "$", "Program", true},
    {kVivado, kVerified, "Program/Verify Operation successful.", "$", "Program", true},
    {kVivado, kStage, "Performing Verify Operation...", "$", "Verify", true},
    {kVivado, kTook, "program_hw_cfgmem: Time (s): ", "elapsed = ", "", true},
    {kVivado, kStage, "INFO: [Labtoolstcl 44-664] Will wait up to ", "", "Boot", true},
    {kVivado, kDone, "INFO: [Labtools 27-32] Done pin status: HIGH", "$", "Boot", true},
};

// The first characters the shapes start with: any other line is none of them.
constexpr std::array<bool, 256> kFirst = [] {
    std::array<bool, 256> a{};
    for (const Shape& s : kShapes) a[static_cast<u8>(s.prefix[0])] = true;
    return a;
}();

// A program named at the start of a command (make echoing it, or the task's
// own): whose output follows. (A path before it, or a Python running it.)
struct Named {
    Engine engine;
    std::string_view program;
};
constexpr Named kNamed[] = {
    {kOpenOcd, "openocd"}, {kOpenOcd, "openocd-esp32"}, {kEsptool, "esptool"}, {kEsptool, "esptool.py"}, {kProbeRs, "probe-rs"}, {kProbeRs, "cargo-flash"},
    {kJLink, "JLinkExe"}, {kJLink, "JLink.exe"}, {kOpenFpgaLoader, "openFPGALoader"}, {kPicotool, "picotool"}, {kVivado, "vivado"},
    {kDfuUtil, "dfu-util"}, {kPyOcd, "pyocd"}, {kCubeProg, "STM32_Programmer_CLI"}, {kIceprog, "iceprog"}, {kEcpprog, "ecpprog"}, {kPgrcmd, "pgrcmd"},
};

// The first characters of a command make echoes that could be one of them:
// a path, a Python running it, or its name.
constexpr std::array<bool, 256> kEchoFirst = [] {
    std::array<bool, 256> a{};
    a['/'] = a['.'] = a['p'] = true;
    for (const Named& n : kNamed) a[static_cast<u8>(n.program[0])] = true;
    return a;
}();

bool digit(char c) { return c >= '0' && c <= '9'; }

// A number at s[*i] ("45", "45.00", "1.234567"), read on.
bool decimal_at(std::string_view s, size_t* i, double* out) {
    size_t k = *i;
    double v = 0, scale = 0;
    bool any = false;
    for (; k < s.size(); ++k) {
        if (digit(s[k])) {
            any = true;
            if (scale > 0) v += (s[k] - '0') * (scale /= 10);
            else v = v * 10 + (s[k] - '0');
        } else if (s[k] == '.' && scale == 0 && k + 1 < s.size() && digit(s[k + 1])) {
            scale = 1;
        } else {
            break;
        }
    }
    if (!any) return false;
    *i = k;
    *out = v;
    return true;
}

// A size at s[*i]: "4.00kB", "25.40 KiB", "120 B", read on (1024 to a kB, as
// these tools count).
bool size_at(std::string_view s, size_t* i, u64* out) {
    size_t k = *i;
    double v = 0;
    if (!decimal_at(s, &k, &v)) return false;
    if (k < s.size() && s[k] == ' ') ++k;
    static constexpr std::pair<std::string_view, double> kUnits[] = {{"KiB", 1024.0}, {"MiB", 1048576.0}, {"GiB", 1073741824.0}, {"kB", 1024.0}, {"KB", 1024.0},
                                                                     {"MB", 1048576.0}, {"GB", 1073741824.0}, {"bytes", 1.0}, {"B", 1.0}};
    for (const auto& [unit, scale] : kUnits)
        if (s.substr(k).starts_with(unit)) {
            const size_t e = k + unit.size();
            if (e < s.size() && s[e] != ' ' && s[e] != '/' && s[e] != ')' && s[e] != ',') return false;  // (not the whole of a word)
            *i = e;
            *out = static_cast<u64>(v * scale + 0.5);
            return true;
        }
    return false;
}

// A percent in s from `from`: the number before the first '%' ("45.00%",
// "(5 %)", "[100%]", " 62%"). -1: none.
double percent_in(std::string_view s, size_t from, size_t* after) {
    const size_t pct = s.find('%', from);
    if (pct == std::string_view::npos) return -1;
    size_t e = pct;
    if (e > from && s[e - 1] == ' ') --e;  // (esptool v4's "(5 %)")
    size_t b = e;
    while (b > from && (digit(s[b - 1]) || s[b - 1] == '.')) --b;
    size_t k = b;
    double v = 0;
    if (b == e || !decimal_at(s, &k, &v) || k != e) return -1;
    *after = pct + 1;
    return std::min(v, 100.0);
}

// Seconds after " in " (or at the start of s): "1.234567s", "0.4 seconds", "1.80s", "3.940s". -1: none.
double seconds_in(std::string_view s) {
    size_t at = s.find(" in ");
    at = at == std::string_view::npos ? 0 : at + 4;
    double v = 0;
    if (!decimal_at(s, &at, &v)) return -1;
    if (at < s.size() && s[at] == ' ') ++at;
    if (s.substr(at).starts_with("ms")) return v / 1000;  // (Espressif's OpenOCD: "Programming Finished in 2305 ms")
    return at < s.size() && s[at] == 's' ? v : -1;
}

// Bytes as the button shows them: done and in all in the unit of all
// ("25.4/41.2 KB"); done alone when all is not known.
std::string size_words(u64 done, u64 total) {
    const u64 big = std::max(done, total);
    const double unit = big >= 1024 * 1024 ? 1048576.0 : big >= 1024 ? 1024.0 : 1.0;
    const char* name = unit > 1024 ? " MB" : unit > 1 ? " KB" : " B";
    auto fig = [&](u64 n) {
        char buf[32];
        if (unit == 1) std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(n));
        else std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(n) / unit);
        return std::string(buf);
    };
    return total ? fig(done) + "/" + fig(total) + name : fig(done) + name;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// A pyOCD log line's message: "0001234 I Erasing... [loader]" is
// "I Erasing..." (pyocd/utility/color_log.py line 28).
std::string_view pyocd_message(std::string_view s) {
    if (s.size() > 10 && s[7] == ' ' && std::all_of(s.begin(), s.begin() + 7, [](char c) { return c >= '0' && c <= '9'; })) {
        s.remove_prefix(8);
        if (s.ends_with(']'))
            if (const size_t open = s.rfind(" ["); open != std::string_view::npos) s = s.substr(0, open);
    }
    return s;
}

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

// ---------------------------------------------------------------- the words

std::string BuildScan::progress_words(const Progress& p, bool stages) {
    std::string out;
    if (stages)
        for (const std::string& s : p.done) out += (out.empty() ? "" : " \xC2\xB7 ") + s + " \xE2\x9C\x93";  // "Erase ✓ · "
    if (!p.stage.empty()) {
        std::string now = p.stage;
        if (p.percent >= 0) now += " " + std::to_string(p.percent) + "%";
        if (p.bytes || p.total) now += " " + size_words(p.bytes, p.total);
        out += (out.empty() ? "" : " \xC2\xB7 ") + now;
    }
    return out;
}

// ---------------------------------------------------------------- the lines

void BuildScan::use_command(std::string_view command) {
    // (The words of a shell's command: the first, or one after "exec", "&&"
    // or a Python, is the program.)
    size_t at = 0;
    while (at < command.size()) {
        while (at < command.size() && (command[at] == ' ' || command[at] == ';' || command[at] == '&' || command[at] == '|')) ++at;
        size_t e = at;
        while (e < command.size() && command[e] != ' ' && command[e] != ';' && command[e] != '&' && command[e] != '|') ++e;
        std::string_view word = command.substr(at, e - at);
        if (const size_t slash = word.rfind('/'); slash != std::string_view::npos) word = word.substr(slash + 1);
        while (!word.empty() && (word.front() == '\'' || word.front() == '"')) word.remove_prefix(1);
        while (!word.empty() && (word.back() == '\'' || word.back() == '"')) word.remove_suffix(1);
        for (const Named& n : kNamed)
            if (word == n.program) return progress_engine(n.engine);
        at = e;
    }
}

bool BuildScan::wants_screen() const { return engine_ == kProbeRs; }

// The terminal's rows as it shows them, each read for a bar as a line
// being redrawn: probe-rs's idle bars (no bytes yet) pass, the one that
// took its time is done, the one under way is the stage.
void BuildScan::screen(const std::vector<std::string>& rows) {
    screen_row_ = true;
    for (const std::string& r : rows)
        if (!r.empty()) progress_line(r, true);
    screen_row_ = false;
}

bool BuildScan::progress_line(std::string_view text, bool unfinished) {
    // (After a failure whose words go on, the rest is its: held.)
    if (prog_holding_) {
        if (!unfinished) progress_hold(text);
        return true;
    }
    if (engine_ == kPyOcd) text = pyocd_message(text);
    // (Spaces first: probe-rs right-aligns its names.)
    size_t lead = 0;
    while (lead < text.size() && text[lead] == ' ') ++lead;
    if (lead == text.size() || !kFirst[static_cast<u8>(text[lead])]) {
        // make echoing a command: an engine's, whose output follows.
        if (!unfinished && lead == 0 && !text.empty() && kEchoFirst[static_cast<u8>(text[0])]) {
            const size_t sp = text.find(' ');
            std::string_view word = text.substr(0, sp);
            if (const size_t slash = word.rfind('/'); slash != std::string_view::npos) word = word.substr(slash + 1);
            if (word.starts_with("python")) {
                const std::string_view rest = sp == std::string_view::npos ? std::string_view{} : trim(text.substr(sp));
                word = rest.substr(0, rest.find(' '));
                if (word == "-m") word = trim(rest.substr(2)).substr(0, trim(rest.substr(2)).find(' '));
                if (const size_t slash = word.rfind('/'); slash != std::string_view::npos) word = word.substr(slash + 1);
            }
            for (const Named& n : kNamed)
                if (word == n.program) progress_engine(n.engine);
        }
        return false;
    }
    const std::string_view line = trim(text);
    // The engine's own shapes first (picotool's "Erasing:" is openFPGALoader's
    // too), then any other's that says whose it is.
    const Shape* shape = nullptr;
    for (int pass = 0; pass < 2 && !shape; ++pass)
        for (const Shape& s : kShapes) {
            if ((s.engine == engine_) != (pass == 0) || (pass == 1 && s.needs) || !line.starts_with(s.prefix)) continue;
            if (s.then == "$") {
                if (line.size() != s.prefix.size()) continue;
            } else if (!s.then.empty() && line.find(s.then, s.prefix.size()) == std::string_view::npos) {
                continue;
            }
            shape = &s;
            break;
        }
    if (!shape) return false;
    size_t after = shape->prefix.size();
    // (A line still being written is read only for its bar: the rest wait for its end.)
    // (A row the terminal shows, of an engine drawing in place: its bars,
    // and its own count of its time, the row "Finished in 2.07s" that ends
    // it, which comes as no line of its own.)
    if (unfinished && shape->kind != kBar && shape->kind != kBarDone && shape->kind != kCount && !(screen_row_ && shape->kind == kTook)) return false;
    progress_engine(shape->engine);
    if (progress_.engine.empty()) progress_.engine = std::string(kEngineName[engine_]);
    if (prog_start_ == 0) prog_start_ = now_s();
    if (progress_.failed && shape->kind != kError) return true;  // (after a failure, what it goes on to say is not progress)
    // A stage under way: the one before it is done.
    auto begin = [&](std::string_view stage) {
        if (progress_.stage == stage) return;
        if (std::ranges::find(progress_.done, stage) != progress_.done.end()) return;  // (a redrawn line of one done: probe-rs's)
        if (!progress_.stage.empty() && std::ranges::find(progress_.done, progress_.stage) == progress_.done.end()) progress_.done.push_back(progress_.stage);
        progress_.stage = std::string(stage);
        progress_.percent = -1;
        progress_.bytes = progress_.total = 0;
    };
    auto done = [&](std::string_view named) {
        const std::string stage(named.empty() ? std::string_view(progress_.stage) : named);  // (a copy: the one under way is cleared)
        if (stage.empty()) return;
        if (std::ranges::find(progress_.done, stage) == progress_.done.end()) progress_.done.push_back(stage);
        if (progress_.stage == stage) progress_.stage.clear(), progress_.percent = -1;
        if (stage == "Verify") prog_verified_ = true;  // (a verifying stage that ended well)
    };
    auto fail = [&](std::string_view message) {
        if (progress_.left) return;  // (the board gone as asked: kLeft)
        // ("** OpenOCD init failed **" after "Error: open failed": "OpenOCD init failed: open failed".)
        progress_.failed = true;
        message = trim(message);
        if (message.starts_with("** ") && message.ends_with(" **")) message = message.substr(3, message.size() - 6);
        const std::string m = prog_error_.empty() ? std::string(message) : std::string(message) + ": " + prog_error_;
        if (!m.empty()) add(kProgram[engine_], 1, 0, 1, m);
    };
    switch (shape->kind) {
        case kHello: break;
        case kLeft:  // (after what was written was all written, and only then)
            if (!progress_.failed && std::ranges::find(progress_.done, std::string_view("Program")) != progress_.done.end()) progress_.left = true;
            break;
        case kStage: begin(shape->stage); break;
        case kPart:
            std::erase(progress_.done, std::string(shape->stage));
            std::erase(progress_.done, std::string("Verify"));
            begin(shape->stage);
            break;
        case kBar:
        case kBarDone: {
            double pct = percent_in(line, after, &after);
            const bool took = line.find("(took ", after) != std::string_view::npos;
            // probe-rs's idle bar (no bytes yet) is not under way; one that
            // took its time is done.
            if (engine_ == kProbeRs && !took && line.find(" @ ", after) == std::string_view::npos) break;
            // (A bar of no stage of its own is the one under way's: pyOCD's, iceprog's.)
            const std::string stage(shape->stage.empty() ? std::string_view(progress_.stage) : shape->stage);
            if (stage.empty() || std::ranges::find(progress_.done, stage) != progress_.done.end()) break;
            begin(stage);
            if (pct >= 0) progress_.percent = static_cast<i32>(pct);
            // Bytes after it: "4.00kB/22.00kB" (esptool v5), or past the bar, "25.40 KiB @" (probe-rs).
            size_t k = after;
            while (k < line.size() && line[k] == ' ') ++k;
            if (k < line.size() && line[k] == '[') {
                const size_t close = line.find(']', k);
                k = close == std::string_view::npos ? line.size() : close + 1;
                while (k < line.size() && line[k] == ' ') ++k;
            }
            u64 a = 0, b = 0;
            if (size_at(line, &k, &a)) {
                progress_.bytes = a;
                if (k < line.size() && line[k] == '/' && (++k, size_at(line, &k, &b))) progress_.total = b;
                if (took) progress_.total = a;
            }
            if (took || (shape->kind == kBarDone && line.ends_with("Done."))) {
                if (took && progress_.total && stage == "Program") prog_bytes_ = progress_.total;  // (the bytes programmed: not the sectors erased)
                done(stage);
            }
            break;
        }
        case kDone:
            if (const double t = seconds_in(line); t >= 0) prog_seconds_ = std::max(prog_seconds_, 0.0) + t;  // (the tool's own time for it)
            // (What its bar came to, where it counted bytes: dfu-util's Download.)
            if (progress_.bytes && !prog_bytes_ && (shape->stage.empty() ? progress_.stage : std::string(shape->stage)) == "Program") prog_bytes_ = progress_.bytes;
            done(shape->stage);
            break;
        case kOk:
            prog_verified_ = true;
            done({});
            break;
        case kWrote: {
            // "wrote 41200 bytes from file app.bin in 1.234567s", "Wrote 41200 bytes (22528 compressed) at 0x00010000 in 0.4 seconds"
            size_t k = after;
            double n = 0;
            if (decimal_at(line, &k, &n)) prog_bytes_ = (prog_wrote_ ? prog_bytes_ : 0) + static_cast<u64>(n);
            prog_wrote_ = true;
            if (const double s = seconds_in(line.substr(k)); s >= 0) prog_seconds_ = std::max(prog_seconds_, 0.0) + s;
            if (engine_ == kEsptool || engine_ == kUf2) done(shape->stage);
            break;
        }
        case kBytes: {
            const size_t open = line.rfind('(');
            size_t k = open == std::string_view::npos ? line.size() : open + 1;
            double n = 0;
            if (decimal_at(line, &k, &n)) prog_bytes_ = static_cast<u64>(n);
            break;
        }
        case kTook: {
            size_t k = after;
            double s = 0;
            if (const size_t e = line.find("elapsed = ", after); e != std::string_view::npos) {  // (Vivado's "elapsed = 00:00:54": hours, minutes, seconds)
                k = e + 10;
                double h = 0, m = 0;
                if (decimal_at(line, &k, &h) && k < line.size() && line[k] == ':' && decimal_at(line, &++k, &m) && k < line.size() && line[k] == ':' &&
                    decimal_at(line, &++k, &s))
                    prog_took_ = h * 3600 + m * 60 + s;
            } else if (decimal_at(line, &k, &s)) {
                prog_took_ = s;
            }
            break;
        }
        case kVerified:
            prog_verified_ = true;
            done(shape->stage);
            break;
        case kUnchanged:  // (what was to be written is there already: its Program wrote nothing)
            prog_unchanged_ = prog_verified_ = true;
            if (progress_.stage == "Program") progress_.stage.clear(), progress_.percent = -1;
            break;
        case kFlash: prog_flash_ = true; break;
        case kError: prog_error_ = std::string(trim(line.substr(after))); return false;  // (others may read it too)
        case kFailed: fail(line); break;
        case kFailedHere: {
            std::string_view m = line.substr(after);
            if (engine_ == kVivado && m.starts_with('['))  // ("[Labtools 27-2149] File top.bit not found.": the message alone)
                if (const size_t close = m.find("] "); close != std::string_view::npos) m.remove_prefix(close + 2);
            fail(m);
            break;
        }
        case kFailedLine: fail(line); break;
        case kFailedThen: fail(shape->then); break;
        case kHeld:
            progress_.failed = prog_holding_ = true;
            prog_held_ = std::string(line);
            prog_notes_.clear();
            break;
        case kWrite: begin(prog_flash_ ? "Program" : "Load"); break;
        case kCount: {
            // "programming..  0256/1234": the bytes done of all of them.
            size_t k = after;
            while (k < line.size() && line[k] == ' ') ++k;
            double a = 0, b = 0;
            if (!decimal_at(line, &k, &a) || k >= line.size() || line[k] != '/' || (++k, !decimal_at(line, &k, &b)) || b <= 0) break;
            if (std::ranges::find(progress_.done, shape->stage) != progress_.done.end()) break;
            begin(shape->stage);
            progress_.bytes = static_cast<u64>(a);
            progress_.total = static_cast<u64>(b);
            progress_.percent = static_cast<i32>(std::min(100.0, a * 100 / b));
            break;
        }
        case kSize: {
            size_t k = after;
            double n = 0;
            if (decimal_at(line, &k, &n)) prog_bytes_ = static_cast<u64>(n);
            break;
        }
        case kProgrammed: {
            // "Erased 16384 bytes (4 sectors), programmed 16384 bytes (16 pages), identical 0 bytes (0 pages) at 9.80 kB/s"
            size_t k = line.find(shape->then, after) + shape->then.size();
            double n = 0;
            if (!decimal_at(line, &k, &n)) break;
            prog_bytes_ = static_cast<u64>(n);
            prog_wrote_ = true;
            if (prog_bytes_ == 0) {  // (all of it there already)
                prog_unchanged_ = true;
                std::erase(progress_.done, std::string("Program"));
                if (progress_.stage == "Program") progress_.stage.clear(), progress_.percent = -1;
            }
            break;
        }
        case kDoneBit: {
            if (progress_.stage != "Load") break;
            size_t k = after;
            u64 v = 0;
            for (; k < line.size() && std::isxdigit(static_cast<unsigned char>(line[k])); ++k) v = v * 16 + static_cast<u64>(std::isdigit(static_cast<unsigned char>(line[k])) ? line[k] - '0' : (line[k] | 0x20) - 'a' + 10);
            if (v & 0x100) done("Load");
            else fail(std::string(line) + " (DONE is not set)");
            break;
        }
    }
    progress_changed();
    return true;
}

// The words of a failure that go on over the lines after it, wrapped
// (they end with a full stop), then its notes, each a line not indented
// and the indented lines under it.
void BuildScan::progress_hold(std::string_view text) {
    const std::string_view t = trim(text);
    if (t.empty() || t == "but:") return;
    if (!prog_held_.ends_with('.')) {
        prog_held_ += ' ';
        prog_held_ += t;
    } else if (text.starts_with(' ') && !prog_notes_.empty()) {
        prog_notes_.back() += ' ';
        prog_notes_.back() += t;
    } else {
        prog_notes_.emplace_back(t);
    }
}

// The failure held, as a problem with its notes.
void BuildScan::progress_release() {
    if (!prog_holding_) return;
    prog_holding_ = false;
    if (BuildProblem* p = add(kProgram[engine_], 1, 0, 1, prog_held_))
        for (std::string& n : prog_notes_) p->notes.push_back(BuildNote{{}, 0, 0, std::move(n)});
    prog_held_.clear();
    prog_notes_.clear();
}

// Whose output is being read: another engine's run, what one before it said is over.
void BuildScan::progress_engine(u8 engine) {
    if (engine == engine_) return;
    progress_release();
    engine_ = engine;
    const bool had = !progress_.engine.empty();
    progress_ = Progress{};
    prog_error_.clear();
    prog_verified_ = prog_unchanged_ = prog_flash_ = prog_wrote_ = false;
    prog_seconds_ = prog_took_ = -1;
    prog_start_ = 0;
    prog_bytes_ = 0;
    if (had) progress_changed();
}

// The output has ended: what the run came to.
void BuildScan::progress_end() {
    progress_release();
    if (progress_.engine.empty()) return;
    std::string& s = progress_.summary;
    s.clear();
    if (progress_.failed) {
        if (!progress_.stage.empty()) s = "stopped in " + progress_.stage + (progress_.percent >= 0 ? " at " + std::to_string(progress_.percent) + "%" : "");
    } else {
        progress_.cut = progress_.stage;
        progress_.cut_percent = progress_.percent;
        if (!progress_.stage.empty() && std::ranges::find(progress_.done, progress_.stage) == progress_.done.end()) progress_.done.push_back(progress_.stage);
        progress_.stage.clear();
        progress_.percent = -1;
        const bool wrote = prog_bytes_ > 0 || std::ranges::find(progress_.done, std::string_view("Program")) != progress_.done.end();
        const bool loaded = std::ranges::find(progress_.done, std::string_view("Load")) != progress_.done.end();
        if (prog_unchanged_ && !wrote) s = "Unchanged";
        else if (wrote || (loaded && prog_flash_)) s = "Flashed";
        else if (loaded) s = "Loaded into SRAM";
        if (!s.empty()) {
            if (prog_bytes_) {
                char buf[32];
                if (prog_bytes_ < 1024) std::snprintf(buf, sizeof buf, " %llu B", static_cast<unsigned long long>(prog_bytes_));
                else std::snprintf(buf, sizeof buf, " %.1f", static_cast<double>(prog_bytes_) / (prog_bytes_ >= 1048576 ? 1048576.0 : 1024.0));
                std::string fig = buf;
                if (fig.ends_with(".0")) fig.resize(fig.size() - 2);  // ("128 KB", as the sizes' words have it)
                s += prog_bytes_ < 1024 ? fig : fig + (prog_bytes_ >= 1048576 ? " MB" : " KB");
            }
            // The tool's own count of the run, else of the writing, else the time its lines came over.
            const double t = prog_took_ >= 0 ? prog_took_ : prog_seconds_ >= 0 ? prog_seconds_ : prog_start_ > 0 ? now_s() - prog_start_ : -1;
            if (t >= 0 && (wrote || !prog_unchanged_)) {  // (some parts already there, another written: that one's time)
                char buf[32];
                std::snprintf(buf, sizeof buf, " in %.1f s", t);
                s += buf;
            }
            if (prog_verified_) s += ", verified";
        }
    }
    progress_changed();
}

void BuildScan::progress_changed() {
    // (Told again only when what is shown changes: the stage, its whole
    // percent, the stages done, a failure, the summary.)
    u64 key = std::hash<std::string>{}(progress_.stage) ^ (static_cast<u64>(progress_.percent + 2) << 1) ^ (progress_.done.size() << 9) ^ (progress_.failed ? 1ull << 15 : 0) ^
              (std::hash<std::string>{}(progress_.summary) << 3) ^ (progress_.total << 20);
    if (key == prog_key_ && progress_gen_) return;
    prog_key_ = key;
    ++progress_gen_;
}

}  // namespace blit
