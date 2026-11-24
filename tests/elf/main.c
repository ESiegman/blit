// Its entry, with no C library: built with -nostdlib for any ARM target.
#include "motor.h"

static int ticks;

static void tick(void)
{
    ++ticks;
}

int main(void)
{
    tick();
    motor_run(3);
    return ticks;
}

void _start(void)
{
    main();
    for (;;) {
    }
}
