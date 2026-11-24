#pragma once
// Crash dumps in a program's output, decoded in place (the embedded
// design's E7): what a board prints when its firmware crashes, read as it
// comes, and each address in it told as the function and line of source it
// is (addrsrc.hpp, against the firmware's ELF), in lines put into the
// terminal under the line that gave it, as `idf.py monitor` prints them, so
// that a transcript and a saved log keep them. The innermost frame of the
// program's own code is a problem ("Crash at motor.c:212 in pid_step").
//
// The dumps read (their text from their sources, cited in crashscan.cpp):
// - ESP-IDF's panic, on Xtensa (ESP32, -S2, -S3) and RISC-V (-C3, -C6,
//   -H2, -P4), and Arduino-ESP32's, which is ESP-IDF's: "Guru Meditation
//   Error: Core  0 panic'ed (LoadProhibited)", each PC of its "Backtrace:
//   0x400d1234:0x3ffb1230 ..." (and of any backtrace it prints, a task
//   watchdog's), the register dump's MEPC and RA on RISC-V, and abort()'s
//   "abort() was called at PC 0x...";
// - the program's own format: Lua patterns (a run configuration's "crash"
//   in .blit/run.json), a line one takes being a dump's, the hexadecimal
//   numbers in its first capture (the whole match, with none) its
//   addresses, innermost first; lines one after another that the patterns
//   take are one dump.
//
// It runs on the thread that reads the program's output, before the bytes
// go onto the screen (appterm.cpp: a task's and a run's output; a serial
// port's and RTT's tabs call it the same way); nothing here knows about
// windows. The ELF is asked for when a dump first needs it, and an address
// is decoded as the dump is read (a unit's tables the first time: a few
// milliseconds).
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "addrsrc.hpp"
#include "buildscan.hpp"
#include "common.hpp"

namespace blit {

struct CrashOptions {
    // The project's folder (a path here, or "ssh://host/path" there): a
    // frame in a file under it is the program's own, unless it is in its
    // build's output or in components it fetched (build, managed_components,
    // .pio, _deps).
    std::string root;
    // The ELF to decode with, asked for on the scanning thread the first
    // time a dump needs one: null, and why, when there is none (the scan
    // says why once, under the first dump).
    std::function<std::shared_ptr<const AddrSource>(std::string* why)> elf;
    // The program's own panic formats: Lua patterns (as lua_find takes them).
    std::vector<std::string> patterns;
    // The program decodes its dumps itself (`idf.py monitor`): no lines are
    // added, the problems still are.
    bool quiet = false;
};

class CrashScan {
public:
    explicit CrashScan(CrashOptions o);

    // What to show under a dump's line: text for the terminal (lines, each
    // dim, ending "\r\n"), to go in after byte `at` of the bytes just fed
    // (the end of the line it decodes). The caller writes the bytes up to
    // `at`, then the text, then the rest (and so for each, in order).
    struct Insert {
        size_t at = 0;
        std::string text;
    };
    // The program's output as it comes (a terminal's bytes, escapes and all).
    void feed(std::string_view bytes, std::vector<Insert>* out);
    // Or a line at a time, as a serial tab reads them (serialtab.hpp): a
    // whole line without its line end (its escapes left out here, as feed
    // leaves them out); the lines to show under it are added to `add` as
    // plain text, for the caller to dim (feed's Inserts carry their own
    // escapes, as a terminal's bytes must).
    void take_line(std::string_view raw, std::vector<std::string>* add);
    // The end of the output: a dump still open is done (its problem added).
    void finish();
    // The problems found since this was last emptied (the caller takes
    // them): one for each crash; the same crash again (a board that crashes
    // at each start) is the one problem again with its count one more.
    std::vector<BuildProblem>& found() { return found_; }
    // Whether any dump has been seen (for the tests).
    u32 dumps() const { return dumps_; }

    // A pattern the scan can take; false, and why, if not.
    static bool pattern_ok(std::string_view pattern, std::string* why);
    // Whether a command decodes its own dumps (`idf.py monitor`,
    // esp-idf-monitor), for CrashOptions::quiet.
    static bool decodes_itself(std::string_view command);

    static constexpr size_t kMaxLine = 4096;   // bytes of a line looked at
    static constexpr u32 kMaxAddresses = 64;   // addresses of one line decoded, at most
    static constexpr u32 kMaxDumpLines = 200;  // lines a dump goes on for, at most

private:
    // The bytes from p onto the line being put together (escapes left out,
    // a CR going back to its start), up to an LF (returned: where it is) or e.
    const char* gather(const char* p, const char* e);
    // A whole line looked at: what to show under it, a line each, plain.
    void line(std::string_view text, std::vector<std::string>* add);
    // Each address's frames, as lines under the dump's line (`label`: the
    // register's name, if any); the frames kept for its problem if `keep`.
    void decode(const std::vector<u64>& addrs, std::string_view label, std::vector<std::string>* add, bool keep);
    void begin(u8 kind, std::string reason);
    void end();
    bool own(const std::string& path) const;
    const AddrSource* source();

    CrashOptions o_;
    std::shared_ptr<const AddrSource> src_;
    bool asked_ = false, said_ = false;  // (the ELF asked for; why there is none, said)
    std::string why_;

    // The bytes: a line being put together, and where in an escape sequence they are.
    enum : u8 { kText, kEsc, kEscMore, kCsi, kString, kStringEsc } esc_ = kText;
    bool cr_ = false;
    std::string line_;
    std::vector<std::string> adding_;  // (feed's: a line's, before they are made a terminal's text)

    // The dump being read: whose, why it stopped (the exception), its
    // frames so far (the backtrace's, once it has one), and how far it has gone.
    enum : u8 { kNone, kEsp, kOwn };
    u8 dump_ = kNone;
    std::string reason_;
    std::vector<SourceFrame> frames_;
    bool backtrace_ = false;  // (its backtrace has come: a blank line after it ends it)
    u32 dump_lines_ = 0;
    u32 dumps_ = 0;
    std::vector<BuildProblem> found_;
    std::unordered_map<std::string, u32> seen_;  // (each problem's times so far, by what it says)
};

}  // namespace blit
