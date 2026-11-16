// A program for the debugging tests: a loop, a function it calls, and
// what it prints.
#include <stdio.h>

static int square(int x)
{
    int y = x * x;
    return y;
}

int main(void)
{
    int total = 0;
    for (int i = 1; i <= 3; ++i) {
        total += square(i);
        printf("i=%d total=%d\n", i, total);
    }
    return total == 14 ? 0 : 1;
}
