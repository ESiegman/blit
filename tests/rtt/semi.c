// A bare-metal Cortex-M3 program for QEMU's mps2-an385 that prints through
// semihosting (tests/rtt.sh): SYS_WRITE0 (0x04, a string), SYS_WRITEC
// (0x03, a character) and SYS_WRITE (0x05) to the handle SYS_OPEN (0x01)
// gives for ":tt", each a BKPT 0xAB with the operation in r0 and its
// argument (a block's address) in r1 (Arm's "Semihosting for AArch32 and
// AArch64", 2.0, sections 6.1-6.3). Then a crash's text, as firmware
// prints one, and a loop that counts, as firmware never ends.
typedef unsigned int u32;
extern u32 _estack, _sbss, _ebss;
void reset(void);
__attribute__((section(".vectors"), used)) void (*const vectors[])(void) = {(void (*)(void))&_estack, reset};

static u32 semihost(u32 op, const void* arg) {
    register u32 r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static u32 length(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

volatile u32 ticks;

// "0x" and eight hex digits of v, into out.
static void hex(char* out, u32 v) {
    out[0] = '0', out[1] = 'x';
    for (int i = 0; i < 8; ++i) out[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
}

int main(void) {
    semihost(0x04, "semihosting: hello from the board\n");
    const char c = '!';
    semihost(0x03, &c);
    semihost(0x03, "\n");
    const u32 open[3] = {(u32) ":tt", 4, 2};  // (mode 4: "w")
    const u32 tt = semihost(0x01, open);
    const char* line = "written to :tt\n";
    const u32 write[3] = {tt, (u32)line, length(line)};
    semihost(0x05, write);
    // (A crash's text in ESP-IDF's shape, the decoder's: the pc a place in length().)
    char trace[] = "Backtrace: 0x........:0x20000ff0\n";
    hex(trace + 11, (u32)&length + 4);
    semihost(0x04, "Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.\n");
    semihost(0x04, trace);
    semihost(0x04, "Rebooting...\n");
    for (;;) ++ticks;
}

void reset(void) {
    for (u32* p = &_sbss; p < &_ebss; ++p) *p = 0;
    main();
}
