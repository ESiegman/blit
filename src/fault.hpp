#pragma once
// The fault report (the embedded design's E8): what a board's stop in a
// fault says, from the core's own registers and memory, for the three
// kinds of core blit debugs boards of:
//
//   Cortex-M (ARMv6-M, v7-M, v8-M): the exception from IPSR (3 HardFault
//   to 7 SecureFault), its status (CFSR, HFSR, MMFAR, BFAR, AFSR, and on
//   v8-M with the Security Extension SFSR and SFAR), every bit set in
//   words; EXC_RETURN (which stack, whether an FPU frame was stacked); the
//   frame the core stacked (R0 to R3, R12, LR, PC, xPSR). ARMv6-M and
//   v8-M Baseline (Cortex-M0, M0+, M1, M23) have no fault status
//   registers: the stacked frame alone, and the report says so.
//   RISC-V: mcause (its interrupt bit, its code named), mepc, mtval.
//   Xtensa (ESP32, ESP32-S3): EXCCAUSE named, EPC1, EXCVADDR.
//
// The app (appdebug.cpp) reads what is asked for here, step by step,
// through the debugger (DAP's Registers scope and readMemory), and names
// the addresses through the ELF (addrsrc.hpp); this file knows nothing of
// either, so that its tables can be checked alone.
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "addrsrc.hpp"
#include "common.hpp"

namespace blit {

// What is known of a stop, gathered by the app.
struct FaultFacts {
    enum Arch : u8 { kUnknown, kCortexM, kRiscV, kXtensa } arch = kUnknown;
    // The innermost frame's registers, by their names in lower case, as
    // the debugger's Registers scope gives them (32 bits each).
    std::map<std::string, u64, std::less<>> regs;
    // The call stack: each frame's function (as the debugger names it,
    // innermost first) and the address of its instruction.
    std::vector<std::string> frames;
    std::vector<u64> ips;
    bool user_break = false;  // stopped at one of the user's own breakpoints

    // Cortex-M. The stack pointer of the frame the debugger made for the
    // exception (gdb's "<signal handler called>": its SP is where the core
    // stacked the frame), where it unwound through one.
    bool exc_sp_known = false;
    u64 exc_sp = 0;
    // Words of the System Control Block read from 0xE000ED00 (-1 where not
    // read), SFSR and SFAR, ACTLR.
    std::vector<i64> scb;
    i64 sfsr = -1, sfar = -1, actlr = -1;
    // The stacked frame's words, from its R0 (-1 where not read).
    std::vector<i64> stacked;

    // ESP-IDF's panic handler's frame (XtExcFrame, RvExcFrame): its
    // address and words, where a frame of the panic handler is on the
    // stack; `pseudo` when it came by panicHandler, whose cause is one of
    // ESP-IDF's own (PANIC_RSN_*).
    u64 panic_frame = 0;
    bool pseudo = false;
    std::vector<i64> panic_words;
};

// The kind of core, from the names of its registers (xPSR and MSP; mcause;
// EXCCAUSE).
FaultFacts::Arch fault_arch(const FaultFacts& f);

// ---- Cortex-M, the reads in turn

// Whether the stop may be in a fault handler: IPSR 3 to 7, or the frame
// innermost a fault handler by its name. (Read the rest only if so.)
bool fault_cm_maybe(const FaultFacts& f);
// The frame the debugger made for the exception (its PC is EXC_RETURN):
// its index in the stack, or -1.
int fault_cm_exc_frame(const FaultFacts& f);
// Whether CPUID says the core has fault status registers (ARMv7-M, v8-M
// Mainline); and the Security Extension (ID_PFR1); and an ACTLR with
// DISDEFWBUF (Cortex-M3 and M4).
bool fault_cm_has_status(u32 cpuid);
bool fault_cm_has_security(u32 cpuid, u32 id_pfr1);
bool fault_cm_has_disdefwbuf(u32 cpuid);
// EXC_RETURN: LR where it is one, else the exception frame's PC; false
// when neither is.
bool fault_cm_exc_return(const FaultFacts& f, u32* exc);
// Where the stacked frame's R0 is (the stack EXC_RETURN chooses; for the
// main stack, the exception frame's SP where the debugger unwound through
// it, else SP as it is now, `exact` false), and how many words of it to
// read; false when that cannot be said.
bool fault_cm_frame_at(const FaultFacts& f, u64* r0, bool* exact);

// ---- RISC-V and Xtensa

// The names of the frames of ESP-IDF's panic handler that have the
// exception's frame as their argument `frame`.
bool fault_panic_function(std::string_view name, bool* pseudo);
// How many words of the panic frame to read (XtExcFrame to EXCVADDR,
// RvExcFrame to MTVAL).
u32 fault_panic_words(FaultFacts::Arch a);

// ---- the report

// The addresses the report names (the stacked PC and LR, mepc and mtvec's
// base, EPC1 and the PC): what the app looks up in the ELF.
std::vector<u64> fault_addresses(const FaultFacts& f);

struct FaultReport {
    bool fault = false;  // the stop is at a fault (else nothing more)
    std::string line;    // the Debug tab's line
    std::string title;   // the report's first line
    // The report's lines: each its text, its style (0 plain, 3 the accent,
    // 4 quieter, 2 an error's), and the address Enter goes to on it (0 none).
    struct Line {
        std::string text;
        u8 style = 0;
        u64 addr = 0;
    };
    std::vector<Line> lines;
    u64 pc = 0;  // where it faulted: the stacked PC, mepc, EPC1
};

// The report of what is known; `where` names an address (the innermost of
// its frames; none where the ELF says nothing of it).
FaultReport fault_report(const FaultFacts& f, const std::map<u64, SourceFrame>& where);

}  // namespace blit
