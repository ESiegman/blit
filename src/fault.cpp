// The fault report (fault.hpp). The tables, and where each is from:
//
// Cortex-M:
//   ARMv7-M Architecture Reference Manual (DDI 0403E.e):
//     B1.5.6 Exception entry behavior (the frame stacked: R0-R3, R12, LR,
//       ReturnAddress, xPSR; 8 words, or 26 with the FP state; xPSR bit 9
//       says a word of padding was put in for 8-byte alignment)
//     B1.5.8 Exception return behavior (EXC_RETURN: bit 2 the stack, bit 3
//       thread or handler mode, bit 4 clear for the FP frame)
//     B3.2 System Control Space, its summary of the SCB's registers:
//       CPUID 0xE000ED00, CFSR 0xE000ED28 (MMFSR, BFSR, UFSR), HFSR
//       0xE000ED2C, MMFAR 0xE000ED34, BFAR 0xE000ED38, AFSR 0xE000ED3C,
//       ID_PFR1 0xE000ED44; and each register's own description there,
//       its bits as named below
//   ARMv8-M Architecture Reference Manual (DDI 0553): the registers'
//     descriptions in its register chapter (CFSR's STKOF; SFSR 0xE000EDE4
//     and SFAR 0xE000EDE8; ID_PFR1.Security, bits 7:4, non-zero with the
//     Security Extension), and its exception model's EXC_RETURN (ES bit 0,
//     DCRS bit 5, S bit 6) and stack frames (the additional state context,
//     ten words below R0, when DCRS is 0)
//   ARMv6-M Architecture Reference Manual (DDI 0419): its SCB has no
//     CFSR, HFSR or fault address registers; every fault is a HardFault
//   Cortex-M3 and M4 Technical Reference Manuals: ACTLR (0xE000E008)
//     bit 1 DISDEFWBUF, the write buffer off: bus faults precise
//   Each core's TRM: CPUID's PARTNO (bits 15:4), as named in kParts
//
// RISC-V: The RISC-V Instruction Set Manual, Volume II: Privileged
//   Architecture, version 20211203: its table "Machine cause register
//   (mcause) values after trap" (the codes named below), and the
//   descriptions of mepc, mtval (the address for misaligned, access and
//   page faults; the instruction's bits, or 0, for an illegal one), mtvec
//   (BASE and MODE) and mstatus (MPP)
//
// Xtensa, ESP-IDF v6.1.1:
//   components/xtensa/include/xtensa/corebits.h:45-86 EXCCAUSE's codes
//     (the words below are its comments'), :135-144 DEBUGCAUSE's bits
//   components/esp_system/port/arch/xtensa/panic_arch.c:228-238 the names
//     ESP-IDF's panic message gives each code ("StoreProhibited"), and
//     :282-291 its own causes (PANIC_RSN_*, components/xtensa/include/
//     esp_private/panic_reason.h:3-11) that panicHandler's frame carries
//   components/xtensa/include/xtensa_context.h:123-144 XtExcFrame (exit,
//     pc, ps, a0-a15, sar, exccause, excvaddr: a word each)
//   components/riscv/include/riscv/rvruntime-frames.h:48-84 RvExcFrame
//     (mepc, ra, sp, ... mstatus, mtvec, mcause, mtval: a word each)
//   components/esp_system/port/panic_handler.c:298-317 panicHandler and
//     xt_unhandled_exception, each given the frame as `frame`
//   components/esp_system/panic.c:381-406 with a debugger attached
//     (CONFIG_ESP_DEBUG_OCDAWARE), the panic handler sets hardware
//     breakpoint 0 at the faulting instruction (IBREAKA0) and returns: the
//     core runs it again and stops there, EXCCAUSE and EXCVADDR as the
//     exception left them; on a RISC-V chip, components/esp_hw_support/
//     cpu.c:73-100, it asks the debugger to set that breakpoint
//     (ESP_SEMIHOSTING_SYS_BREAKPOINT_SET): a trigger, dcsr.cause 2 when it
//     stops there (RISC-V External Debug Support 0.13.2, dcsr), mepc the PC
#include "fault.hpp"

#include <algorithm>
#include <cstdio>

namespace blit {

namespace {

std::string hex8(u64 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08llx", static_cast<unsigned long long>(v & 0xFFFFFFFFu));
    return b;
}

std::string base_of(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

bool reg(const FaultFacts& f, std::string_view name, u64* v) {
    const auto it = f.regs.find(name);
    if (it == f.regs.end()) return false;
    *v = it->second & 0xFFFFFFFFu;
    return true;
}

i64 word(const std::vector<i64>& w, size_t i) { return i < w.size() ? w[i] : -1; }

// An address's place: "motor.c:212", else its function, else "".
std::string place(const std::map<u64, SourceFrame>& where, u64 addr) {
    const auto it = where.find(addr);
    if (it == where.end()) return {};
    const SourceFrame& s = it->second;
    if (!s.path.empty() && s.line) return base_of(s.path) + ":" + std::to_string(s.line);
    return s.function;
}

// Its function and place, for the report: "pid_step, motor.c:212".
std::string named(const std::map<u64, SourceFrame>& where, u64 addr) {
    const auto it = where.find(addr);
    if (it == where.end()) return {};
    const SourceFrame& s = it->second;
    std::string out = s.function;
    if (!s.path.empty() && s.line) out += (out.empty() ? "" : ", ") + base_of(s.path) + ":" + std::to_string(s.line);
    return out;
}

// A bit's words, short, for the Debug tab's line: up to the first colon or parenthesis.
std::string short_words(std::string_view w) {
    return std::string(w.substr(0, std::min(w.find(": "), w.find(" ("))));
}

struct Bit {
    u32 mask;
    const char* name;
    const char* words;
    u8 fault;  // the exception whose status it is (4 MemManage, 5 BusFault, 6 UsageFault); 0 for an address's being valid
};

// CFSR (MMFSR, bits 7:0; BFSR, 15:8; UFSR, 31:16).
constexpr Bit kCfsr[] = {
    {1u << 0, "IACCVIOL", "instruction access violation: a fetch from where the MPU allows none, or from execute-never memory", 4},
    {1u << 1, "DACCVIOL", "data access violation: a load or store the MPU does not allow", 4},
    {1u << 3, "MUNSTKERR", "MemManage fault unstacking on a return from an exception", 4},
    {1u << 4, "MSTKERR", "MemManage fault stacking on entry to the exception", 4},
    {1u << 5, "MLSPERR", "MemManage fault saving the FP state lazily", 4},
    {1u << 7, "MMARVALID", "MMFAR holds the address that faulted", 0},
    {1u << 8, "IBUSERR", "bus error fetching an instruction", 5},
    {1u << 9, "PRECISERR", "precise data bus error: the stacked PC is the instruction that faulted", 5},
    {1u << 10, "IMPRECISERR", "imprecise data bus error: the stacked PC is after the instruction that faulted", 5},
    {1u << 11, "UNSTKERR", "bus error unstacking on a return from an exception", 5},
    {1u << 12, "STKERR", "bus error stacking on entry to the exception", 5},
    {1u << 13, "LSPERR", "bus error saving the FP state lazily", 5},
    {1u << 15, "BFARVALID", "BFAR holds the address that faulted", 0},
    {1u << 16, "UNDEFINSTR", "undefined instruction", 6},
    {1u << 17, "INVSTATE", "invalid state: an instruction run with EPSR.T clear (a branch to an even address) or a bad IT state", 6},
    {1u << 18, "INVPC", "invalid PC on an exception return: a bad EXC_RETURN, or a stacked state that does not match it", 6},
    {1u << 19, "NOCP", "no coprocessor: an FP instruction with the FPU off (CPACR) or absent", 6},
    {1u << 20, "STKOF", "stack overflow: the stack pointer went below its limit (MSPLIM or PSPLIM)", 6},
    {1u << 24, "UNALIGNED", "unaligned access (CCR.UNALIGN_TRP, or an instruction that never takes one)", 6},
    {1u << 25, "DIVBYZERO", "divide by zero (CCR.DIV_0_TRP)", 6},
};

constexpr Bit kHfsr[] = {
    {1u << 1, "VECTTBL", "bus error reading the vector table", 3},
    {1u << 30, "FORCED", "forced: a configurable fault escalated to HardFault (its handler disabled, or no higher in priority than what ran)", 3},
    {1u << 31, "DEBUGEVT", "a debug event (BKPT) with halting debug off", 3},
};

constexpr Bit kSfsr[] = {
    {1u << 0, "INVEP", "invalid entry point: non-secure code called secure code where there is no SG instruction", 7},
    {1u << 1, "INVIS", "invalid integrity signature unstacking", 7},
    {1u << 2, "INVER", "invalid exception return", 7},
    {1u << 3, "AUVIOL", "attribution unit violation: non-secure access to secure memory", 7},
    {1u << 4, "INVTRAN", "invalid transition: secure code branched to non-secure code without BXNS or BLXNS", 7},
    {1u << 5, "LSPERR", "fault saving the FP state lazily", 7},
    {1u << 6, "SFARVALID", "SFAR holds the address that faulted", 0},
    {1u << 7, "LSERR", "lazy state error: the FP state was not as the return expected", 7},
};

const char* exception_name(u32 n) {
    switch (n) {
        case 3: return "HardFault";
        case 4: return "MemManage";
        case 5: return "BusFault";
        case 6: return "UsageFault";
        case 7: return "SecureFault";
        default: return nullptr;
    }
}

// The fault handlers by name (CMSIS's, the Pico SDK's, libopencm3's): the exception each is for.
u32 handler_exception(std::string_view name) {
    struct Named {
        const char* name;
        u32 n;
    };
    static constexpr Named kNames[] = {
        {"HardFault_Handler", 3}, {"MemManage_Handler", 4}, {"BusFault_Handler", 5}, {"UsageFault_Handler", 6}, {"SecureFault_Handler", 7},
        {"isr_hardfault", 3},     {"isr_memmanage", 4},     {"isr_busfault", 5},     {"isr_usagefault", 6},     {"isr_securefault", 7},
        {"hard_fault_handler", 3}, {"mem_manage_handler", 4}, {"bus_fault_handler", 5}, {"usage_fault_handler", 6}, {"secure_fault_handler", 7},
    };
    for (const Named& n : kNames)
        if (name == n.name) return n.n;
    return 0;
}

struct Part {
    u32 partno;
    const char* name;
    bool v8;
};
constexpr Part kParts[] = {
    {0xC20, "Cortex-M0", false},  {0xC60, "Cortex-M0+", false}, {0xC21, "Cortex-M1", false}, {0xC23, "Cortex-M3", false}, {0xC24, "Cortex-M4", false},
    {0xC27, "Cortex-M7", false},  {0xD20, "Cortex-M23", true},  {0xD21, "Cortex-M33", true}, {0xD22, "Cortex-M55", true}, {0xD23, "Cortex-M85", true},
    {0xD24, "Cortex-M52", true},  {0xD31, "Cortex-M35P", true},
};

const Part* part_of(u32 cpuid) {
    if ((cpuid >> 24) != 0x41) return nullptr;  // (Arm's own: another implementer's numbers are its own)
    for (const Part& p : kParts)
        if (p.partno == ((cpuid >> 4) & 0xFFF)) return &p;
    return nullptr;
}

// The core in words: "Cortex-M3 r2p1", else "a Cortex-M core (CPUID 0x...)".
std::string core_words(u32 cpuid) {
    if (const Part* p = part_of(cpuid)) return std::string(p->name) + " r" + std::to_string((cpuid >> 20) & 0xF) + "p" + std::to_string(cpuid & 0xF);
    return "a Cortex-M core (CPUID " + hex8(cpuid) + ")";
}

bool is_v8(u32 cpuid) {
    const Part* p = part_of(cpuid);
    return p && p->v8;
}

// ---- RISC-V: mcause's codes (the privileged specification, Table 3.6)

const char* rv_exception(u64 code) {
    switch (code) {
        case 0: return "Instruction address misaligned";
        case 1: return "Instruction access fault";
        case 2: return "Illegal instruction";
        case 3: return "Breakpoint";
        case 4: return "Load address misaligned";
        case 5: return "Load access fault";
        case 6: return "Store/AMO address misaligned";
        case 7: return "Store/AMO access fault";
        case 8: return "Environment call from U-mode";
        case 9: return "Environment call from S-mode";
        case 11: return "Environment call from M-mode";
        case 12: return "Instruction page fault";
        case 13: return "Load page fault";
        case 15: return "Store/AMO page fault";
        default: return nullptr;
    }
}

const char* rv_interrupt(u64 code) {
    switch (code) {
        case 1: return "Supervisor software interrupt";
        case 3: return "Machine software interrupt";
        case 5: return "Supervisor timer interrupt";
        case 7: return "Machine timer interrupt";
        case 9: return "Supervisor external interrupt";
        case 11: return "Machine external interrupt";
        default: return nullptr;
    }
}

// mtval holds the address that faulted for these.
bool rv_address_cause(u64 code) { return code <= 1 || (code >= 4 && code <= 7) || code == 12 || code == 13 || code == 15; }

// ---- Xtensa: EXCCAUSE's codes (corebits.h), with panic_arch.c's names

struct Cause {
    u32 code;
    const char* name;
    const char* words;
    bool address;  // EXCVADDR holds the address it is about
};
constexpr Cause kXtCauses[] = {
    {0, "IllegalInstruction", "illegal instruction", false},
    {1, "Syscall", "system call (SYSCALL instruction)", false},
    {2, "InstructionFetchError", "instruction fetch error", true},
    {3, "LoadStoreError", "load or store error", true},
    {4, "Level1Interrupt", "level 1 interrupt", false},
    {5, "Alloca", "stack extension assist (MOVSP instruction) for alloca", false},
    {6, "IntegerDivideByZero", "integer divide by zero", false},
    {7, "PCValue", "next PC value illegal", false},
    {8, "Privileged", "privileged instruction", false},
    {9, "LoadStoreAlignment", "unaligned load or store", true},
    {12, "InstrPDAddrError", "PIF data error on instruction fetch", true},
    {13, "LoadStorePIFDataError", "PIF data error on load or store", true},
    {14, "InstrPIFAddrError", "PIF address error on instruction fetch", true},
    {15, "LoadStorePIFAddrError", "PIF address error on load or store", true},
    {16, "InstTLBMiss", "ITLB miss (no ITLB entry matches, hardware refill also missed)", true},
    {17, "InstTLBMultiHit", "ITLB multihit (multiple ITLB entries match)", true},
    {18, "InstFetchPrivilege", "ring privilege violation on instruction fetch", true},
    {20, "InstrFetchProhibited", "cache attribute does not allow instruction fetch", true},
    {24, "LoadStoreTLBMiss", "DTLB miss (no DTLB entry matches, hardware refill also missed)", true},
    {25, "LoadStoreTLBMultihit", "DTLB multihit (multiple DTLB entries match)", true},
    {26, "LoadStorePrivilege", "ring privilege violation on load or store", true},
    {28, "LoadProhibited", "cache attribute does not allow load", true},
    {29, "StoreProhibited", "cache attribute does not allow store", true},
    {32, "Cp0Dis", "access to coprocessor 0 when disabled", false},
    {33, "Cp1Dis", "access to coprocessor 1 when disabled", false},
    {34, "Cp2Dis", "access to coprocessor 2 when disabled", false},
    {35, "Cp3Dis", "access to coprocessor 3 when disabled", false},
    {36, "Cp4Dis", "access to coprocessor 4 when disabled", false},
    {37, "Cp5Dis", "access to coprocessor 5 when disabled", false},
    {38, "Cp6Dis", "access to coprocessor 6 when disabled", false},
    {39, "Cp7Dis", "access to coprocessor 7 when disabled", false},
};

const Cause* xt_cause(u64 code) {
    for (const Cause& c : kXtCauses)
        if (c.code == code) return &c;
    return nullptr;
}

// ESP-IDF's own causes, in panicHandler's frame (panic_arch.c:282-291).
const char* xt_pseudo(u64 code) {
    static constexpr const char* kPseudo[] = {"Unknown reason", "Unhandled debug exception", "Double exception", "Unhandled kernel exception",
                                              "Coprocessor exception", "Interrupt wdt timeout on CPU0", "Interrupt wdt timeout on CPU1", "Cache error"};
    return code < std::size(kPseudo) ? kPseudo[code] : nullptr;
}

// Where a windowed call returns to: a0's low 30 bits, the top two (the
// call's window increment, in a0) those of the code it is in (the Xtensa
// ISA's windowed register ABI; ESP-IDF's esp_cpu_process_stack_pc does as much).
u64 xt_return(u64 a0, u64 pc) { return (a0 & 0x3FFFFFFFu) | (pc & 0xC0000000u); }

// A fault, not a trap the program means: these causes are its own doing.
bool xt_fault(u64 code) { return code != 1 && code != 4 && code != 5; }

// The report as it is put together.
struct Out {
    FaultReport r;
    void add(std::string text, u8 style = 0, u64 addr = 0) { r.lines.push_back({std::move(text), style, addr}); }
    void heading(const char* text) {
        if (!r.lines.empty()) add({});
        add(text, 3);
    }
    // A register's line: its name padded, its value, then what more there is.
    void value(const char* name, u64 v, const std::string& more = {}, u64 addr = 0) {
        std::string t = "  " + std::string(name);
        t.resize(std::max<size_t>(t.size() + 1, 14), ' ');
        t += hex8(v);
        if (!more.empty()) t += "  " + more;
        add(std::move(t), 0, addr);
    }
    // A register's line with its value in decimal (a cause's code).
    void number(const char* name, u64 v, const std::string& more) {
        std::string t = "  " + std::string(name);
        t.resize(std::max<size_t>(t.size() + 1, 14), ' ');
        t += std::to_string(v);
        if (!more.empty()) t += "  " + more;
        add(std::move(t), 0, 0);
    }
    void bits(const Bit* table, size_t n, u32 v) {
        for (size_t i = 0; i < n; ++i)
            if (v & table[i].mask) {
                std::string t = "      " + std::string(table[i].name);
                if (table[i].fault >= 3 && table[i].fault <= 7) t += " (" + std::string(exception_name(table[i].fault)) + ")";
                add(t + ": " + table[i].words, 0);
            }
    }
};

// ---- Cortex-M's report

FaultReport cm_report(const FaultFacts& f, const std::map<u64, SourceFrame>& where) {
    Out o;
    u64 xpsr = 0;
    const bool have_xpsr = reg(f, "xpsr", &xpsr);
    u32 n = have_xpsr ? static_cast<u32>(xpsr & 0x1FF) : 0;
    if (!exception_name(n)) n = f.frames.empty() ? 0 : handler_exception(f.frames[0]);
    if (!exception_name(n)) return {};
    o.r.fault = true;
    const i64 cpuid = word(f.scb, 0);
    const bool status = cpuid >= 0 && fault_cm_has_status(static_cast<u32>(cpuid));
    const bool v8 = cpuid >= 0 && is_v8(static_cast<u32>(cpuid));
    const i64 cfsr = word(f.scb, 0x28 / 4), hfsr = word(f.scb, 0x2C / 4), mmfar = word(f.scb, 0x34 / 4), bfar = word(f.scb, 0x38 / 4), afsr = word(f.scb, 0x3C / 4);
    const i64 id_pfr1 = word(f.scb, 0x44 / 4);
    const bool security = status && id_pfr1 >= 0 && fault_cm_has_security(static_cast<u32>(cpuid), static_cast<u32>(id_pfr1));

    // What the fault was, in a few words: the bits of its status (the
    // first in words, the rest by name), and the address where valid.
    auto status_words = [&](u32 of) {
        std::string out;
        if (cfsr < 0) return out;
        int said = 0;
        for (const Bit& b : kCfsr)
            if ((static_cast<u32>(cfsr) & b.mask) && b.fault == of) {
                out += (said ? ", " : "") + std::string(b.name) + (said ? "" : ", " + short_words(b.words));
                ++said;
            }
        const u32 c = static_cast<u32>(cfsr);
        if (of == 4 && (c & (1u << 7)) && mmfar >= 0) out += " at " + hex8(static_cast<u64>(mmfar));
        if (of == 5 && (c & (1u << 15)) && bfar >= 0) out += " at " + hex8(static_cast<u64>(bfar));
        return out;
    };
    std::string what = exception_name(n);
    if (!status && cpuid >= 0) {
        what += " (" + std::string(part_of(static_cast<u32>(cpuid)) ? part_of(static_cast<u32>(cpuid))->name : "this core") + ": no fault status registers)";
    } else if (n == 3 && hfsr >= 0 && (hfsr & (1ll << 30))) {
        std::string from;
        for (const u32 of : {4u, 5u, 6u})
            if (const std::string w = status_words(of); !w.empty() && from.empty()) from = std::string(exception_name(of)) + ": " + w;
        if (security && f.sfsr > 0 && from.empty()) from = "SecureFault";
        what += from.empty() ? ": FORCED, a configurable fault escalated" : ", forced from " + from;
    } else if (n == 3 && hfsr >= 0 && (hfsr & 2)) {
        what += ": VECTTBL, bus error reading the vector table";
    } else if (n == 7 && f.sfsr >= 0) {
        std::string w;
        for (const Bit& b : kSfsr)
            if ((static_cast<u32>(f.sfsr) & b.mask) && b.fault) w += w.empty() ? std::string(b.name) + ", " + short_words(b.words) : ", " + std::string(b.name);
        if ((f.sfsr & (1 << 6)) && f.sfar >= 0) w += " at " + hex8(static_cast<u64>(f.sfar));
        if (!w.empty()) what += ": " + w;
    } else if (n >= 4 && n <= 6) {
        if (const std::string w = status_words(n); !w.empty()) what += ": " + w;
    }

    o.r.title = what;
    if (!status && cpuid >= 0) o.add(std::string(exception_name(n)) + "  (" + core_words(static_cast<u32>(cpuid)) + ": no fault status registers)", 2);
    else o.add(what + (cpuid >= 0 ? "  (" + core_words(static_cast<u32>(cpuid)) + ")" : std::string()), 2);

    // The status registers.
    if (status) {
        o.heading("Fault status");
        if (cfsr >= 0) {
            o.value("CFSR", static_cast<u64>(cfsr), cfsr ? std::string() : "none set");
            o.bits(kCfsr, std::size(kCfsr), static_cast<u32>(cfsr));
        }
        if (hfsr >= 0) {
            o.value("HFSR", static_cast<u64>(hfsr), hfsr ? std::string() : "none set");
            o.bits(kHfsr, std::size(kHfsr), static_cast<u32>(hfsr));
        }
        if (mmfar >= 0) o.value("MMFAR", static_cast<u64>(mmfar), cfsr >= 0 && (cfsr & (1 << 7)) ? "the address of the MemManage fault" : "not valid (MMARVALID is clear)");
        if (bfar >= 0) o.value("BFAR", static_cast<u64>(bfar), cfsr >= 0 && (cfsr & (1 << 15)) ? "the address of the bus fault" : "not valid (BFARVALID is clear)");
        if (afsr >= 0) o.value("AFSR", static_cast<u64>(afsr), "the implementation's own");
        if (security && f.sfsr >= 0) {
            o.value("SFSR", static_cast<u64>(f.sfsr), f.sfsr ? std::string() : "none set");
            o.bits(kSfsr, std::size(kSfsr), static_cast<u32>(f.sfsr));
            if (f.sfar >= 0) o.value("SFAR", static_cast<u64>(f.sfar), (f.sfsr & (1 << 6)) ? "the address of the SecureFault" : "not valid (SFARVALID is clear)");
        }
    } else if (cpuid >= 0) {
        o.heading("Fault status");
        o.add("  ARMv6-M and v8-M Baseline have no CFSR, HFSR or fault address registers: every fault is a HardFault, and the stacked frame is all there is.", 4);
    }

    // EXC_RETURN and the stacked frame.
    u32 exc = 0;
    const bool have_exc = fault_cm_exc_return(f, &exc);
    std::string stack_words, fpu_words;
    o.heading("Exception return");
    if (!have_exc) {
        o.add("  EXC_RETURN was not found: LR is no longer it, and the debugger did not unwind through the exception", 4);
    } else {
        const bool psp = exc & 4, thread = exc & 8, fpu = !(exc & 16);
        stack_words = !thread ? "handler stack (MSP)" : psp ? "thread stack (PSP)" : "main stack (MSP)";
        fpu_words = fpu ? "FPU frame" : "no FPU frame";
        std::string more = std::string(thread ? "thread mode" : "handler mode (a fault in an exception handler)") + ", " + (psp ? "process stack (PSP)" : "main stack (MSP)") + ", " +
                           (fpu ? "FPU frame (26 words)" : "no FPU frame (8 words)");
        if (v8) {
            more += std::string(exc & 64 ? "; secure stack" : "; non-secure stack") + (exc & 1 ? ", secure exception" : ", non-secure exception");
            if (!(exc & 32)) more += "; the callee registers stacked too (ten words below R0)";
        }
        o.value("EXC_RETURN", exc, more);
        u64 r0 = 0;
        bool exact = true;
        if (fault_cm_frame_at(f, &r0, &exact)) {
            std::string at = "  stacked at " + hex8(r0) + " (" + (psp ? "PSP" : exact ? "MSP at the exception's entry" : "MSP as it is now") + ")";
            o.add(at, 0);
            if (!exact) o.add("  (the debugger did not unwind through the exception: the frame is there only if the handler has pushed nothing since)", 4);
        }
    }

    // The stacked frame.
    u64 pc = 0;
    if (have_exc && !f.stacked.empty() && word(f.stacked, 0) >= 0) {
        o.heading("Stacked frame");
        static constexpr const char* kNames[] = {"R0", "R1", "R2", "R3", "R12", "LR", "PC", "xPSR"};
        for (size_t i = 0; i < 8; ++i) {
            const i64 w = word(f.stacked, i);
            if (w < 0) continue;
            const u64 v = static_cast<u64>(w);
            if (i == 5) {
                const u64 at = v & ~u64{1};
                o.value(kNames[i], v, named(where, at), (v >> 24) == 0xFF ? 0 : at);
            } else if (i == 6) {
                pc = v;
                o.value(kNames[i], v, named(where, v), v);
            } else if (i == 7) {
                const u32 ipsr = static_cast<u32>(v & 0x1FF);
                std::string more = ipsr == 0 ? "thread mode" : exception_name(ipsr) ? "in " + std::string(exception_name(ipsr)) : "in exception " + std::to_string(ipsr);
                if (!(v & (1u << 24))) more += "; T clear";
                if (v & (1u << 9)) more += "; a word of padding above the frame";
                o.value(kNames[i], v, more);
            } else {
                o.value(kNames[i], v);
            }
        }
        if (const i64 x = word(f.stacked, 7); x >= 0) {
            u64 r0 = 0;
            bool exact = true;
            if (fault_cm_frame_at(f, &r0, &exact)) {
                const u64 sp = r0 + ((exc & 16) ? 32 : 104) + ((x & (1 << 9)) ? 4 : 0);
                o.value("SP before", sp, "the stack pointer before the exception");
            }
        }
        if (cfsr >= 0 && (cfsr & ((1 << 4) | (1 << 12)))) o.add("  The core could not stack the frame (MSTKERR or STKERR): what is read there is not the program's.", 4);
    }

    // The imprecise bus fault's remark (research expectation 13).
    const bool imprecise = cfsr >= 0 && (cfsr & (1 << 10));
    const bool m3m4 = cpuid >= 0 && fault_cm_has_disdefwbuf(static_cast<u32>(cpuid));
    if (imprecise && m3m4) {
        o.heading("Imprecise bus fault");
        o.add("  The write buffer let the program run on before the error came back: the store that faulted is before the stacked PC.", 0);
        if (f.actlr >= 0 && (f.actlr & 2)) o.add("  ACTLR.DISDEFWBUF is set already (ACTLR " + hex8(static_cast<u64>(f.actlr)) + ").", 0);
        else o.add("  Setting ACTLR.DISDEFWBUF (bit 1 of 0xE000E008) turns the write buffer off on Cortex-M3 and M4, which makes such faults precise, at a cost in speed.", 0);
    }

    // The Debug tab's line.
    std::string line = what;
    if (!stack_words.empty()) line += "; " + stack_words + "; " + fpu_words;
    if (pc) {
        line += "; stacked PC " + hex8(pc);
        if (const std::string p = place(where, pc); !p.empty()) line += " in " + p;
    }
    o.r.line = std::move(line);
    o.r.pc = pc;
    return o.r;
}

// ---- RISC-V's report

FaultReport rv_report(const FaultFacts& f, const std::map<u64, SourceFrame>& where) {
    Out o;
    u64 mcause = 0, mepc = 0, mtval = 0, mtvec = 0, mstatus = 0, pc = 0;
    // (A panic frame whose sp is 0 is none: read before the panic handler had it.)
    const bool panic = f.panic_frame && word(f.panic_words, 35) >= 0 && word(f.panic_words, 2) > 0;
    bool have = false;
    if (panic) {  // (RvExcFrame: mepc 0, ..., mstatus 32, mtvec 33, mcause 34, mtval 35)
        mepc = static_cast<u64>(word(f.panic_words, 0));
        mstatus = static_cast<u64>(word(f.panic_words, 32));
        mtvec = static_cast<u64>(word(f.panic_words, 33));
        mcause = static_cast<u64>(word(f.panic_words, 34));
        mtval = static_cast<u64>(word(f.panic_words, 35));
        have = true;
    } else {
        have = reg(f, "mcause", &mcause) && reg(f, "mepc", &mepc);
        reg(f, "mtval", &mtval);
        reg(f, "mtvec", &mtvec);
        reg(f, "mstatus", &mstatus);
    }
    reg(f, "pc", &pc);
    if (!have) return {};
    const bool interrupt = mcause >> 31;
    const u64 code = mcause & 0x7FFFFFFF;
    // Stopped at the breakpoint ESP-IDF's panic handler had the debugger set
    // at the faulting instruction: mepc's, by a trigger (dcsr.cause 2,
    // where the debugger gives dcsr), not at one of the user's.
    u64 dcsr = 0;
    const bool rerun = !panic && !f.in_panic && pc && pc == mepc && !f.user_break && (!reg(f, "dcsr", &dcsr) || ((dcsr >> 6) & 7) == 2);
    if (!panic && !f.in_panic) {
        // Else in the trap handler: its PC in the function at mtvec's base
        // (the ELF's names; else the debugger's for the frame). Either way a
        // fault, not an interrupt or an environment call.
        const u64 base = mtvec & ~u64{3};
        const auto b = where.find(base), p = where.find(pc);
        const bool in_handler = base && (pc == base || (b != where.end() && !b->second.function.empty() &&
                                                        ((p != where.end() && p->second.function == b->second.function) || (!f.frames.empty() && f.frames[0] == b->second.function))));
        if ((!in_handler && !rerun) || interrupt || code == 8 || code == 9 || code == 11) return {};
    }
    o.r.fault = true;
    std::string what;
    if (panic && f.pseudo) what = "ESP-IDF's panic from an interrupt (mcause " + std::to_string(code) + "): a cache error or an interrupt watchdog";
    else if (interrupt) what = std::string(rv_interrupt(code) ? rv_interrupt(code) : "Interrupt") + " (mcause " + hex8(mcause) + ")";
    else what = std::string(rv_exception(code) ? rv_exception(code) : "Exception") + " (mcause " + std::to_string(code) + ")";
    o.r.title = what;
    o.add(what, 2);
    o.heading("Trap");
    o.value("mcause", mcause, std::string(interrupt ? "interrupt " : "exception ") + std::to_string(code) +
                                  (!interrupt && rv_exception(code) ? ": " + std::string(rv_exception(code)) : interrupt && rv_interrupt(code) ? ": " + std::string(rv_interrupt(code)) : ""));
    o.value("mepc", mepc, named(where, mepc), mepc);
    const bool address = !interrupt && rv_address_cause(code);
    o.value("mtval", mtval, address ? (mtval ? "the address that faulted" : "the address 0, or the core gives none") : code == 2 && !interrupt ? (mtval ? "the instruction's bits" : "an instruction of all zeros, or the core gives none") : "");
    if (mtvec) {
        const std::string handler = named(where, mtvec & ~u64{3});
        o.value("mtvec", mtvec, handler + (handler.empty() ? "" : " ") + ((mtvec & 3) == 1 ? "(vectored)" : "(direct)"));
    }
    if (mstatus) {
        static constexpr const char* kMode[] = {"user", "supervisor", "", "machine"};
        const u32 mpp = static_cast<u32>((mstatus >> 11) & 3);
        o.value("mstatus", mstatus, "MPP " + std::to_string(mpp) + (*kMode[mpp] ? std::string(": it was in ") + kMode[mpp] + " mode" : std::string()));
    }
    if (panic) o.add("  from ESP-IDF's panic frame at " + hex8(f.panic_frame), 4);
    else if (f.in_panic) o.add("  from the core's registers, in ESP-IDF's panic handler (its frame could not be read here)", 4);
    else if (rerun) o.add("  ESP-IDF's panic handler had a breakpoint set at the instruction that faulted and returned to it: the core stopped there.", 4);
    std::string line = what;
    if (address && mtval) line += " at " + hex8(mtval) + " (mtval)";
    else if (!interrupt && code == 2 && mtval) line += "; instruction " + hex8(mtval);
    line += "; mepc " + hex8(mepc);
    if (const std::string p = place(where, mepc); !p.empty()) line += " in " + p;
    o.r.line = std::move(line);
    o.r.pc = mepc;
    return o.r;
}

// ---- Xtensa's report

FaultReport xt_report(const FaultFacts& f, const std::map<u64, SourceFrame>& where) {
    Out o;
    u64 cause = 0, epc1 = 0, vaddr = 0, pc = 0;
    reg(f, "pc", &pc);
    // (A panic frame whose a1, the stack pointer, is 0 is none: read before
    // the panic handler had it, at its first instruction, where gdb's a2 is
    // not yet its argument.)
    const bool panic = f.panic_frame && word(f.panic_words, 21) >= 0 && word(f.panic_words, 4) > 0;
    bool rerun = false;
    if (panic) {  // (XtExcFrame: exit 0, pc 1, ps 2, a0 3 ... a15 18, sar 19, exccause 20, excvaddr 21)
        epc1 = static_cast<u64>(word(f.panic_words, 1));
        cause = static_cast<u64>(word(f.panic_words, 20));
        vaddr = static_cast<u64>(word(f.panic_words, 21));
    } else {
        if (!reg(f, "exccause", &cause)) return {};
        reg(f, "epc1", &epc1);
        reg(f, "excvaddr", &vaddr);
        // Stopped at the hardware breakpoint ESP-IDF's panic handler set at
        // the faulting instruction (IBREAKA0, enabled; DEBUGCAUSE.IBREAK),
        // not at one of the user's.
        u64 dc = 0, en = 0, a0 = 0;
        rerun = reg(f, "debugcause", &dc) && (dc & 2) && reg(f, "ibreakenable", &en) && (en & 1) && reg(f, "ibreaka0", &a0) && a0 == pc && !f.user_break;
        if ((!rerun && !f.in_panic) || !xt_fault(cause)) return {};
    }
    o.r.fault = true;
    const Cause* c = f.pseudo ? nullptr : xt_cause(cause);
    std::string what;
    if (f.pseudo) what = std::string(xt_pseudo(cause) ? xt_pseudo(cause) : "Unknown reason") + " (ESP-IDF's cause " + std::to_string(cause) + ")";
    else what = std::string(c ? c->name : "Unknown") + " (EXCCAUSE " + std::to_string(cause) + ")" + (c ? std::string(", ") + c->words : std::string());
    o.r.title = what;
    o.add(what, 2);
    o.heading("Exception");
    o.number("EXCCAUSE", cause, f.pseudo ? "written by ESP-IDF's panicHandler" : c ? std::string(c->name) + ": " + c->words : "");
    // (The PC that faulted: EPC1, or, at the breakpoint ESP-IDF set, where it stopped: the debugger's own window
    // exceptions in the panic handler may have changed EPC1 since.)
    const u64 at = rerun && pc ? pc : epc1;
    o.value("EPC1", epc1, named(where, epc1), epc1);
    const bool address = c && c->address;
    o.value("EXCVADDR", vaddr, address ? "the address that faulted" : "not about an address for this cause");
    if (panic) {
        const u64 a0 = static_cast<u64>(std::max<i64>(0, word(f.panic_words, 3))), ret = xt_return(a0, epc1);
        const std::string to = named(where, ret);
        o.value("A0", a0, "returns to " + hex8(ret) + (to.empty() ? std::string() : " in " + to), ret);
        o.value("A1", static_cast<u64>(std::max<i64>(0, word(f.panic_words, 4))), "the stack pointer");
        o.add("  from ESP-IDF's panic frame at " + hex8(f.panic_frame), 4);
    } else if (f.in_panic) {
        o.add("  from the core's registers, in ESP-IDF's panic handler (its frame could not be read here)", 4);
    }
    if (rerun) {
        if (pc != epc1) o.value("PC", pc, named(where, pc), pc);
        o.add("  ESP-IDF's panic handler set a breakpoint at the instruction that faulted and returned to it: the core stopped there.", 4);
    }
    std::string line = what;
    if (address) line += " at " + hex8(vaddr) + " (EXCVADDR)";
    line += std::string(at == epc1 ? "; EPC1 " : "; PC ") + hex8(at);
    if (const std::string p = place(where, at); !p.empty()) line += " in " + p;
    o.r.line = std::move(line);
    o.r.pc = at;
    return o.r;
}

}  // namespace

FaultFacts::Arch fault_arch(const FaultFacts& f) {
    if (f.regs.contains("xpsr") && (f.regs.contains("msp") || f.regs.contains("msp_s") || f.regs.contains("msp_ns"))) return FaultFacts::kCortexM;
    if (f.regs.contains("mcause") && f.regs.contains("mepc")) return FaultFacts::kRiscV;
    if (f.regs.contains("exccause") && f.regs.contains("epc1")) return FaultFacts::kXtensa;
    return FaultFacts::kUnknown;
}

bool fault_cm_maybe(const FaultFacts& f) {
    u64 xpsr = 0;
    if (reg(f, "xpsr", &xpsr) && exception_name(static_cast<u32>(xpsr & 0x1FF))) return true;
    return !f.frames.empty() && handler_exception(f.frames[0]);
}

int fault_cm_exc_frame(const FaultFacts& f) {
    for (size_t i = 1; i < f.ips.size(); ++i)
        if (f.ips[i] >= 0xF0000000u && f.ips[i] <= 0xFFFFFFFFu) return static_cast<int>(i);
    return -1;
}

bool fault_cm_has_status(u32 cpuid) { return ((cpuid >> 16) & 0xF) == 0xF; }  // (CPUID.Architecture: 0xC ARMv6-M and v8-M Baseline, 0xF v7-M and v8-M Mainline)

bool fault_cm_has_security(u32 cpuid, u32 id_pfr1) { return fault_cm_has_status(cpuid) && is_v8(cpuid) && ((id_pfr1 >> 4) & 0xF) != 0; }

bool fault_cm_has_disdefwbuf(u32 cpuid) {
    const Part* p = part_of(cpuid);
    return p && (p->partno == 0xC23 || p->partno == 0xC24);
}

bool fault_cm_exc_return(const FaultFacts& f, u32* exc) {
    u64 lr = 0;
    if (reg(f, "lr", &lr) && (lr >> 24) == 0xFF) return *exc = static_cast<u32>(lr), true;
    if (const int k = fault_cm_exc_frame(f); k > 0) return *exc = static_cast<u32>(f.ips[static_cast<size_t>(k)]), true;
    return false;
}

bool fault_cm_frame_at(const FaultFacts& f, u64* r0, bool* exact) {
    u32 exc = 0;
    if (!fault_cm_exc_return(f, &exc)) return false;
    const i64 cpuid = word(f.scb, 0);
    const bool v8 = cpuid >= 0 && is_v8(static_cast<u32>(cpuid));
    // (On v8-M with the Security Extension the stack pointers are banked: EXC_RETURN.S says whose.)
    const char* bank = v8 && f.regs.contains("psp_s") ? (exc & 64 ? "_s" : "_ns") : "";
    u64 sp = 0;
    *exact = true;
    if ((exc & 8) && (exc & 4)) {  // the process stack: the handler does not move it
        if (!reg(f, std::string("psp") + bank, &sp) && !reg(f, "psp", &sp)) return false;
    } else if (f.exc_sp_known) {
        sp = f.exc_sp;
    } else {
        if (!reg(f, std::string("msp") + bank, &sp) && !reg(f, "msp", &sp) && !reg(f, "sp", &sp)) return false;
        *exact = false;
    }
    // (Below R0, the additional state context when DCRS is 0: ten words.)
    if (v8 && !(exc & 32)) sp += 40;
    *r0 = sp;
    return true;
}

bool fault_panic_function(std::string_view name, bool* pseudo) {
    *pseudo = name == "panicHandler";
    return name == "panicHandler" || name == "xt_unhandled_exception";
}

u32 fault_panic_words(FaultFacts::Arch a) { return a == FaultFacts::kXtensa ? 22 : a == FaultFacts::kRiscV ? 36 : 0; }

std::vector<u64> fault_addresses(const FaultFacts& f) {
    std::vector<u64> out;
    auto add = [&](u64 a) {
        a &= 0xFFFFFFFFu;
        if (a && std::ranges::find(out, a) == out.end()) out.push_back(a);
    };
    u64 v = 0;
    switch (f.arch) {
        case FaultFacts::kCortexM:
            if (const i64 lr = word(f.stacked, 5); lr >= 0 && (lr >> 24) != 0xFF) add(static_cast<u64>(lr) & ~u64{1});
            if (const i64 pc = word(f.stacked, 6); pc >= 0) add(static_cast<u64>(pc));
            break;
        case FaultFacts::kRiscV:
            if (f.panic_frame) {
                add(static_cast<u64>(std::max<i64>(0, word(f.panic_words, 0))));
                add(static_cast<u64>(std::max<i64>(0, word(f.panic_words, 33))) & ~u64{3});
            } else {
                if (reg(f, "mepc", &v)) add(v);
                if (reg(f, "mtvec", &v)) add(v & ~u64{3});
            }
            if (reg(f, "pc", &v)) add(v);
            break;
        case FaultFacts::kXtensa:
            if (f.panic_frame) {
                const u64 pc = static_cast<u64>(std::max<i64>(0, word(f.panic_words, 1)));
                add(pc);
                add(xt_return(static_cast<u64>(std::max<i64>(0, word(f.panic_words, 3))), pc));
            }
            if (reg(f, "epc1", &v)) add(v);
            if (reg(f, "pc", &v)) add(v);
            break;
        case FaultFacts::kUnknown: break;
    }
    return out;
}

FaultReport fault_report(const FaultFacts& f, const std::map<u64, SourceFrame>& where) {
    switch (f.arch) {
        case FaultFacts::kCortexM: return cm_report(f, where);
        case FaultFacts::kRiscV: return rv_report(f, where);
        case FaultFacts::kXtensa: return xt_report(f, where);
        case FaultFacts::kUnknown: break;
    }
    return {};
}

}  // namespace blit
