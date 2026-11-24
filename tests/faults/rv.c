// A bare-metal RISC-V program that faults (tests/faults.sh), for QEMU's
// sifive_e (an RV32IMAC E31): its trap handler spins until the debugger
// pauses it. MODE: 1 a load from an unmapped address (mcause 5, mtval
// the address), 2 an illegal instruction (mcause 2).
typedef unsigned int u32;
extern u32 _estack;
volatile u32 sink;

__attribute__((aligned(4), noinline)) void trap_handler(void) {
    for (;;) __asm volatile("nop");
}

__attribute__((noinline)) void motor_step(void) {
#if MODE == 1
    sink = *(volatile u32*)0x60000000;
#else
    __asm volatile(".word 0");
#endif
}

int main(void) {
    __asm volatile("csrw mtvec, %0" ::"r"(trap_handler));
    motor_step();
    for (;;) {
    }
}

__attribute__((naked, section(".start"))) void _start(void) { __asm volatile("la sp, _estack; j main"); }
