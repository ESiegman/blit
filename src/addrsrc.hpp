#pragma once
// Address to source (the embedded design's F7, evaluation 6.22): what
// function and line of source an address in an ELF is, read natively from
// its DWARF (versions 2 to 5): the line tables (.debug_line, with DWARF 5's
// .debug_line_str), and the functions and the functions inlined into them
// (.debug_info, .debug_abbrev, .debug_str, .debug_str_offsets, .debug_addr,
// .debug_ranges, .debug_rnglists), as `addr2line -fiC` gives them. With no
// debug information at an address, its function is the symbol table's.
// Split DWARF (.dwo files) and type units are not read.
//
// A unit's lines and functions are read the first time an address in it
// is asked about, so that an ELF of a few hundred megabytes (a host
// program's) costs its units' first entries to open and one unit's tables
// for each new place asked about. As the ELF reader, every read is checked
// against the section's bytes (a damaged unit gives no answer, never a
// read past its end), and nothing here knows of windows or the app, so
// that blit's agent can carry it beside the ELF on another machine.
// lookup() may be called from any number of threads at once.
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "elf.hpp"

namespace blit {

// A frame of an address: its function and where in the source it is. Of
// an address in code that was inlined, the inlined function's frame comes
// first (inlined set), then the function it was inlined into, at the line
// of the call, and so out to the function that holds the code.
struct SourceFrame {
    std::string function;  // demangled; "" when not known
    std::string path;      // as the debug information has it (a full path, where it gives one); "" when not known
    u32 line = 0, col = 0; // from 1; 0 when not known
    bool inlined = false;  // this function's code was inlined into the next frame's
    bool unnamed_file = false;  // no path, though the symbol table gives the code a file (one with no name: ElfSymbol)
};

class AddrSource {
public:
    explicit AddrSource(std::shared_ptr<const Elf> elf);
    ~AddrSource();
    AddrSource(const AddrSource&) = delete;
    AddrSource& operator=(const AddrSource&) = delete;

    // The frames at `addr`, innermost first; none when the ELF knows of no
    // function there. (An ARM ELF's Thumb bit is taken off first.)
    std::vector<SourceFrame> lookup(u64 addr) const;

    const Elf& elf() const { return *elf_; }
    const std::shared_ptr<const Elf>& elf_ptr() const { return elf_; }
    // Whether it has DWARF to read (else names come from its symbols alone).
    bool has_debug() const;
    // The compilation units found (for the tests and the benchmarks).
    size_t units() const;
    // Every unit's tables read now (the benchmark's measure of the whole).
    void read_all() const;

    struct Impl;

private:
    std::shared_ptr<const Elf> elf_;
    std::unique_ptr<Impl> impl_;
};

// A C++ (Itanium ABI) name demangled; the name unchanged when it is not one.
std::string demangled(std::string_view name);

// A frame as `addr2line -pfiC` prints it: "pid_step at /p/motor.c:212",
// " (inlined by) motor_run at /p/motor.c:220" for the frames after an
// inlined one, "?? at ??:0"-style for what is not known.
std::string frame_words(const SourceFrame& f, bool inlined_by);

}  // namespace blit
