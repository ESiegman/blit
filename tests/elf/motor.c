// A firmware's own code, for tests/elf.sh: a write through a null pointer
// two calls down, one of them inlined.
#include "motor.h"

static volatile int *target;

static inline __attribute__((always_inline)) void store(int v)
{
    *target = v;
}

__attribute__((noinline)) int pid_step(int e)
{
    int out = e * 3 + 1;
    store(out);
    return out;
}

__attribute__((noinline)) void motor_run(int n)
{
    for (int i = 0; i < n; ++i)
        pid_step(i);
}
