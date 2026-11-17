// A program for the debugging tests with two threads: the second waits in
// a function of its own (on one line, so that where it is is never in
// doubt) while the first stops at a breakpoint.
#include <pthread.h>
#include <stdio.h>

static volatile int ready, go;

static void wait_here(void) { ready = 1; while (!go) { } }

static void* worker(void* arg)
{
    wait_here();
    return arg;
}

int main(void)
{
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    while (!ready) { }
    go = 1;
    pthread_join(t, NULL);
    printf("joined\n");
    return 0;
}
