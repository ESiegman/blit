#include "elf.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace blit {

namespace {

constexpr u32 kShtSymtab = 2, kShtNote = 7, kShtNobits = 8, kShtDynsym = 11;
constexpr u64 kShfCompressed = 0x800;
constexpr u16 kEmArm = 40;

// zlib's stream into exactly `size` bytes; false if it is not that many.
bool inflate_to(std::string_view in, u64 size, std::string* out) {
    if (size > Elf::kMaxUnpacked) return false;
    // (zlib packs no better than about 1032 to 1: a size past that is a lie.)
    if (size / 1032 > in.size() + 64) return false;
    out->assign(size, '\0');
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.next_out = reinterpret_cast<Bytef*>(out->data());
    u64 in_left = in.size(), out_left = size;
    int r = Z_OK;
    while (r == Z_OK) {
        // (avail_in and avail_out are 32 bits wide; fed in parts.)
        const uInt take_in = static_cast<uInt>(std::min<u64>(in_left, 1u << 30));
        const uInt take_out = static_cast<uInt>(std::min<u64>(out_left, 1u << 30));
        z.avail_in = take_in;
        z.avail_out = take_out;
        r = inflate(&z, Z_NO_FLUSH);
        in_left -= take_in - z.avail_in;
        out_left -= take_out - z.avail_out;
        if (r == Z_OK && take_in == z.avail_in && take_out == z.avail_out) break;  // (no progress: cut short)
        if (r == Z_BUF_ERROR) break;
    }
    inflateEnd(&z);
    if (r != Z_STREAM_END || out_left != 0) {
        out->clear();
        return false;
    }
    return true;
}

}  // namespace

u64 Elf::get(u64 off, u32 n, bool* ok) const {
    if (off > bytes_.size() || n > bytes_.size() - off) {
        *ok = false;
        return 0;
    }
    u64 v = 0;
    for (u32 i = 0; i < n; ++i) {
        const u64 b = static_cast<u8>(bytes_[off + (little_ ? i : n - 1 - i)]);
        v |= b << (8 * i);
    }
    return v;
}

std::shared_ptr<const Elf> Elf::read(std::string bytes, std::string* why) {
    auto e = std::make_shared<Elf>();
    e->bytes_ = std::move(bytes);
    if (!e->parse(why)) return nullptr;
    return e;
}

std::shared_ptr<const Elf> Elf::read_file(const std::string& path, std::string* why) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *why = path + ": " + std::strerror(errno);
        return nullptr;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        *why = path + " is not a file";
        return nullptr;
    }
    if (static_cast<u64>(st.st_size) > kMaxFile) {
        ::close(fd);
        *why = path + " is larger than " + std::to_string(kMaxFile >> 20) + " MB";
        return nullptr;
    }
    std::string bytes(static_cast<size_t>(st.st_size), '\0');
    size_t got = 0;
    while (got < bytes.size()) {
        const ssize_t k = ::read(fd, bytes.data() + got, bytes.size() - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += static_cast<size_t>(k);
    }
    ::close(fd);
    bytes.resize(got);  // (one being written to may have got shorter)
    std::string w;
    auto e = read(std::move(bytes), &w);
    if (!e) *why = path + ": " + w;
    return e;
}

bool Elf::parse(std::string* why) {
    const std::string_view b = bytes_;
    if (!is_elf(b)) return *why = "not an ELF", false;
    if (b.size() < 52 || (b[4] != 1 && b[4] != 2) || (b[5] != 1 && b[5] != 2)) return *why = "not an ELF this reads (its class or byte order)", false;
    wide_ = b[4] == 2;
    little_ = b[5] == 1;
    if (wide_ && b.size() < 64) return *why = "cut short in its header", false;
    bool ok = true;
    type_ = static_cast<u16>(get(16, 2, &ok));
    machine_ = static_cast<u16>(get(18, 2, &ok));
    entry_ = get(24, wide_ ? 8 : 4, &ok);
    const u64 phoff = wide_ ? get(32, 8, &ok) : get(28, 4, &ok);
    const u64 shoff = wide_ ? get(40, 8, &ok) : get(32, 4, &ok);
    const u64 phentsize = get(wide_ ? 54 : 42, 2, &ok);
    u64 phnum = get(wide_ ? 56 : 44, 2, &ok);
    const u64 shentsize = get(wide_ ? 58 : 46, 2, &ok);
    u64 shnum = get(wide_ ? 60 : 48, 2, &ok);
    u64 shstrndx = get(wide_ ? 62 : 50, 2, &ok);
    if (!ok) return *why = "cut short in its header", false;

    // Sections. (Past 0xff00 of them, section 0 holds the count and the
    // names' index.)
    const u64 sh_min = wide_ ? 64 : 40;
    if (shoff && shentsize >= sh_min && shoff < b.size()) {
        if (shnum == 0) shnum = get(shoff + (wide_ ? 32 : 20), wide_ ? 8 : 4, &ok);
        if (shstrndx == 0xffff) shstrndx = get(shoff + (wide_ ? 40 : 24), 4, &ok);
        if (!ok) return *why = "cut short in its section headers", false;
        shnum = std::min<u64>(shnum, (b.size() - shoff) / shentsize);  // (no more than the file has room for)
        sections_.reserve(static_cast<size_t>(shnum));
        std::vector<u32> name_at;
        name_at.reserve(static_cast<size_t>(shnum));
        for (u64 i = 0; i < shnum; ++i) {
            const u64 h = shoff + i * shentsize;
            ElfSection s;
            name_at.push_back(static_cast<u32>(get(h, 4, &ok)));
            s.type = static_cast<u32>(get(h + 4, 4, &ok));
            if (wide_) {
                s.flags = get(h + 8, 8, &ok), s.addr = get(h + 16, 8, &ok), s.offset = get(h + 24, 8, &ok), s.size = get(h + 32, 8, &ok);
                s.link = static_cast<u32>(get(h + 40, 4, &ok)), s.info = static_cast<u32>(get(h + 44, 4, &ok)), s.entsize = get(h + 56, 8, &ok);
            } else {
                s.flags = get(h + 8, 4, &ok), s.addr = get(h + 12, 4, &ok), s.offset = get(h + 16, 4, &ok), s.size = get(h + 20, 4, &ok);
                s.link = static_cast<u32>(get(h + 24, 4, &ok)), s.info = static_cast<u32>(get(h + 28, 4, &ok)), s.entsize = get(h + 36, 4, &ok);
            }
            sections_.push_back(s);
        }
        if (!ok) return *why = "cut short in its section headers", false;
        // Their names, from the section of names (each cut at the end of it).
        if (shstrndx < sections_.size()) {
            const ElfSection& names = sections_[static_cast<size_t>(shstrndx)];
            if (names.type != kShtNobits && names.offset <= b.size() && names.size <= b.size() - names.offset) {
                const std::string_view t = b.substr(static_cast<size_t>(names.offset), static_cast<size_t>(names.size));
                for (size_t i = 0; i < sections_.size(); ++i) {
                    if (name_at[i] >= t.size()) continue;
                    const std::string_view rest = t.substr(name_at[i]);
                    sections_[i].name = rest.substr(0, std::min(rest.find('\0'), rest.size()));
                }
            }
        }
    }

    // Segments.
    const u64 ph_min = wide_ ? 56 : 32;
    if (phoff && phentsize >= ph_min && phoff < b.size()) {
        phnum = std::min<u64>(phnum, (b.size() - phoff) / phentsize);
        segments_.reserve(static_cast<size_t>(phnum));
        for (u64 i = 0; i < phnum; ++i) {
            const u64 p = phoff + i * phentsize;
            ElfSegment s;
            s.type = static_cast<u32>(get(p, 4, &ok));
            if (wide_) {
                s.flags = static_cast<u32>(get(p + 4, 4, &ok)), s.offset = get(p + 8, 8, &ok), s.vaddr = get(p + 16, 8, &ok);
                s.paddr = get(p + 24, 8, &ok), s.filesz = get(p + 32, 8, &ok), s.memsz = get(p + 40, 8, &ok);
            } else {
                s.offset = get(p + 4, 4, &ok), s.vaddr = get(p + 8, 4, &ok), s.paddr = get(p + 12, 4, &ok);
                s.filesz = get(p + 16, 4, &ok), s.memsz = get(p + 20, 4, &ok), s.flags = static_cast<u32>(get(p + 24, 4, &ok));
            }
            segments_.push_back(s);
        }
        if (!ok) return *why = "cut short in its program headers", false;
    }

    // Symbols: the full table, else the dynamic one.
    const ElfSection* table = nullptr;
    for (const ElfSection& s : sections_)
        if (s.type == kShtSymtab) table = &s;
    if (!table)
        for (const ElfSection& s : sections_)
            if (s.type == kShtDynsym) table = &s;
    if (table && table->link < sections_.size()) {
        const ElfSection& strs = sections_[table->link];
        const u64 ent = wide_ ? 24 : 16;
        const bool in = table->offset <= b.size() && table->size <= b.size() - table->offset;
        const bool strs_in = strs.type != kShtNobits && strs.offset <= b.size() && strs.size <= b.size() - strs.offset;
        if (in && strs_in && table->type != kShtNobits) {
            const std::string_view names = b.substr(static_cast<size_t>(strs.offset), static_cast<size_t>(strs.size));
            const u64 n = table->size / ent;
            std::string_view file;  // (the source file the symbols after an STT_FILE symbol came from)
            bool file_seen = false;
            for (u64 i = 1; i < n; ++i) {  // (0 is the null symbol)
                const u64 at = table->offset + i * ent;
                ElfSymbol y;
                const u32 name = static_cast<u32>(get(at, 4, &ok));
                u8 info;
                if (wide_) {
                    info = static_cast<u8>(get(at + 4, 1, &ok));
                    y.shndx = static_cast<u16>(get(at + 6, 2, &ok));
                    y.value = get(at + 8, 8, &ok);
                    y.size = get(at + 16, 8, &ok);
                } else {
                    y.value = get(at + 4, 4, &ok);
                    y.size = get(at + 8, 4, &ok);
                    info = static_cast<u8>(get(at + 12, 1, &ok));
                    y.shndx = static_cast<u16>(get(at + 14, 2, &ok));
                }
                if (!ok) break;
                y.type = info & 0xf;
                y.bind = info >> 4;
                // (Undefined, absolute or common; not code nor data; an
                // ARM mapping symbol, $t or $d; annobin's hidden local labels.)
                const u8 other = static_cast<u8>(get(at + (wide_ ? 5 : 13), 1, &ok));
                if (y.type == 4) {  // (STT_FILE)
                    const std::string_view r = name < names.size() ? names.substr(name) : std::string_view();
                    file = r.substr(0, std::min(r.find('\0'), r.size()));
                    file_seen = true;
                    continue;
                }
                if (y.bind == 0) y.file = file, y.unnamed_file = file_seen && file.empty();
                if (y.shndx == 0 || y.shndx >= 0xff00 || (y.type != 0 && y.type != 1 && y.type != 2 && y.type != 10) || name >= names.size()) continue;
                const std::string_view rest = names.substr(name);
                y.name = rest.substr(0, std::min(rest.find('\0'), rest.size()));
                if (y.name.empty() || (y.name[0] == '$' && y.bind == 0)) continue;
                if (y.type == 0 && y.size == 0 && y.bind == 0 && (other & 3) == 2) continue;
                if (machine_ == kEmArm && (y.type == 2 || y.type == 0)) y.value &= ~1ull;  // (Thumb's)
                symbols_.push_back(y);
            }
            ok = true;
        }
    }
    std::ranges::stable_sort(symbols_, {}, &ElfSymbol::value);
    for (size_t i = 0; i < symbols_.size(); ++i)
        if (symbols_[i].type != 1) functions_.push_back(static_cast<u32>(i));
    return true;
}

std::string Elf::machine_name() const {
    switch (machine_) {
    case 2: return "SPARC";
    case 3: return "x86";
    case 8: return "MIPS";
    case 20: return "PowerPC";
    case 21: return "PowerPC 64";
    case 40: return "ARM";
    case 62: return "x86-64";
    case 83: return "AVR";
    case 94: return "Xtensa";
    case 105: return "MSP430";
    case 183: return "AArch64";
    case 224: return "AMDGPU";
    case 243: return "RISC-V";
    default: return "machine " + std::to_string(machine_);
    }
}

const ElfSection* Elf::section(std::string_view name) const {
    for (const ElfSection& s : sections_)
        if (s.name == name) return &s;
    return nullptr;
}

std::string_view Elf::data(std::string_view name) const {
    if (const ElfSection* s = section(name)) return data(*s);
    // GNU's older compressed debug sections: .zdebug_info for .debug_info.
    if (name.starts_with(".debug_")) {
        const std::string z = ".z" + std::string(name.substr(1));
        if (const ElfSection* s = section(z)) return data(*s);
    }
    return {};
}

std::string_view Elf::data(const ElfSection& s) const {
    if (s.type == kShtNobits || s.offset > bytes_.size() || s.size > bytes_.size() - s.offset) return {};
    const std::string_view raw = std::string_view(bytes_).substr(static_cast<size_t>(s.offset), static_cast<size_t>(s.size));
    const bool zdebug = s.name.starts_with(".zdebug_");
    if (!(s.flags & kShfCompressed) && !zdebug) return raw;
    const size_t index = static_cast<size_t>(&s - sections_.data());
    std::lock_guard lock(mu_);
    if (const auto it = unpacked_.find(index); it != unpacked_.end()) return *it->second;
    auto out = std::make_unique<std::string>();
    bool ok = true;
    if (s.flags & kShfCompressed) {
        // Elf32_Chdr: type, size, alignment (4 bytes each); Elf64_Chdr:
        // type, reserved (4 bytes each), size, alignment (8 each). Type 1
        // is zlib; 2 (zstd) is not read.
        const u64 head = wide_ ? 24 : 12;
        const u64 at = s.offset;
        const u64 kind = get(at, 4, &ok);
        const u64 size = wide_ ? get(at + 8, 8, &ok) : get(at + 4, 4, &ok);
        if (ok && kind == 1 && raw.size() >= head && !inflate_to(raw.substr(head), size, out.get())) out->clear();
    } else if (raw.size() >= 12 && raw.starts_with("ZLIB")) {  // ("ZLIB" and the size, 8 bytes big-endian)
        u64 size = 0;
        for (int i = 0; i < 8; ++i) size = size << 8 | static_cast<u8>(raw[4 + static_cast<size_t>(i)]);
        if (!inflate_to(raw.substr(12), size, out.get())) out->clear();
    }
    const std::string_view v = *out;
    unpacked_.emplace(index, std::move(out));
    return v;
}

std::string Elf::build_id() const {
    for (const ElfSection& s : sections_) {
        if (s.type != kShtNote) continue;
        const std::string_view d = data(s);
        bool ok = true;
        // Notes: name's size, description's size, type; the name and the
        // description, each padded to 4 bytes.
        for (u64 at = 0; at + 12 <= d.size();) {
            const u64 base = s.offset + at;
            const u64 namesz = get(base, 4, &ok), descsz = get(base + 4, 4, &ok), type = get(base + 8, 4, &ok);
            if (!ok || namesz > d.size() || descsz > d.size()) break;
            const u64 name_at = at + 12, desc_at = name_at + ((namesz + 3) & ~3ull);
            if (desc_at > d.size() || descsz > d.size() - desc_at) break;
            if (type == 3 && namesz == 4 && d.substr(static_cast<size_t>(name_at), 4) == std::string_view("GNU\0", 4)) {
                static const char kHex[] = "0123456789abcdef";
                std::string out;
                for (u64 i = 0; i < descsz; ++i) {
                    const u8 c = static_cast<u8>(d[static_cast<size_t>(desc_at + i)]);
                    out += kHex[c >> 4];
                    out += kHex[c & 15];
                }
                return out;
            }
            at = desc_at + ((descsz + 3) & ~3ull);
        }
    }
    return {};
}

const ElfSymbol* Elf::function_at(u64 addr, u64* offset) const {
    // As binutils names an address it has no debug information for
    // (bfd's elf_find_function): of the code symbols in the section the
    // address is in, the nearest at or below it; of several there, in the
    // table's order, one that takes the address in over one that does not
    // (else the larger), a function over a label, and then the smaller.
    u16 in = 0;
    for (size_t i = 0; i < sections_.size(); ++i) {
        const ElfSection& s = sections_[i];
        if ((s.flags & 2) && addr >= s.addr && addr - s.addr < s.size) in = static_cast<u16>(std::min<size_t>(i, 0xffff));  // (SHF_ALLOC)
    }
    if (!in) return nullptr;
    auto it = std::ranges::upper_bound(functions_, addr, {}, [&](u32 i) { return symbols_[i].value; });
    // The nearest value at or below it that a symbol of the section has.
    auto j = it;
    while (j != functions_.begin() && symbols_[*(j - 1)].shndx != in) --j;
    if (j == functions_.begin()) return nullptr;
    const u64 at = symbols_[*(j - 1)].value;
    auto from = j;
    while (from != functions_.begin() && symbols_[*(from - 1)].value == at) --from;
    const ElfSymbol* best = nullptr;
    auto size = [](const ElfSymbol& y) { return y.size ? y.size : 1; };
    for (auto k = from; k != j; ++k) {
        const ElfSymbol& y = symbols_[*k];
        if (y.shndx != in) continue;
        if (!best) {
            best = &y;
            continue;
        }
        const bool best_covers = addr - best->value < size(*best), covers = addr - y.value < size(y);
        bool better;
        if (!best_covers) better = size(y) > size(*best);
        else if (!covers) better = false;
        else if (best->type != y.type && (best->type == 2 || y.type == 2)) better = y.type == 2;
        else if ((best->type == 0) != (y.type == 0)) better = y.type != 0;
        else better = size(y) < size(*best);
        if (better) best = &y;
    }
    if (offset) *offset = addr - best->value;
    return best;
}

}  // namespace blit
