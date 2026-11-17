// A program for the profiling tests with two threads: the second, named
// "worker", does twice the work of the first, each in a function of its
// own, so that which thread and which function is heavier is never in doubt.
#define _GNU_SOURCE  // (glibc declares pthread_setname_np only then)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static volatile double sink;

__attribute__((noinline)) static double main_work(int n)
{
    volatile double s = 0;
    for (int i = 0; i < n; ++i)
        s += (double)i * 0.5;
    return s;
}

__attribute__((noinline)) static double worker_work(int n)
{
    volatile double s = 1;
    for (int i = 1; i < n; ++i)
        s *= 1.0000001;
    return s;
}

static void* worker(void* arg)
{
    const int rounds = *(const int*)arg;
#if defined(__APPLE__)
    pthread_setname_np("worker");
#else
    pthread_setname_np(pthread_self(), "worker");
#endif
    for (int r = 0; r < 2 * rounds; ++r)
        sink += worker_work(2000000);
    return NULL;
}

int main(int argc, char** argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 100;
    pthread_t t;
    pthread_create(&t, NULL, worker, &rounds);
    for (int r = 0; r < rounds; ++r)
        sink += main_work(2000000);
    pthread_join(t, NULL);
    printf("%f\n", sink);
    return 0;
}
