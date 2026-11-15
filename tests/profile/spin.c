// A program for the profiling tests: most of its time in two functions,
// one called from the other, each over several lines.
#include <stdio.h>
#include <stdlib.h>

static volatile double sink;

__attribute__((noinline)) static double inner(int n)
{
    volatile double s = 0;
    for (int i = 0; i < n; ++i)
        s += (double)i * 0.5;
    return s;
}

__attribute__((noinline)) static double outer(int n)
{
    volatile double t = 1;
    for (int i = 1; i < n; ++i)
        t *= 1.0000001;
    return t + inner(n);
}

int main(int argc, char** argv)
{
    const int rounds = argc > 1 ? atoi(argv[1]) : 100;
    for (int r = 0; r < rounds; ++r)
        sink += outer(2000000);
    printf("%f\n", sink);
    return 0;
}
