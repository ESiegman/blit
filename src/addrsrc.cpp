#include "addrsrc.hpp"

#include <cxxabi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace blit {

namespace {

// DWARF's numbers, those read here.
constexpr u64 kTagInlined = 0x1d, kTagCompileUnit = 0x11, kTagSubprogram = 0x2e, kTagPartialUnit = 0x3c, kTagSkeletonUnit = 0x4a;
constexpr u32 kAtName = 0x03, kAtStmtList = 0x10, kAtLowPc = 0x11, kAtHighPc = 0x12, kAtCompDir = 0x1b, kAtLanguage = 0x13, kAtAbstractOrigin = 0x31,
              kAtSpecification = 0x47, kAtRanges = 0x55, kAtCallColumn = 0x57, kAtCallFile = 0x58, kAtCallLine = 0x59, kAtLinkageName = 0x6e,
              kAtStrOffsetsBase = 0x72, kAtAddrBase = 0x73, kAtRnglistsBase = 0x74, kAtMipsLinkageName = 0x2007, kAtGnuAddrBase = 0x2133;
enum : u32 {
    kFormAddr = 0x01, kFormBlock2 = 0x03, kFormBlock4 = 0x04, kFormData2 = 0x05, kFormData4 = 0x06, kFormData8 = 0x07, kFormString = 0x08,
    kFormBlock = 0x09, kFormBlock1 = 0x0a, kFormData1 = 0x0b, kFormFlag = 0x0c, kFormSdata = 0x0d, kFormStrp = 0x0e, kFormUdata = 0x0f,
    kFormRefAddr = 0x10, kFormRef1 = 0x11, kFormRef2 = 0x12, kFormRef4 = 0x13, kFormRef8 = 0x14, kFormRefUdata = 0x15, kFormIndirect = 0x16,
    kFormSecOffset = 0x17, kFormExprloc = 0x18, kFormFlagPresent = 0x19, kFormStrx = 0x1a, kFormAddrx = 0x1b, kFormRefSup4 = 0x1c,
    kFormStrpSup = 0x1d, kFormData16 = 0x1e, kFormLineStrp = 0x1f, kFormRefSig8 = 0x20, kFormImplicitConst = 0x21, kFormLoclistx = 0x22,
    kFormRnglistx = 0x23, kFormRefSup8 = 0x24, kFormStrx1 = 0x25, kFormStrx2 = 0x26, kFormStrx3 = 0x27, kFormStrx4 = 0x28, kFormAddrx1 = 0x29,
    kFormAddrx2 = 0x2a, kFormAddrx3 = 0x2b, kFormAddrx4 = 0x2c, kFormGnuAddrIndex = 0x1f01, kFormGnuStrIndex = 0x1f02, kFormGnuRefAlt = 0x1f20,
    kFormGnuStrpAlt = 0x1f21,
};
constexpr u32 kLnctPath = 1, kLnctDirectoryIndex = 2;
constexpr u64 kNone = ~0ull;
constexpr u32 kMaxRanges = 1u << 16;  // a range list's entries, at most
constexpr u32 kMaxHops = 8;           // abstract origins and specifications followed for a name, at most

// A section's bytes, read from `at` with every read checked: past the end,
// a read gives 0 and the cursor is no longer ok (and stays at the end).
struct Cursor {
    std::string_view d;
    u64 at = 0;
    bool little = true;
    bool ok = true;

    u64 left() const { return at < d.size() ? d.size() - at : 0; }
    void fail() {
        ok = false;
        at = d.size();
    }
    u64 u(u32 n) {
        if (at > d.size() || n > d.size() - at) return fail(), 0;
        u64 v = 0;
        for (u32 i = 0; i < n; ++i) v |= static_cast<u64>(static_cast<u8>(d[at + (little ? i : n - 1 - i)])) << (8 * i);
        at += n;
        return v;
    }
    u64 uleb() {
        u64 v = 0;
        u32 shift = 0;
        for (;;) {
            if (at >= d.size()) return fail(), 0;
            const u8 b = static_cast<u8>(d[at++]);
            if (shift < 64) v |= static_cast<u64>(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) return v;
        }
    }
    i64 sleb() {
        u64 v = 0;
        u32 shift = 0;
        u8 b = 0;
        do {
            if (at >= d.size()) return fail(), 0;
            b = static_cast<u8>(d[at++]);
            if (shift < 64) v |= static_cast<u64>(b & 0x7f) << shift;
            shift += 7;
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) v |= ~0ull << shift;
        return static_cast<i64>(v);
    }
    std::string_view cstr() {
        if (at >= d.size()) return fail(), std::string_view();
        const size_t nul = d.find('\0', at);
        if (nul == std::string_view::npos) return fail(), std::string_view();
        const std::string_view s = d.substr(at, nul - at);
        at = nul + 1;
        return s;
    }
    void skip(u64 n) {
        if (n > left()) return fail();
        at += n;
    }
};

// A NUL-ended string at an offset into a section of strings; "" past it.
std::string_view str_at(std::string_view s, u64 off) {
    if (off >= s.size()) return {};
    const std::string_view rest = s.substr(off);
    return rest.substr(0, std::min(rest.find('\0'), rest.size()));
}

struct Sections {
    std::string_view info, abbrev, str, line, line_str, str_offsets, addr, ranges, rnglists;
};

// A unit's abbreviations: for each code, its tag, whether it has children,
// and its attributes' names and forms.
struct AbbrevTable {
    struct Spec {
        u32 name = 0, form = 0;
        i64 implicit = 0;
    };
    struct Entry {
        u64 tag = 0;
        bool children = false;
        bool set = false;
        u32 first = 0, count = 0;
    };
    std::vector<Entry> dense;  // by code, for the small codes compilers give
    std::unordered_map<u64, Entry> sparse;
    std::vector<Spec> specs;

    const Entry* find(u64 code) const {
        if (code < dense.size()) return dense[code].set ? &dense[code] : nullptr;
        const auto it = sparse.find(code);
        return it != sparse.end() ? &it->second : nullptr;
    }
    void read(std::string_view d, u64 off, bool little) {
        Cursor c{d, off, little};
        size_t n = 0;
        while (c.ok && c.at < d.size()) {
            const u64 code = c.uleb();
            if (code == 0 || !c.ok) break;
            Entry e;
            e.tag = c.uleb();
            e.children = c.u(1) != 0;
            e.set = true;
            e.first = static_cast<u32>(specs.size());
            for (;;) {
                Spec s;
                const u64 name = c.uleb(), form = c.uleb();
                if (!c.ok) break;
                if (form == kFormImplicitConst) s.implicit = c.sleb();
                if (name == 0 && form == 0) break;
                s.name = static_cast<u32>(std::min<u64>(name, 0xffffffffu));
                s.form = static_cast<u32>(std::min<u64>(form, 0xffffffffu));
                specs.push_back(s);
            }
            e.count = static_cast<u32>(specs.size() - e.first);
            ++n;
            // (Codes are small and dense from a compiler; a vector grows only
            // as far as twice the entries read, the rest go in a map.)
            if (code < 2 * n + 64) {
                if (code >= dense.size()) dense.resize(code + 1);
                if (!dense[code].set) dense[code] = e;
            } else {
                sparse.emplace(code, e);
            }
        }
    }
};

// What an attribute's value is.
struct Val {
    enum Kind : u8 { kNothing, kAddr, kConst, kSigned, kStr, kRef, kSecOff, kStrx, kAddrx, kRnglistx, kFlag } kind = kNothing;
    u64 u = 0;
    i64 s = 0;
    std::string_view str;
    bool constant() const { return kind == kConst || kind == kSigned; }
};

struct Line {
    u64 addr = 0;
    u32 line = 0;
    u16 file = 0, col = 0;
};
struct Seq {
    u64 lo = 0, hi = 0;
    u32 first = 0, count = 0;
};
struct Func {
    u32 parent = ~0u;
    u32 depth = 0;
    u32 ranges_first = 0, ranges_count = 0;
    bool inlined = false;
    std::string_view name, linkage;
    u64 origin = kNone, spec = kNone;
    u32 call_file = 0, call_line = 0, call_col = 0;
};

// A unit's lines and functions, once read.
struct Tables {
    std::vector<std::string> files;  // by the number its line table gives (DWARF 5 from 0; before it, from 1, and 0 is the unit's own file)
    std::vector<Line> rows;
    std::vector<Seq> seqs;  // (by lo)
    std::vector<Func> funcs;
    std::vector<std::pair<u64, u64>> ranges;
};

// A unit's header and what its own entry says.
struct Head {
    u64 offset = 0, end = 0, die = 0;  // (in .debug_info: its header, past its end, its first entry)
    u16 version = 0;
    u8 addr_size = 4;
    bool dwarf64 = false;
    u64 abbrev_off = 0;
    std::string_view name, comp_dir;
    u64 low_pc = 0;
    u64 stmt_list = kNone;
    u64 str_offsets_base = 8, addr_base = 0, rnglists_base = 0;
    bool rnglists_base_set = false;
    bool mangled = true;  // its language's names may be mangled (as binutils has it: all but C's and the like; a unit that says none too)
};
struct Unit : Head {
    std::unique_ptr<Tables> tables;
};

// Whether a language's names may be mangled, as binutils decides it
// (bfd's mangle_style): all but C, COBOL, Fortran 77 and 2018, Pascal,
// PL/I, UPC and the assemblers.
bool mangled_language(u64 lang) {
    switch (lang) {
    case 0x01: case 0x02: case 0x05: case 0x06: case 0x07: case 0x09: case 0x0c: case 0x0f: case 0x12: case 0x1d: case 0x2c: case 0x2d:
    case 0x31: case 0x3e: case 0x3f: case 0x8001: case 0x8004: case 0x8006: case 0x8007: case 0x8765: return false;
    default: return true;
    }
}

std::string join_path(std::string_view dir, std::string_view name) {
    const bool whole = name.starts_with('/') || (name.size() > 2 && name[1] == ':' && (name[2] == '/' || name[2] == '\\'));
    if (whole || dir.empty()) return std::string(name);
    std::string out(dir);
    if (!out.ends_with('/') && !out.ends_with('\\')) out += '/';
    out += name;
    return out;
}

}  // namespace

struct AddrSource::Impl {
    const Elf* elf = nullptr;
    Sections s;
    bool little = true;
    mutable std::vector<Unit> units;  // (by offset; their tables read when first needed)
    // Each unit's code, by where it starts: a unit's DIE ranges (or, with
    // none, its line table's sequences); maxhi[i] the highest end of those
    // up to i, so that every range holding an address is found going back
    // from it.
    struct UnitRange {
        u64 lo = 0, hi = 0;
        u32 unit = 0;
    };
    std::vector<UnitRange> unit_ranges;
    std::vector<u64> maxhi;
    // The sections the program has in memory, as [lo, hi): an address in
    // none of them is no place in it (what a linker dropped is at 0).
    std::vector<std::pair<u64, u64>> loaded;
    mutable std::mutex mu;
    mutable std::unordered_map<u64, std::unique_ptr<AbbrevTable>> abbrevs;

    const AbbrevTable& abbrev(u64 off) const {
        auto& slot = abbrevs[off];
        if (!slot) {
            slot = std::make_unique<AbbrevTable>();
            slot->read(s.abbrev, off, little);
        }
        return *slot;
    }

    u64 offset(Cursor& c, bool dwarf64) const { return c.u(dwarf64 ? 8 : 4); }

    // An attribute's value, read in its form (with the unit's sizes); false
    // for a form not known, after which nothing more of the entry can be read.
    bool value(Cursor& c, u32 form, i64 implicit, const Head& u, Val* v) const {
        *v = Val{};
        for (int indirect = 0; indirect < 4; ++indirect) {
            switch (form) {
            case kFormAddr: v->kind = Val::kAddr, v->u = c.u(u.addr_size); return c.ok;
            case kFormData1: v->kind = Val::kConst, v->u = c.u(1); return c.ok;
            case kFormData2: v->kind = Val::kConst, v->u = c.u(2); return c.ok;
            case kFormData4: v->kind = Val::kConst, v->u = c.u(4); return c.ok;
            case kFormData8: v->kind = Val::kConst, v->u = c.u(8); return c.ok;
            case kFormData16: c.skip(16); return c.ok;
            case kFormSdata: v->kind = Val::kSigned, v->s = c.sleb(), v->u = static_cast<u64>(v->s); return c.ok;
            case kFormUdata: v->kind = Val::kConst, v->u = c.uleb(); return c.ok;
            case kFormImplicitConst: v->kind = Val::kSigned, v->s = implicit, v->u = static_cast<u64>(implicit); return true;
            case kFormFlag: v->kind = Val::kFlag, v->u = c.u(1); return c.ok;
            case kFormFlagPresent: v->kind = Val::kFlag, v->u = 1; return true;
            case kFormString: v->kind = Val::kStr, v->str = c.cstr(); return c.ok;
            case kFormStrp: v->kind = Val::kStr, v->str = str_at(s.str, offset(c, u.dwarf64)); return c.ok;
            case kFormLineStrp: v->kind = Val::kStr, v->str = str_at(s.line_str, offset(c, u.dwarf64)); return c.ok;
            case kFormStrpSup:
            case kFormGnuStrpAlt: offset(c, u.dwarf64); return c.ok;  // (in another file: no string)
            case kFormStrx:
            case kFormGnuStrIndex: v->kind = Val::kStrx, v->u = c.uleb(); return c.ok;
            case kFormStrx1: v->kind = Val::kStrx, v->u = c.u(1); return c.ok;
            case kFormStrx2: v->kind = Val::kStrx, v->u = c.u(2); return c.ok;
            case kFormStrx3: v->kind = Val::kStrx, v->u = c.u(3); return c.ok;
            case kFormStrx4: v->kind = Val::kStrx, v->u = c.u(4); return c.ok;
            case kFormAddrx:
            case kFormGnuAddrIndex: v->kind = Val::kAddrx, v->u = c.uleb(); return c.ok;
            case kFormAddrx1: v->kind = Val::kAddrx, v->u = c.u(1); return c.ok;
            case kFormAddrx2: v->kind = Val::kAddrx, v->u = c.u(2); return c.ok;
            case kFormAddrx3: v->kind = Val::kAddrx, v->u = c.u(3); return c.ok;
            case kFormAddrx4: v->kind = Val::kAddrx, v->u = c.u(4); return c.ok;
            case kFormRef1: v->kind = Val::kRef, v->u = u.offset + c.u(1); return c.ok;
            case kFormRef2: v->kind = Val::kRef, v->u = u.offset + c.u(2); return c.ok;
            case kFormRef4: v->kind = Val::kRef, v->u = u.offset + c.u(4); return c.ok;
            case kFormRef8: v->kind = Val::kRef, v->u = u.offset + c.u(8); return c.ok;
            case kFormRefUdata: v->kind = Val::kRef, v->u = u.offset + c.uleb(); return c.ok;
            case kFormRefAddr: v->kind = Val::kRef, v->u = u.version <= 2 ? c.u(u.addr_size) : offset(c, u.dwarf64); return c.ok;
            case kFormRefSig8: c.skip(8); return c.ok;
            case kFormRefSup4: c.skip(4); return c.ok;
            case kFormRefSup8: c.skip(8); return c.ok;
            case kFormGnuRefAlt: offset(c, u.dwarf64); return c.ok;
            case kFormSecOffset: v->kind = Val::kSecOff, v->u = offset(c, u.dwarf64); return c.ok;
            case kFormRnglistx: v->kind = Val::kRnglistx, v->u = c.uleb(); return c.ok;
            case kFormLoclistx: c.uleb(); return c.ok;
            case kFormExprloc:
            case kFormBlock: c.skip(c.uleb()); return c.ok;
            case kFormBlock1: c.skip(c.u(1)); return c.ok;
            case kFormBlock2: c.skip(c.u(2)); return c.ok;
            case kFormBlock4: c.skip(c.u(4)); return c.ok;
            case kFormIndirect: form = static_cast<u32>(std::min<u64>(c.uleb(), 0xffffffffu)); continue;
            default: c.fail(); return false;
            }
        }
        c.fail();
        return false;
    }

    // An index's string and address, through the unit's bases.
    std::string_view strx(const Head& u, u64 index) const {
        const u64 size = u.dwarf64 ? 8 : 4;
        if (index > s.str_offsets.size() / size) return {};
        Cursor c{s.str_offsets, u.str_offsets_base + index * size, little};
        const u64 off = c.u(static_cast<u32>(size));
        return c.ok ? str_at(s.str, off) : std::string_view();
    }
    u64 addrx(const Head& u, u64 index) const {
        if (index > s.addr.size() / u.addr_size) return 0;
        Cursor c{s.addr, u.addr_base + index * u.addr_size, little};
        return c.u(u.addr_size);
    }
    std::string_view string(const Head& u, const Val& v) const { return v.kind == Val::kStr ? v.str : v.kind == Val::kStrx ? strx(u, v.u) : std::string_view(); }
    u64 address(const Head& u, const Val& v) const { return v.kind == Val::kAddrx ? addrx(u, v.u) : v.u; }

    // A range list, as pairs of [lo, hi): DWARF 5's (.debug_rnglists) or
    // before it (.debug_ranges); `base` the unit's base address.
    void range_list(const Head& u, const Val& v, u64 base, std::vector<std::pair<u64, u64>>* out) const {
        const u64 top = u.addr_size >= 8 ? ~0ull : (1ull << (8 * u.addr_size)) - 1;
        if (u.version < 5) {
            if (v.kind != Val::kSecOff && v.kind != Val::kConst) return;
            Cursor c{s.ranges, v.u, little};
            for (u32 n = 0; n < kMaxRanges && c.ok; ++n) {
                const u64 lo = c.u(u.addr_size), hi = c.u(u.addr_size);
                if (!c.ok || (lo == 0 && hi == 0)) break;
                if (lo == top) {
                    base = hi;
                    continue;
                }
                if (hi > lo) out->emplace_back(base + lo, base + hi);
            }
            return;
        }
        u64 at = v.u;
        if (v.kind == Val::kRnglistx) {
            if (!u.rnglists_base_set) return;
            const u64 size = u.dwarf64 ? 8 : 4;
            if (v.u > s.rnglists.size() / size) return;
            Cursor c{s.rnglists, u.rnglists_base + v.u * size, little};
            at = u.rnglists_base + c.u(static_cast<u32>(size));
            if (!c.ok) return;
        } else if (v.kind != Val::kSecOff) {
            return;
        }
        Cursor c{s.rnglists, at, little};
        for (u32 n = 0; n < kMaxRanges && c.ok; ++n) {
            const u64 kind = c.u(1);
            u64 lo = 0, hi = 0;
            switch (kind) {
            case 0: return;  // (DW_RLE_end_of_list)
            case 1: base = addrx(u, c.uleb()); continue;
            case 2: lo = addrx(u, c.uleb()), hi = addrx(u, c.uleb()); break;
            case 3: lo = addrx(u, c.uleb()), hi = lo + c.uleb(); break;
            case 4: lo = base + c.uleb(), hi = base + c.uleb(); break;
            case 5: base = c.u(u.addr_size); continue;
            case 6: lo = c.u(u.addr_size), hi = c.u(u.addr_size); break;
            case 7: lo = c.u(u.addr_size), hi = lo + c.uleb(); break;
            default: return;
            }
            if (c.ok && hi > lo) out->emplace_back(lo, hi);
        }
    }

    // The ranges an entry's code takes: low_pc and high_pc (an end, or a
    // length when a constant), or its ranges.
    void pc_ranges(const Head& u, const Val& low, const Val& high, const Val& ranges, std::vector<std::pair<u64, u64>>* out) const {
        if (ranges.kind != Val::kNothing) {
            range_list(u, ranges, u.low_pc, out);
            return;
        }
        if (low.kind != Val::kAddr && low.kind != Val::kAddrx) return;
        const u64 lo = address(u, low);
        u64 hi = 0;
        if (high.constant()) hi = lo + high.u;
        else if (high.kind == Val::kAddr || high.kind == Val::kAddrx) hi = address(u, high);
        if (hi > lo) out->emplace_back(lo, hi);
    }

    // Reading the units' headers and first entries (the constructor's).
    void read_units() {
        Cursor c{s.info, 0, little};
        while (c.ok && c.left() >= 11) {
            Unit u;
            u.offset = c.at;
            u64 len = c.u(4);
            if (len >= 0xfffffff0u) {
                if (len != 0xffffffffu) break;
                u.dwarf64 = true;
                len = c.u(8);
            }
            if (!c.ok || len > c.left()) break;
            u.end = c.at + len;
            u.version = static_cast<u16>(c.u(2));
            u8 type = 1;  // (DW_UT_compile)
            if (u.version >= 5) {
                type = static_cast<u8>(c.u(1));
                u.addr_size = static_cast<u8>(c.u(1));
                u.abbrev_off = offset(c, u.dwarf64);
                if (type == 4 || type == 5) c.skip(8);                                     // (a skeleton's or split unit's id)
                if (type == 2 || type == 6) c.skip(8), offset(c, u.dwarf64);               // (a type unit's signature and type)
            } else {
                u.abbrev_off = offset(c, u.dwarf64);
                u.addr_size = static_cast<u8>(c.u(1));
            }
            u.die = c.at;
            const bool fine = c.ok && u.version >= 2 && u.version <= 5 && (u.addr_size == 2 || u.addr_size == 4 || u.addr_size == 8) && u.die <= u.end;
            c.at = u.end;
            if (!fine || type == 2 || type == 6) continue;
            first_entry(u);
            units.push_back(std::move(u));
        }
    }

    // A unit's own entry: its name, folder, bases and code.
    void first_entry(Head& u) {
        const AbbrevTable& ab = abbrev(u.abbrev_off);
        Cursor c{s.info.substr(0, u.end), u.die, little};
        const AbbrevTable::Entry* e = ab.find(c.uleb());
        if (!e || (e->tag != kTagCompileUnit && e->tag != kTagPartialUnit && e->tag != kTagSkeletonUnit)) return;
        Val name, dir, low, high, ranges;
        for (u32 i = 0; i < e->count; ++i) {
            const AbbrevTable::Spec& sp = ab.specs[e->first + i];
            Val v;
            if (!value(c, sp.form, sp.implicit, u, &v)) return;
            switch (sp.name) {
            case kAtName: name = v; break;
            case kAtCompDir: dir = v; break;
            case kAtLowPc: low = v; break;
            case kAtHighPc: high = v; break;
            case kAtRanges: ranges = v; break;
            case kAtStmtList: u.stmt_list = v.u; break;
            case kAtLanguage: u.mangled = mangled_language(v.u); break;
            case kAtStrOffsetsBase: u.str_offsets_base = v.u; break;
            case kAtAddrBase:
            case kAtGnuAddrBase: u.addr_base = v.u; break;
            case kAtRnglistsBase: u.rnglists_base = v.u, u.rnglists_base_set = true; break;
            default: break;
            }
        }
        // (The bases may come after what they are needed for: the values
        // are made into strings and addresses once all are read.)
        u.name = string(u, name);
        u.comp_dir = string(u, dir);
        if (low.kind != Val::kNothing) u.low_pc = address(u, low);
        std::vector<std::pair<u64, u64>> code;
        pc_ranges(u, low, high, ranges, &code);
        const u32 index = static_cast<u32>(units.size());
        for (const auto& [lo, hi] : code) unit_ranges.push_back({lo, hi, index});
        if (code.empty() && u.stmt_list != kNone) {
            // No ranges said: those of its line table's sequences.
            Tables t;
            read_lines(u, &t);
            for (const Seq& q : t.seqs) unit_ranges.push_back({q.lo, q.hi, index});
        }
    }

    // A unit's line table: its files, and its rows in sequences.
    void read_lines(const Head& u, Tables* t) const {
        if (u.stmt_list == kNone) return;
        Cursor c{s.line, u.stmt_list, little};
        bool d64 = false;
        u64 len = c.u(4);
        if (len >= 0xfffffff0u) {
            if (len != 0xffffffffu) return;
            d64 = true;
            len = c.u(8);
        }
        if (!c.ok || len > c.left()) return;
        const u64 end = c.at + len;
        c.d = c.d.substr(0, end);
        const u16 version = static_cast<u16>(c.u(2));
        if (version < 2 || version > 5) return;
        if (version >= 5) c.skip(2);  // (address and segment selector sizes)
        const u64 header = offset(c, d64);
        if (!c.ok || header > c.left()) return;
        const u64 program = c.at + header;
        const u32 min_inst = static_cast<u32>(c.u(1));
        const u32 max_ops = version >= 4 ? static_cast<u32>(c.u(1)) : 1;
        const bool default_stmt = c.u(1) != 0;
        (void)default_stmt;
        const i32 line_base = static_cast<std::int8_t>(c.u(1));
        const u32 line_range = static_cast<u32>(c.u(1));
        const u32 opcode_base = static_cast<u32>(c.u(1));
        if (!c.ok || line_range == 0 || opcode_base == 0) return;
        u8 lengths[256] = {};
        for (u32 i = 1; i < opcode_base; ++i) lengths[i] = static_cast<u8>(c.u(1));

        // Its folders and files, made full from the unit's folder.
        std::vector<std::string> dirs;
        if (version >= 5) {
            Head lu = u;  // (the forms' sizes are the line table's)
            lu.dwarf64 = d64;
            auto entries = [&](auto&& take) {
                const u32 nformats = static_cast<u32>(c.u(1));
                std::vector<std::pair<u32, u32>> formats;
                for (u32 i = 0; i < nformats && c.ok; ++i) {
                    const u64 kind = c.uleb(), form = c.uleb();
                    formats.emplace_back(static_cast<u32>(std::min<u64>(kind, 0xffffffffu)), static_cast<u32>(std::min<u64>(form, 0xffffffffu)));
                }
                const u64 count = c.uleb();
                for (u64 i = 0; i < count && c.ok && c.at < program; ++i) {
                    std::string_view path;
                    u64 dir = 0;
                    for (const auto& [kind, form] : formats) {
                        Val v;
                        if (!value(c, form, 0, lu, &v)) return;
                        if (kind == kLnctPath) path = string(lu, v);
                        else if (kind == kLnctDirectoryIndex) dir = v.u;
                    }
                    take(path, dir);
                }
            };
            entries([&](std::string_view path, u64) { dirs.push_back(dirs.empty() ? std::string(path) : join_path(dirs.front(), path)); });
            if (dirs.empty()) dirs.emplace_back(u.comp_dir);
            entries([&](std::string_view path, u64 dir) { t->files.push_back(join_path(dir < dirs.size() ? std::string_view(dirs[dir]) : std::string_view(), path)); });
        } else {
            dirs.emplace_back(u.comp_dir);
            for (;;) {
                const std::string_view d = c.cstr();
                if (!c.ok || d.empty()) break;
                dirs.push_back(join_path(u.comp_dir, d));
            }
            t->files.push_back(join_path(u.comp_dir, u.name));  // (0: the unit's own)
            for (;;) {
                const std::string_view f = c.cstr();
                if (!c.ok || f.empty()) break;
                const u64 dir = c.uleb();
                c.uleb(), c.uleb();  // (its time and size)
                t->files.push_back(join_path(dir < dirs.size() ? std::string_view(dirs[dir]) : std::string_view(), f));
            }
        }
        if (!c.ok) return;

        // The program.
        c.at = program;
        u64 address = 0;
        u32 op_index = 0, file = 1, line = 1, column = 0;
        size_t first = t->rows.size();
        auto row = [&] {
            if (file <= 0xffff) t->rows.push_back({address, line, static_cast<u16>(file), static_cast<u16>(std::min<u32>(column, 0xffff))});
        };
        auto advance = [&](u64 ops) {
            if (max_ops <= 1) {
                address += min_inst * ops;
            } else {
                address += min_inst * ((op_index + ops) / max_ops);
                op_index = static_cast<u32>((op_index + ops) % max_ops);
            }
        };
        auto reset = [&] {
            address = 0, op_index = 0, file = 1, line = 1, column = 0;
        };
        auto end_sequence = [&] {
            const size_t n = t->rows.size() - first;
            if (n) {
                std::stable_sort(t->rows.begin() + static_cast<std::ptrdiff_t>(first), t->rows.end(), [](const Line& a, const Line& b) { return a.addr < b.addr; });
                const u64 lo = t->rows[first].addr;
                if (address > lo) t->seqs.push_back({lo, address, static_cast<u32>(first), static_cast<u32>(n)});
                else t->rows.resize(first);  // (empty, or going backwards: no sequence)
            }
            first = t->rows.size();
            reset();
        };
        while (c.ok && c.at < end) {
            const u32 op = static_cast<u32>(c.u(1));
            if (op >= opcode_base) {  // a special opcode
                const u32 adj = op - opcode_base;
                advance(adj / line_range);
                line = static_cast<u32>(static_cast<i64>(line) + line_base + static_cast<i32>(adj % line_range));
                row();
                continue;
            }
            switch (op) {
            case 0: {  // extended
                const u64 n = c.uleb();
                if (!c.ok || n == 0 || n > c.left()) return;
                const u64 next = c.at + n;
                const u32 ext = static_cast<u32>(c.u(1));
                if (ext == 1) {
                    row();
                    end_sequence();
                } else if (ext == 2) {
                    address = c.u(static_cast<u32>(std::min<u64>(n - 1, 8)));
                    op_index = 0;
                }
                c.at = next;
                break;
            }
            case 1: row(); break;  // copy
            case 2: advance(c.uleb()); break;
            case 3: line = static_cast<u32>(static_cast<i64>(line) + c.sleb()); break;
            case 4: file = static_cast<u32>(std::min<u64>(c.uleb(), 0xffffffffu)); break;
            case 5: column = static_cast<u32>(std::min<u64>(c.uleb(), 0xffffffffu)); break;
            case 6: case 7: case 10: case 11: break;  // (is_stmt, basic block, prologue and epilogue: not kept)
            case 8: advance((255 - opcode_base) / line_range); break;
            case 9: address += c.u(2), op_index = 0; break;
            case 12: c.uleb(); break;  // (the ISA)
            default:
                for (u32 i = 0; i < lengths[op] && c.ok; ++i) c.uleb();
                break;
            }
        }
        t->rows.resize(first);  // (a sequence not ended: dropped)
        std::ranges::sort(t->seqs, {}, &Seq::lo);
    }

    // A unit's functions and the functions inlined into them, each with the
    // ranges of its code and its place in the tree.
    void read_funcs(const Unit& u, Tables* t) const {
        const AbbrevTable& ab = abbrev(u.abbrev_off);
        Cursor c{s.info.substr(0, u.end), u.die, little};
        std::vector<std::pair<u32, u32>> stack;  // (depth, function) of the functions the entries are in
        u32 depth = 0;
        std::vector<std::pair<u64, u64>> code;
        while (c.ok && c.at < u.end) {
            const u64 code_n = c.uleb();
            if (code_n == 0) {
                if (depth) --depth;
                continue;
            }
            const AbbrevTable::Entry* e = ab.find(code_n);
            if (!e) return;  // (damaged: nothing more can be read)
            const bool fn = e->tag == kTagSubprogram || e->tag == kTagInlined;
            Func f;
            Val low, high, ranges;
            for (u32 i = 0; i < e->count; ++i) {
                const AbbrevTable::Spec& sp = ab.specs[e->first + i];
                Val v;
                if (!value(c, sp.form, sp.implicit, u, &v)) return;
                if (!fn) continue;
                switch (sp.name) {
                case kAtName: f.name = string(u, v); break;
                case kAtLinkageName:
                case kAtMipsLinkageName: f.linkage = string(u, v); break;
                case kAtLowPc: low = v; break;
                case kAtHighPc: high = v; break;
                case kAtRanges: ranges = v; break;
                case kAtAbstractOrigin: if (v.kind == Val::kRef) f.origin = v.u; break;
                case kAtSpecification: if (v.kind == Val::kRef) f.spec = v.u; break;
                case kAtCallFile: f.call_file = static_cast<u32>(std::min<u64>(v.u, 0xffffffffu)); break;
                case kAtCallLine: f.call_line = static_cast<u32>(std::min<u64>(v.u, 0xffffffffu)); break;
                case kAtCallColumn: f.call_col = static_cast<u32>(std::min<u64>(v.u, 0xffffffffu)); break;
                default: break;
                }
            }
            while (!stack.empty() && stack.back().first >= depth) stack.pop_back();
            if (fn) {
                code.clear();
                pc_ranges(u, low, high, ranges, &code);
                if (!code.empty()) {
                    f.inlined = e->tag == kTagInlined;
                    f.depth = depth;
                    f.parent = stack.empty() ? ~0u : stack.back().second;
                    f.ranges_first = static_cast<u32>(t->ranges.size());
                    f.ranges_count = static_cast<u32>(code.size());
                    t->ranges.insert(t->ranges.end(), code.begin(), code.end());
                    stack.emplace_back(depth, static_cast<u32>(t->funcs.size()));
                    t->funcs.push_back(f);
                }
            }
            if (e->children) ++depth;
        }
    }

    Tables& tables(Unit& u) const {
        if (!u.tables) {
            auto t = std::make_unique<Tables>();
            read_lines(u, t.get());
            read_funcs(u, t.get());
            u.tables = std::move(t);
        }
        return *u.tables;
    }

    // The unit an offset into .debug_info is in.
    const Unit* unit_at(u64 off) const {
        auto it = std::ranges::upper_bound(units, off, {}, &Unit::offset);
        if (it == units.begin()) return nullptr;
        --it;
        return off >= it->die && off < it->end ? &*it : nullptr;
    }

    // A function's name: its linkage name (demangled) where it or what it
    // stands for (its abstract origin, its specification) has one, else
    // its name.
    std::string name_of(const Func& f, bool* linkage = nullptr) const {
        std::string_view link = f.linkage, name = f.name;
        u64 next = f.origin != kNone ? f.origin : f.spec;
        for (u32 hop = 0; hop < kMaxHops && link.empty() && next != kNone; ++hop) {
            const Unit* u = unit_at(next);
            if (!u) break;
            const AbbrevTable& ab = abbrev(u->abbrev_off);
            Cursor c{s.info.substr(0, u->end), next, little};
            const AbbrevTable::Entry* e = ab.find(c.uleb());
            if (!e) break;
            next = kNone;
            u64 spec = kNone;
            for (u32 i = 0; i < e->count; ++i) {
                const AbbrevTable::Spec& sp = ab.specs[e->first + i];
                Val v;
                if (!value(c, sp.form, sp.implicit, *u, &v)) break;
                if (sp.name == kAtName && name.empty()) name = string(*u, v);
                else if ((sp.name == kAtLinkageName || sp.name == kAtMipsLinkageName) && link.empty()) link = string(*u, v);
                else if (sp.name == kAtAbstractOrigin && v.kind == Val::kRef) next = v.u;
                else if (sp.name == kAtSpecification && v.kind == Val::kRef) spec = v.u;
            }
            if (next == kNone) next = spec;
        }
        if (linkage) *linkage = !link.empty();
        return !link.empty() ? demangled(link) : std::string(name);
    }

    // The row of a unit's line table that holds an address.
    const Line* row_at(const Tables& t, u64 addr) const {
        // (Sequences may overlap: code a linker dropped keeps its rows at 0.)
        auto it = std::ranges::upper_bound(t.seqs, addr, {}, &Seq::lo);
        while (it != t.seqs.begin()) {
            --it;
            if (addr < it->hi) {
                const Line* first = t.rows.data() + it->first;
                const Line* last = first + it->count;
                const Line* r = std::upper_bound(first, last, addr, [](u64 a, const Line& l) { return a < l.addr; });
                if (r != first) return r - 1;
            }
        }
        return nullptr;
    }

    // The innermost function whose code holds an address (the deepest in
    // the tree; of two as deep, the one with less code).
    const Func* func_at(const Tables& t, u64 addr) const {
        const Func* best = nullptr;
        u64 best_size = 0;
        for (const Func& f : t.funcs) {
            for (u32 i = 0; i < f.ranges_count; ++i) {
                const auto& [lo, hi] = t.ranges[f.ranges_first + i];
                if (addr < lo || addr >= hi) continue;
                if (!best || f.depth > best->depth || (f.depth == best->depth && hi - lo < best_size)) best = &f, best_size = hi - lo;
                break;
            }
        }
        return best;
    }
};

AddrSource::AddrSource(std::shared_ptr<const Elf> elf) : elf_(std::move(elf)), impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    m.elf = elf_.get();
    m.little = elf_->little();
    m.s.info = elf_->data(".debug_info");
    m.s.abbrev = elf_->data(".debug_abbrev");
    m.s.str = elf_->data(".debug_str");
    m.s.line = elf_->data(".debug_line");
    m.s.line_str = elf_->data(".debug_line_str");
    m.s.str_offsets = elf_->data(".debug_str_offsets");
    m.s.addr = elf_->data(".debug_addr");
    m.s.ranges = elf_->data(".debug_ranges");
    m.s.rnglists = elf_->data(".debug_rnglists");
    for (const ElfSection& sec : elf_->sections())
        if ((sec.flags & 2) && sec.size) m.loaded.emplace_back(sec.addr, sec.addr + sec.size);  // (SHF_ALLOC)
    std::ranges::sort(m.loaded);
    std::lock_guard lock(m.mu);
    m.read_units();
    std::ranges::sort(m.unit_ranges, {}, &Impl::UnitRange::lo);
    m.maxhi.reserve(m.unit_ranges.size());
    u64 hi = 0;
    for (const Impl::UnitRange& r : m.unit_ranges) m.maxhi.push_back(hi = std::max(hi, r.hi));
}

AddrSource::~AddrSource() = default;

bool AddrSource::has_debug() const { return !impl_->units.empty(); }
size_t AddrSource::units() const { return impl_->units.size(); }

void AddrSource::read_all() const {
    std::lock_guard lock(impl_->mu);
    for (Unit& u : impl_->units) impl_->tables(u);
}

std::vector<SourceFrame> AddrSource::lookup(u64 addr) const {
    const Impl& m = *impl_;
    if (elf_->machine() == 40) addr &= ~1ull;  // (Thumb's bit)
    std::vector<SourceFrame> out;
    const bool loaded = std::ranges::any_of(m.loaded, [&](const std::pair<u64, u64>& r) { return addr >= r.first && addr < r.second; });
    if (!loaded && !m.loaded.empty()) return out;
    std::lock_guard lock(m.mu);
    // The units whose code holds it, the latest starting first.
    auto it = std::ranges::upper_bound(m.unit_ranges, addr, {}, &Impl::UnitRange::lo);
    for (size_t i = static_cast<size_t>(it - m.unit_ranges.begin()); i-- > 0 && m.maxhi[i] > addr;) {
        const Impl::UnitRange& r = m.unit_ranges[i];
        if (addr >= r.hi) continue;
        Unit& u = m.units[r.unit];
        const Tables& t = m.tables(u);
        const Line* row = m.row_at(t, addr);
        const Func* f = m.func_at(t, addr);
        if (!row && !f) continue;
        auto file = [&](u32 n) { return n < t.files.size() ? t.files[n] : std::string(); };
        SourceFrame first;
        if (row) first.path = file(row->file), first.line = row->line, first.col = row->col;
        if (f) {
            // (A function of a language whose names are mangled (C++) that
            // the debug information gives no linkage name: the symbol
            // table's name, as binutils takes it, when a symbol starts where
            // its code does. Of an inlined one, that is the function it was
            // inlined into, binutils' way.)
            bool linkage = false;
            first.function = m.name_of(*f, &linkage);
            if (!linkage && u.mangled && f->ranges_count)
                if (const ElfSymbol* y = elf_->function_at(addr); y && y->value == t.ranges[f->ranges_first].first) first.function = demangled(y->name);
        } else if (const ElfSymbol* y = elf_->function_at(addr)) {
            first.function = demangled(y->name);
            if (first.path.empty()) first.path = y->file, first.unnamed_file = y->unnamed_file;
        }
        out.push_back(std::move(first));
        // Out through what it was inlined into: each at the line of the call.
        for (u32 hops = 0; f && f->inlined && f->parent < t.funcs.size() && hops < 256; ++hops) {
            out.back().inlined = true;
            const Func& p = t.funcs[f->parent];
            SourceFrame g;
            g.function = m.name_of(p);
            g.path = file(f->call_file);
            g.line = f->call_line;
            g.col = f->call_col;
            out.push_back(std::move(g));
            f = &p;
        }
        return out;
    }
    // No debug information there: the symbol table's function.
    if (const ElfSymbol* y = elf_->function_at(addr)) {
        SourceFrame f;
        f.function = demangled(y->name);
        f.path = y->file;
        f.unnamed_file = y->unnamed_file;
        out.push_back(std::move(f));
    }
    return out;
}

std::string demangled(std::string_view name) {
    if (!name.starts_with("_Z") || name.size() > 4096) return std::string(name);
    const std::string n(name);
    int status = 0;
    char* d = abi::__cxa_demangle(n.c_str(), nullptr, nullptr, &status);
    if (!d || status != 0) {
        std::free(d);
        return n;
    }
    std::string out(d);
    std::free(d);
    return out;
}

std::string frame_words(const SourceFrame& f, bool inlined_by) {
    std::string out = inlined_by ? " (inlined by) " : "";
    out += f.function.empty() ? "??" : f.function;
    out += " at ";
    out += f.path.empty() && !f.unnamed_file ? "??" : f.path;  // (a nameless file binutils writes as nothing)
    out += ':';
    out += f.line ? std::to_string(f.line) : f.path.empty() && f.function.empty() ? "0" : "?";
    return out;
}

}  // namespace blit
