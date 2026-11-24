// Bare-metal Cortex-M programs that fault (tests/faults.sh), for QEMU's
// mps2-an385 (Cortex-M3), mps2-an386 (M4 with its FPU), mps2-an505
// (M33) and microbit (M0). Each fault handler spins, as a firmware's
// default handler does, until the debugger pauses it. MODE chooses the
// fault (at build time: an M0 has no divide instruction to fault with):
//   1  UDF                          UsageFault, UNDEFINSTR
//   2  10 / 0 with CCR.DIV_0_TRP    UsageFault, DIVBYZERO
//   3  load from an unmapped address  BusFault, PRECISERR and BFARVALID
//   4  store to an unmapped address   BusFault (imprecise on a real M3: QEMU's are all precise)
//   5  load from where the MPU allows none  MemManage, DACCVIOL and MMARVALID
//   6  UDF with the fault handlers off      HardFault, FORCED (on an M0, HardFault alone)
// PSP: the fault in thread mode on the process stack. FPU: the FPU on and
// used first, so that the core stacks the FP state (EXC_RETURN bit 4 clear).
typedef unsigned int u32;
extern u32 _estack, _sbss, _ebss;
void reset(void);
void HardFault_Handler(void) { for (;;) __asm volatile("nop"); }
void MemManage_Handler(void) { for (;;) __asm volatile("nop"); }
void BusFault_Handler(void) { for (;;) __asm volatile("nop"); }
void UsageFault_Handler(void) { for (;;) __asm volatile("nop"); }
void SecureFault_Handler(void) { for (;;) __asm volatile("nop"); }
__attribute__((section(".vectors"), used)) void (*const vectors[16])(void) = {
    (void (*)(void))&_estack, reset, 0, HardFault_Handler, MemManage_Handler, BusFault_Handler, UsageFault_Handler, SecureFault_Handler};

#define REG(a) (*(volatile u32*)(a))
volatile u32 zero, sink;
volatile float gain = 1.5f;
u32 pstack[256];

__attribute__((noinline)) void motor_step(void) {
#if MODE == 1 || MODE == 6
    __asm volatile("udf #0");
#elif MODE == 2
    sink = 10 / zero;
#elif MODE == 3
    sink = REG(0x5ff00000);
#elif MODE == 4
    REG(0x5ff00000) = 1;
#elif MODE == 5
    sink = REG(0x20000100);
#endif
}

int main(void) {
#ifdef FPU
    REG(0xE000ED88) |= 0xFu << 20;  // CPACR: CP10 and CP11, full access
    __asm volatile("dsb; isb");
    gain = gain * 3.0f;
#endif
#if MODE != 6
    REG(0xE000ED24) |= 7u << 16;  // SHCSR: MemManage, BusFault and UsageFault on
#endif
#if MODE == 2
    REG(0xE000ED14) |= 1u << 4;  // CCR.DIV_0_TRP
#endif
#if MODE == 5
    // The MPU: region 0 all of memory, read and write; region 1, 32 bytes at 0x20000100, no access.
    REG(0xE000ED98) = 0, REG(0xE000ED9C) = 0, REG(0xE000EDA0) = (31u << 1) | (3u << 24) | 1;
    REG(0xE000ED98) = 1, REG(0xE000ED9C) = 0x20000100, REG(0xE000EDA0) = (4u << 1) | 1;
    REG(0xE000ED94) = 1 | 4;  // ENABLE, PRIVDEFENA
    __asm volatile("dsb; isb");
#endif
#ifdef PSP
    __asm volatile("msr psp, %0; movs r0, #2; msr control, r0; isb" ::"r"(&pstack[256]) : "r0");
#endif
    motor_step();
    for (;;) {
    }
}

void reset(void) {
    for (u32* p = &_sbss; p < &_ebss; ++p) *p = 0;
    main();
}
