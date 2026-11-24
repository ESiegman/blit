// A program with SEGGER RTT's control block (tests/rtt.sh): `_SEGGER_RTT`,
// as SEGGER_RTT.c names it, in zeroed memory as SEGGER's is until the
// program sets it up; and a function a crash's backtrace points into.
struct rtt_buffer {
    const char* name;
    char* buf;
    unsigned size, wr, rd, flags;
};
struct rtt_control {
    char id[16];
    int max_up, max_down;
    struct rtt_buffer up[2], down[1];
};
__attribute__((used)) struct rtt_control _SEGGER_RTT;

volatile int stop;

void fault(void) {
    while (!stop) {
    }
}

int main(void) {
    fault();
    return _SEGGER_RTT.max_up;
}
