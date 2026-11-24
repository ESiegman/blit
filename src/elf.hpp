#pragma once
// ELF files, read natively (the embedded design's F5, evaluation 6.22):
// ELF32 and ELF64 in either byte order, their header, sections, segments,
// symbols and build id, and a named section's bytes (unpacked, when the
// linker compressed it with zlib). Every offset and count in the file is
// checked against its bytes before it is used, and nothing is allocated for
// a size the file gives without a limit, so that a file cut short or made
// wrong (a build still writing it, a stray .elf) is read as far as it is
// right, or said to be no ELF; never read past.
//
// Nothing here knows of windows or of the app (the standard library, zlib
// and common.hpp only), so that blit's agent can carry it, beside the ELF
// on another machine. A read file is const: any number of threads may ask
// it things at once.
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common.hpp"

namespace blit {

struct ElfSection {
    std::string_view name;
    u32 type = 0;  // SHT_*: 1 PROGBITS, 2 SYMTAB, 7 NOTE, 8 NOBITS, ...
    u64 flags = 0, addr = 0, offset = 0, size = 0, entsize = 0;
    u32 link = 0, info = 0;
};

struct ElfSegment {
    u32 type = 0, flags = 0;  // PT_* (1 LOAD), PF_*
    u64 offset = 0, vaddr = 0, paddr = 0, filesz = 0, memsz = 0;
};

struct ElfSymbol {
    std::string_view name;
    u64 value = 0, size = 0;  // (a Thumb function's value without its low bit)
    u8 type = 0, bind = 0;    // STT_* (0 NOTYPE, 1 OBJECT, 2 FUNC, 10 GNU_IFUNC), STB_* (0 LOCAL, 1 GLOBAL, 2 WEAK)
    u16 shndx = 0;
    std::string_view file;  // a local symbol's source file, as the STT_FILE symbol before it names it ("" none)
    bool unnamed_file = false;  // that STT_FILE symbol's name is empty (an LTO partition's): a file, though nameless
};

class Elf {
public:
    static constexpr u64 kMaxFile = 2ull << 30;      // bytes of a file read, at most
    static constexpr u64 kMaxUnpacked = 1ull << 30;  // a compressed section's bytes once unpacked, at most

    // An ELF from its bytes; null, and why, when they are not one this
    // reads (not an ELF, cut short in its headers).
    static std::shared_ptr<const Elf> read(std::string bytes, std::string* why);
    // A file here, read whole (no more than kMaxFile).
    static std::shared_ptr<const Elf> read_file(const std::string& path, std::string* why);

    bool wide() const { return wide_; }      // ELF64
    bool little() const { return little_; }  // little-endian
    u16 type() const { return type_; }       // ET_*: 1 REL, 2 EXEC, 3 DYN, 4 CORE
    u16 machine() const { return machine_; }
    u64 entry() const { return entry_; }
    // "ARM", "Xtensa", "RISC-V", "x86-64", ...; "machine 1234" for one not known.
    std::string machine_name() const;
    // The size of an address on the machine (4 or 8).
    u32 address_size() const { return wide_ ? 8 : 4; }

    const std::vector<ElfSection>& sections() const { return sections_; }
    const std::vector<ElfSegment>& segments() const { return segments_; }
    // The symbol table's (.symtab, else .dynsym) functions, objects and
    // labels (untyped symbols) that are defined in a section, by value (of
    // one value, in the table's order); an ARM mapping symbol ($t, $d) is
    // none.
    const std::vector<ElfSymbol>& symbols() const { return symbols_; }
    const ElfSection* section(std::string_view name) const;
    // A section's bytes: unpacked if the linker compressed it (SHF_COMPRESSED
    // with zlib, or GNU's older .zdebug_ sections, looked for by the name
    // without the z); empty for one with none in the file (NOBITS), one that
    // runs past the file's end, or one that cannot be unpacked.
    std::string_view data(const ElfSection& s) const;
    std::string_view data(std::string_view name) const;
    // The GNU build id in hex (NT_GNU_BUILD_ID); "" when it has none.
    std::string build_id() const;
    // The function an address is in, and how far into it it is, as
    // binutils names an address it has no debug information for: the
    // nearest function or label at or below it in the section it is in
    // (of several there, the one that takes it in, a function over a
    // label); null when it is in no section the program has in memory, or
    // nothing in its section is at or below it.
    const ElfSymbol* function_at(u64 addr, u64* offset = nullptr) const;
    // The bytes whole.
    std::string_view bytes() const { return bytes_; }

    Elf(const Elf&) = delete;
    Elf& operator=(const Elf&) = delete;
    Elf() = default;

private:
    bool parse(std::string* why);
    u64 get(u64 off, u32 n, bool* ok) const;

    std::string bytes_;
    bool wide_ = false, little_ = true;
    u16 type_ = 0, machine_ = 0;
    u64 entry_ = 0;
    std::vector<ElfSection> sections_;
    std::vector<ElfSegment> segments_;
    std::vector<ElfSymbol> symbols_;
    std::vector<u32> functions_;  // (symbols_ that are functions or labels, by value)
    // Sections unpacked when first asked for (by their index), kept for as
    // long as the file is.
    mutable std::mutex mu_;
    mutable std::unordered_map<size_t, std::unique_ptr<std::string>> unpacked_;
};

// Whether bytes start as an ELF does.
inline bool is_elf(std::string_view bytes) { return bytes.size() >= 4 && bytes.substr(0, 4) == std::string_view("\x7f" "ELF", 4); }

}  // namespace blit
