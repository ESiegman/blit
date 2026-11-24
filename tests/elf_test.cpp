// The ELF reader and address to source (src/elf.hpp, src/addrsrc.hpp), for
// tests/elf.sh and the benchmarks:
//
//   blit_elf_test lookup ELF [ADDR...]   each address (none: a line each on stdin)
//                                        as `addr2line -pfiaC` prints it
//   blit_elf_test addrs ELF N SEED       every function's start, its middle, and N
//                                        addresses picked at random in the code
//   blit_elf_test info ELF               its header, sections, symbols, build id
//   blit_elf_test damage ELF ROUNDS SEED the ELF cut short at every length and
//                                        damaged at random, each read and asked
//                                        about: it must never read out of bounds
//                                        (the Debug build's sanitizers say) nor
//                                        take more than a budget of memory
//   blit_elf_test bench ELF [N]          the time to open it, to look up N
//                                        addresses, and to read every unit
//   blit_elf_test at ELF FILE LINE [FN]  the first address in the code whose
//                                        innermost frame is that file's line
//                                        (in function FN), for the tests' dumps
#include <sys/resource.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "addrsrc.hpp"
#include "elf.hpp"

using namespace blit;

namespace {

double secs(u64 a, u64 b) { return static_cast<double>(b - a) / 1e9; }

// The most memory the process has had resident.
u64 peak_mb() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<u64>(ru.ru_maxrss) >> 20;
#else
    return static_cast<u64>(ru.ru_maxrss) >> 10;
#endif
}

std::shared_ptr<const Elf> open_elf(const char* path) {
    std::string why;
    auto e = Elf::read_file(path, &why);
    if (!e) {
        std::fprintf(stderr, "%s\n", why.c_str());
        std::exit(2);
    }
    return e;
}

void print_frames(u64 addr, const std::vector<SourceFrame>& frames, u32 width) {
    std::printf("0x%0*" PRIx64 ": ", static_cast<int>(width * 2), addr);
    if (frames.empty()) {
        std::printf("?? ??:0\n");
        return;
    }
    for (size_t i = 0; i < frames.size(); ++i) std::printf("%s\n", frame_words(frames[i], i > 0).c_str());
}

// The executable sections' ranges.
std::vector<std::pair<u64, u64>> code_of(const Elf& e) {
    std::vector<std::pair<u64, u64>> out;
    for (const ElfSection& s : e.sections())
        if ((s.flags & 4) && (s.flags & 2) && s.size) out.emplace_back(s.addr, s.addr + s.size);  // (SHF_EXECINSTR, SHF_ALLOC)
    return out;
}

int damage(const char* path, int rounds, u64 seed) {
    std::string why;
    auto whole = open_elf(path);
    const std::string bytes(whole->bytes());
    const auto code = code_of(*whole);
    std::mt19937_64 rng(seed);
    auto addrs = [&] {
        std::vector<u64> a;
        for (int i = 0; i < 16 && !code.empty(); ++i) {
            const auto& [lo, hi] = code[rng() % code.size()];
            a.push_back(lo + rng() % (hi - lo));
        }
        a.push_back(rng());
        return a;
    };
    auto ask = [&](std::string b) {
        auto e = Elf::read(std::move(b), &why);
        if (!e) return;
        (void)e->build_id();
        (void)e->function_at(rng());
        for (const ElfSection& s : e->sections()) (void)e->data(s);
        AddrSource src(e);
        for (u64 a : addrs()) (void)src.lookup(a);
    };
    // Cut short: at every length up to 4 KB (the headers), then in steps.
    for (size_t n = 0; n < bytes.size(); n += n < 4096 ? 1 : 1 + bytes.size() / 512) ask(bytes.substr(0, n));
    // Damaged: bytes changed at random, in the headers and the debug sections.
    std::vector<std::pair<u64, u64>> spans = {{0, std::min<u64>(bytes.size(), 4096)}};
    for (const ElfSection& s : whole->sections())
        if (s.name.starts_with(".debug_") || s.name == ".symtab" || s.name == ".strtab") spans.emplace_back(s.offset, s.offset + s.size);
    for (int r = 0; r < rounds; ++r) {
        std::string b = bytes;
        const int changes = 1 + static_cast<int>(rng() % 16);
        for (int k = 0; k < changes; ++k) {
            const auto& [lo, hi] = spans[rng() % spans.size()];
            if (hi <= lo || hi > b.size()) continue;
            const u64 at = lo + rng() % (hi - lo);
            switch (rng() % 4) {
            case 0: b[at] = static_cast<char>(rng()); break;
            case 1: b[at] = static_cast<char>(0xff); break;
            case 2: b[at] = static_cast<char>(0x80); break;  // (a LEB128 that goes on)
            default: b[at] = 0; break;
            }
        }
        ask(std::move(b));
    }
    std::printf("damage ok: %s, %d rounds, peak %" PRIu64 " MB\n", path, rounds, peak_mb());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: blit_elf_test lookup|addrs|info|damage|bench ELF ...\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "damage") return damage(argv[2], argc > 3 ? std::atoi(argv[3]) : 2000, argc > 4 ? std::strtoull(argv[4], nullptr, 0) : 1);
    const u64 t0 = now_ns();
    auto e = open_elf(argv[2]);
    if (mode == "info") {
        std::printf("%s %s %s, type %u, entry 0x%" PRIx64 ", %zu sections, %zu segments, %zu symbols, build id %s\n", e->wide() ? "ELF64" : "ELF32",
                    e->little() ? "little-endian" : "big-endian", e->machine_name().c_str(), e->type(), e->entry(), e->sections().size(), e->segments().size(),
                    e->symbols().size(), e->build_id().empty() ? "none" : e->build_id().c_str());
        AddrSource src(e);
        std::printf("%zu units\n", src.units());
        return 0;
    }
    if (mode == "addrs") {
        const int n = argc > 3 ? std::atoi(argv[3]) : 200;
        std::mt19937_64 rng(argc > 4 ? std::strtoull(argv[4], nullptr, 0) : 1);
        const auto code = code_of(*e);
        auto in_code = [&](u64 a) {
            for (const auto& [lo, hi] : code)
                if (a >= lo && a < hi) return true;
            return false;
        };
        // (Of a program with thousands of functions, 3000 of them at random.)
        std::vector<const ElfSymbol*> fns;
        for (const ElfSymbol& y : e->symbols())
            if (y.type == 2 && in_code(y.value)) fns.push_back(&y);
        if (fns.size() > 3000) {
            std::shuffle(fns.begin(), fns.end(), rng);
            fns.resize(3000);
        }
        for (const ElfSymbol* y : fns) {
            std::printf("0x%" PRIx64 "\n", y->value);
            if (y->size > 2) std::printf("0x%" PRIx64 "\n", static_cast<u64>((y->value + y->size / 2) & ~1ull));
        }
        for (int i = 0; i < n && !code.empty(); ++i) {
            const auto& [lo, hi] = code[rng() % code.size()];
            std::printf("0x%" PRIx64 "\n", static_cast<u64>((lo + rng() % (hi - lo)) & ~1ull));
        }
        return 0;
    }
    if (mode == "lookup") {
        AddrSource src(e);
        for (int i = 3; i < argc; ++i) {
            const u64 a = std::strtoull(argv[i], nullptr, 0);
            print_frames(a, src.lookup(a), e->address_size());
        }
        if (argc == 3) {  // (none given: a line each on stdin, as addr2line takes them)
            char line[64];
            while (std::fgets(line, sizeof line, stdin)) {
                const u64 a = std::strtoull(line, nullptr, 0);
                print_frames(a, src.lookup(a), e->address_size());
            }
        }
        return 0;
    }
    if (mode == "at" && argc > 4) {
        AddrSource src(e);
        const std::string file = argv[3];
        const u32 line = static_cast<u32>(std::atoi(argv[4]));
        const std::string fn = argc > 5 ? argv[5] : "";
        for (const auto& [lo, hi] : code_of(*e))
            for (u64 a = lo; a < hi; a += e->machine() == 40 ? 2 : 1) {
                const auto f = src.lookup(a);
                if (f.empty() || f[0].line != line || (!fn.empty() && f[0].function != fn)) continue;
                if (f[0].path.size() < file.size() || f[0].path.compare(f[0].path.size() - file.size(), file.size(), file) != 0) continue;
                std::printf("0x%" PRIx64 "\n", a);
                return 0;
            }
        std::fprintf(stderr, "no address at %s:%u\n", file.c_str(), line);
        return 1;
    }
    if (mode == "bench") {
        const u64 t1 = now_ns();
        AddrSource src(e);
        const u64 t2 = now_ns();
        const int n = argc > 3 ? std::atoi(argv[3]) : 1000;
        const auto code = code_of(*e);
        std::mt19937_64 rng(7);
        size_t frames = 0;
        const u64 t3 = now_ns();
        for (int i = 0; i < n && !code.empty(); ++i) {
            const auto& [lo, hi] = code[rng() % code.size()];
            frames += src.lookup(lo + rng() % (hi - lo)).size();
        }
        AddrSource fresh(e);  // (every unit's tables read, none of them read before)
        const u64 t4 = now_ns();
        fresh.read_all();
        const u64 t5 = now_ns();
        std::printf("%s: %.1f MB, %zu units; read %.1f ms, units indexed %.1f ms, %d lookups %.1f ms (%zu frames), every unit's tables %.1f ms, peak %" PRIu64 " MB\n",
                    argv[2], static_cast<double>(e->bytes().size()) / 1e6, src.units(), secs(t0, t1) * 1e3, secs(t1, t2) * 1e3, n, secs(t3, t4) * 1e3, frames,
                    secs(t4, t5) * 1e3, peak_mb());
        return 0;
    }
    std::fprintf(stderr, "no mode %s\n", mode.c_str());
    return 2;
}
