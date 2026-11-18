// A program for the debugging tests to attach to: it waits, a little at a
// time, until it has been told to stop (by a debugger setting `go` to 0).
#include <stdio.h>
#include <unistd.h>

volatile int go = 1;

int main(void)
{
    int rounds = 0;
    while (go) {
        usleep(20000);
        ++rounds;
    }
    printf("rounds=%d\n", rounds);
    return 0;
}
