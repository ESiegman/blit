// A bare-metal Cortex-M3 program for QEMU's mps2-an385 (tests/boards.sh):
// its vector table, a reset handler that clears .bss, and a main that
// calls a function in a loop, never ending, as firmware does.
typedef unsigned int u32;
extern u32 _estack, _sbss, _ebss;
void reset(void);
__attribute__((section(".vectors"), used)) void (*const vectors[])(void) = {(void (*)(void))&_estack, reset};

volatile u32 total;

static u32 square(u32 x) {
    u32 y = x * x;
    return y;
}

int main(void) {
    total = 0;
    for (u32 i = 1;; ++i) {
        total += square(i);
        if (i == 1000) i = 0;
    }
}

void reset(void) {
    for (u32* p = &_sbss; p < &_ebss; ++p) *p = 0;
    main();
}
