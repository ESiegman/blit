// The fault report's tables (src/fault.hpp) for what a simulator cannot
// make, for tests/faults.sh: what the core's registers and memory would
// hold, as each case's comment says where it is from; printed as the
// Debug tab's line, then the report's lines, " | " between them.
//
//   blit_fault_test CASE
//
//   imprecise              a Cortex-M3's store through its write buffer: IMPRECISERR
//   imprecise-disdefwbuf   the same with ACTLR.DISDEFWBUF already set
//   securefault            a Cortex-M33's non-secure call to secure memory with no SG: INVEP
//   m0-forced              a Cortex-M0's HardFault: no status registers, its stacked frame
//   xtensa-rerun           an ESP32-S3 stopped at the breakpoint ESP-IDF's panic handler set
//   xtensa-user-break      the same registers, at one of the user's own breakpoints: no fault
//   xtensa-panic           stopped in xt_unhandled_exception: its XtExcFrame
//   xtensa-pseudo          stopped in panicHandler: ESP-IDF's own cause (an interrupt watchdog)
//   xtensa-entry           stopped at xt_unhandled_exception's first instruction: its `frame`
//                          not yet the argument (gdb read 0x3a there), the registers instead
//   riscv-panic            an ESP32-C3 stopped in xt_unhandled_exception: its RvExcFrame
//   riscv-interrupt        a RISC-V stop in the trap handler for a timer interrupt: no fault
//   riscv-rerun            an ESP32-C3 stopped at the breakpoint ESP-IDF had the debugger set (from
//                          ESP-IDF's source: no RISC-V ESP32 here to check it on)
//   riscv-step             the same registers, stopped by a step (dcsr.cause 4): no fault
#include <cstdio>
#include <string>

#include "fault.hpp"

using namespace blit;

namespace {

void print(const FaultFacts& f, const std::map<u64, SourceFrame>& where) {
    const FaultReport r = fault_report(f, where);
    if (!r.fault) return (void)std::printf("no fault\n");
    std::printf("%s\n", r.line.c_str());
    std::string out;
    for (const FaultReport::Line& l : r.lines) {
        std::string t = l.text;
        while (t.starts_with(' ')) t.erase(0, 1);
        if (!t.empty()) out += (out.empty() ? "" : " | ") + t + (l.addr ? " [->]" : "");
    }
    std::printf("%s\n", out.c_str());
}

std::map<u64, SourceFrame> names(std::initializer_list<std::pair<u64, SourceFrame>> l) { return {l.begin(), l.end()}; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return std::fprintf(stderr, "usage: blit_fault_test CASE\n"), 2;
    const std::string c = argv[1];
    FaultFacts f;
    if (c == "imprecise" || c == "imprecise-disdefwbuf") {
        // A Cortex-M3 r2p1 (CPUID 0x412FC231), in BusFault_Handler: CFSR
        // IMPRECISERR (bit 10), no BFAR; EXC_RETURN in LR, thread mode, MSP.
        f.arch = FaultFacts::kCortexM;
        f.regs = {{"xpsr", 0x01000005}, {"lr", 0xFFFFFFF9}, {"sp", 0x20001FE0}, {"msp", 0x20001FE0}, {"psp", 0}, {"pc", 0x08000210}};
        f.frames = {"BusFault_Handler", "<signal handler called>", "motor_step", "main"};
        f.ips = {0x08000210, 0xFFFFFFF9, 0x08001A40, 0x08001B02};
        f.exc_sp_known = true, f.exc_sp = 0x20001FE0;
        f.scb.assign(18, -1);
        f.scb[0] = 0x412FC231, f.scb[9] = 0x00050000, f.scb[10] = 0x400, f.scb[11] = 0, f.scb[12] = 0, f.scb[13] = 0, f.scb[14] = 0, f.scb[15] = 0, f.scb[17] = 0x200;
        f.actlr = c == "imprecise" ? 0 : 2;
        f.stacked = {0x2A, 0x40011000, 0, 0, 0, 0x08001B03, 0x08001A40, 0x21000000};
        print(f, names({{0x08001A40, {"motor_step", "/p/motor.c", 212, 0, false}}, {0x08001B02, {"main", "/p/main.c", 30, 0, false}}}));
    } else if (c == "securefault") {
        // A Cortex-M33 r0p4 (CPUID 0x410FD214) in SecureFault_Handler: SFSR
        // INVEP (SFAR not valid), from non-secure thread mode on its PSP:
        // EXC_RETURN 0xFFFFFFBD (ES 1, PSP, thread, no FP, DCRS 1, S 0).
        f.arch = FaultFacts::kCortexM;
        f.regs = {{"xpsr", 0x07}, {"lr", 0xFFFFFFBD}, {"msp_s", 0x30001F00}, {"psp_s", 0}, {"msp_ns", 0x20001F00}, {"psp_ns", 0x20003F00}, {"msp", 0x30001F00}, {"psp", 0}};
        f.frames = {"SecureFault_Handler"};
        f.ips = {0x10000400};
        f.scb.assign(18, -1);
        f.scb[0] = 0x410FD214, f.scb[9] = 0x00070000, f.scb[10] = 0, f.scb[11] = 0, f.scb[12] = 0, f.scb[13] = 0, f.scb[14] = 0, f.scb[15] = 0, f.scb[17] = 0x210;
        f.sfsr = 1, f.sfar = 0;
        f.stacked = {0, 0, 0, 0, 0, 0x00200101, 0x10000800, 0x01000000};
        print(f, {});
    } else if (c == "m0-forced") {
        // A Cortex-M0+ r0p1 (CPUID 0x410CC601) in HardFault_Handler, LR
        // pushed already (no EXC_RETURN there): gdb's frame for the exception.
        f.arch = FaultFacts::kCortexM;
        f.regs = {{"xpsr", 0x03}, {"lr", 0x10000325}, {"sp", 0x20041F00}, {"msp", 0x20041F00}, {"psp", 0}};
        f.frames = {"HardFault_Handler", "<signal handler called>", "motor_step"};
        f.ips = {0x100002F0, 0xFFFFFFF9, 0x10000410};
        f.exc_sp_known = true, f.exc_sp = 0x20041F08;
        f.scb.assign(18, -1);
        f.scb[0] = 0x410CC601;
        f.stacked = {1, 2, 3, 4, 12, 0x10000451, 0x10000410, 0x01000000};
        print(f, names({{0x10000410, {"motor_step", "/p/motor.c", 40, 0, false}}}));
    } else if (c == "xtensa-rerun" || c == "xtensa-user-break") {
        // An ESP32-S3 at the breakpoint panic.c sets (esp_cpu_set_breakpoint(0, addr)):
        // as OpenOCD v0.12.0-esp32-20260831 and ESP-IDF v6.1.1 gave them on the lab
        // box's board, a store to a null pointer.
        f.arch = FaultFacts::kXtensa;
        f.regs = {{"pc", 0x42009126}, {"ps", 0x60a20}, {"exccause", 29}, {"epc1", 0x42009126}, {"excvaddr", 0}, {"debugcause", 2}, {"ibreakenable", 1}, {"ibreaka0", 0x42009126}};
        f.frames = {"motor_step", "app_main", "main_task", "vPortTaskWrapper"};
        f.ips = {0x42009126, 0x42009147, 0x420069fe, 0x4200eadd};
        f.user_break = c == "xtensa-user-break";
        print(f, names({{0x42009126, {"motor_step", "/tmp/m5-fault-esp/main/main.c", 10, 0, false}}}));
    } else if (c == "xtensa-panic" || c == "xtensa-pseudo") {
        // Stopped in xt_unhandled_exception (or panicHandler): its `frame`, an
        // XtExcFrame (xtensa_context.h:123-144): a LoadProhibited at 0x00000004,
        // or ESP-IDF's PANIC_RSN_INTWDT_CPU0 (5).
        f.arch = FaultFacts::kXtensa;
        f.regs = {{"pc", 0x40378a10}, {"exccause", 4}, {"epc1", 0x40041234}, {"excvaddr", 0x4}};
        f.frames = {"panic_handler", c == "xtensa-panic" ? "xt_unhandled_exception" : "panicHandler", "_xt_user_exc"};
        f.ips = {0x40378a10, 0x40378b00, 0x40378c00};
        f.pseudo = c == "xtensa-pseudo";
        f.in_panic = true;
        f.panic_frame = 0x3fc9a0b0;
        f.panic_words.assign(22, 0);
        f.panic_words[1] = 0x42009210, f.panic_words[2] = 0x60030, f.panic_words[3] = 0x82009300, f.panic_words[4] = 0x3fc9a1c0;
        f.panic_words[20] = c == "xtensa-panic" ? 28 : 5, f.panic_words[21] = c == "xtensa-panic" ? 4 : 0;
        print(f, names({{0x42009210, {"read_sensor", "/p/main/sensor.c", 77, 0, false}}}));
    } else if (c == "xtensa-entry") {
        // As the lab box's ESP32-S3 gave it with a breakpoint on
        // xt_unhandled_exception: `frame` 0x3a, the words there zeros.
        f.arch = FaultFacts::kXtensa;
        f.regs = {{"pc", 0x40378e3a}, {"exccause", 29}, {"epc1", 0x42009126}, {"excvaddr", 0}, {"debugcause", 2}, {"ibreakenable", 1}, {"ibreaka0", 0x40378e3a}};
        f.frames = {"xt_unhandled_exception", "_xt_handle_exc"};
        f.ips = {0x40378e3a, 0x40375c20};
        f.user_break = true, f.in_panic = true;
        f.panic_frame = 0x3a;
        f.panic_words.assign(22, 0);
        print(f, names({{0x42009126, {"motor_step", "/tmp/m5-fault-esp/main/main.c", 10, 0, false}}}));
    } else if (c == "riscv-panic") {
        // An ESP32-C3 stopped in xt_unhandled_exception (vectors.S:18's
        // panic_from_exception): its RvExcFrame (rvruntime-frames.h:48-84): a
        // store access fault (7) at 0x00000000.
        f.arch = FaultFacts::kRiscV;
        f.regs = {{"pc", 0x40380a00}, {"mcause", 7}, {"mepc", 0x40380a00}, {"mtval", 0}, {"mtvec", 0x40380001}};
        f.frames = {"panic_handler", "xt_unhandled_exception", "_panic_handler"};
        f.ips = {0x40380a00, 0x40380b00, 0x40380c00};
        f.panic_frame = 0x3fc8e0a0, f.in_panic = true;
        f.panic_words.assign(36, 0);
        f.panic_words[0] = 0x42005d6a, f.panic_words[1] = 0x42005d80, f.panic_words[2] = 0x3fc8e200, f.panic_words[32] = 0x1880, f.panic_words[33] = 0x40380001, f.panic_words[34] = 7, f.panic_words[35] = 0;
        print(f, names({{0x42005d6a, {"motor_step", "/p/main/motor.c", 12, 0, false}}, {0x40380000, {"_vector_table", "", 0, 0, false}}}));
    } else if (c == "riscv-rerun" || c == "riscv-step") {
        // A load access fault (5) at 0x00000000 re-run: the PC is mepc,
        // dcsr.cause 2 (a trigger), or 4 (a step).
        f.arch = FaultFacts::kRiscV;
        f.regs = {{"pc", 0x42005d6a}, {"mcause", 5}, {"mepc", 0x42005d6a}, {"mtval", 0}, {"mtvec", 0x40380001}, {"dcsr", c == "riscv-rerun" ? 0x40000083u : 0x40000103u}};
        f.frames = {"motor_step", "app_main"};
        f.ips = {0x42005d6a, 0x42005d90};
        print(f, names({{0x42005d6a, {"motor_step", "/p/main/motor.c", 12, 0, false}}}));
    } else if (c == "riscv-interrupt") {
        // In the trap handler (mtvec's base) for the machine timer interrupt: no fault.
        f.arch = FaultFacts::kRiscV;
        f.regs = {{"pc", 0x20400018}, {"mcause", 0x80000007}, {"mepc", 0x20400070}, {"mtval", 0}, {"mtvec", 0x20400010}};
        f.frames = {"trap_handler", "main"};
        f.ips = {0x20400018, 0x20400070};
        print(f, names({{0x20400010, {"trap_handler", "/p/rv.c", 9, 0, false}}, {0x20400018, {"trap_handler", "/p/rv.c", 10, 0, false}}}));
    } else {
        return std::fprintf(stderr, "no case %s\n", c.c_str()), 2;
    }
    return 0;
}
