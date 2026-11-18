// A program for the allocation tests: make_big allocates most of the
// bytes, make_small many small blocks; half of make_big's are freed and the
// rest are still held when it ends (a leak), make_small's are all freed.
#include <stdlib.h>
#include <string.h>

static void* kept[64];
static char* volatile sink;  // (so that the compiler keeps the small blocks: a malloc freed at once it may leave out)

__attribute__((noinline)) static void* make_big(void) {
    void* p = malloc(1 << 20);
    memset(p, 1, 64);
    return p;
}

__attribute__((noinline)) static void make_small(void) {
    for (int i = 0; i < 2000; ++i) {
        sink = malloc(64);
        sink[0] = (char)i;
        free(sink);
    }
}

int main(void) {
    for (int i = 0; i < 64; ++i) kept[i] = make_big();
    for (int i = 0; i < 32; ++i) free(kept[i]);
    for (int i = 0; i < 16; ++i) make_small();
    return kept[40] ? 0 : 1;
}
