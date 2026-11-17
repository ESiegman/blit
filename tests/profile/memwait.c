// A program whose memory and waiting are known: with "memory", big() first
// touches 24 MB and small() 8 MB (each page written in the function itself,
// in blocks large enough that malloc hands them over untouched); with
// "waiting", sleeper() sleeps 300 ms and reader() waits 200 ms on a pipe.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void __attribute__((noinline)) touch(volatile char* p, size_t n) {
    for (size_t i = 0; i < n; i += 4096) p[i] = 1;
}
static char* __attribute__((noinline)) big(void) {
    char* p = malloc(24u << 20);
    for (size_t i = 0; i < (24u << 20); i += 4096) ((volatile char*)p)[i] = 1;
    return p;
}
static char* __attribute__((noinline)) small(void) {
    char* p = malloc(8u << 20);
    for (size_t i = 0; i < (8u << 20); i += 4096) ((volatile char*)p)[i] = 1;
    return p;
}
static int fds[2];
static void* writer(void* arg) {
    (void)arg;
    struct timespec t = {0, 200000000};
    nanosleep(&t, NULL);
    (void)!write(fds[1], "x", 1);
    return NULL;
}
static void __attribute__((noinline)) reader(void) {
    char c;
    (void)!read(fds[0], &c, 1);
}
static void __attribute__((noinline)) sleeper(void) {
    struct timespec t = {0, 300000000};
    nanosleep(&t, NULL);
}
int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "waiting") == 0) {
        if (pipe(fds)) return 1;
        pthread_t th;
        pthread_create(&th, NULL, writer, NULL);
        reader();
        pthread_join(th, NULL);
        sleeper();
        return 0;
    }
    char* a = big();
    char* b = small();
    touch(a, 4096);
    free(a);
    free(b);
    return 0;
}
