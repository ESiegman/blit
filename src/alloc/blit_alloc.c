// blit's allocation sampler, put into a program it profiles
// (DYLD_INSERT_LIBRARIES on a Mac, LD_PRELOAD on Linux). malloc, calloc,
// realloc, the aligned ones and free go through here: allocations are
// sampled by bytes (on average one for each BLIT_ALLOC_RATE bytes, each
// weighing that much, a larger one always and weighing its size), each
// with its stack; a sampled block freed again is no longer held. When the
// program ends (or is stopped with SIGINT or SIGTERM) what was sampled is
// written to BLIT_ALLOC_OUT, each address named as dladdr knows it, for
// profile.cpp to read: what was allocated, and what was still held.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// The real allocator.
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define REAL_MALLOC malloc
#define REAL_CALLOC calloc
#define REAL_REALLOC realloc
#define REAL_FREE free
#define REAL_MEMALIGN posix_memalign
#else
extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void __libc_free(void*);
extern void* __libc_memalign(size_t, size_t);
#define REAL_MALLOC __libc_malloc
#define REAL_CALLOC __libc_calloc
#define REAL_REALLOC __libc_realloc
#define REAL_FREE __libc_free
#endif

enum { kFrames = 48 };

typedef struct {
    void* ptr;
    size_t weight, size;
    int live, n;
    void* frames[kFrames];
} Sample;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_rate = 65536;
static Sample* g_samples;  // (in memory of its own, from mmap: never through the allocator)
static size_t g_count, g_cap;
static atomic_ullong g_bytes, g_allocs;
static atomic_int g_ready, g_written;
// Which blocks are sampled and still held: a table of their addresses
// (open addressing), and a filter of bits that a free looks at first.
static uintptr_t* g_live;
static size_t g_live_cap, g_live_n;
static _Atomic(uint64_t) g_filter[1 << 14];
static __thread long long t_until;  // bytes until the next sample
static __thread int t_busy;         // (inside the sampler: what it allocates itself is not sampled)

static void* raw(size_t n) {
    void* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static uint64_t hash_of(uintptr_t p) {
    uint64_t h = (uint64_t)p * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

static void live_put(uintptr_t p, size_t index) {
    if (g_live_n * 2 >= g_live_cap) {
        const size_t cap = g_live_cap ? g_live_cap * 2 : 4096;
        uintptr_t* t = raw(cap * 2 * sizeof(uintptr_t));
        if (!t) return;
        for (size_t i = 0; i < g_live_cap; ++i)
            if (g_live[2 * i]) {
                size_t k = hash_of(g_live[2 * i]) & (cap - 1);
                while (t[2 * k]) k = (k + 1) & (cap - 1);
                t[2 * k] = g_live[2 * i], t[2 * k + 1] = g_live[2 * i + 1];
            }
        if (g_live) munmap(g_live, g_live_cap * 2 * sizeof(uintptr_t));
        g_live = t, g_live_cap = cap;
    }
    size_t k = hash_of(p) & (g_live_cap - 1);
    while (g_live[2 * k] && g_live[2 * k] != p) k = (k + 1) & (g_live_cap - 1);
    if (!g_live[2 * k]) ++g_live_n;
    g_live[2 * k] = p, g_live[2 * k + 1] = index;
    const uint64_t h = hash_of(p);
    atomic_fetch_or(&g_filter[(h >> 6) & ((1 << 14) - 1)], 1ull << (h & 63));
}

// A sampled block freed: no longer held. (Removed with backward shifting, so that the table needs no tombstones.)
static void live_take(uintptr_t p) {
    if (!g_live_cap) return;
    size_t k = hash_of(p) & (g_live_cap - 1);
    while (g_live[2 * k] && g_live[2 * k] != p) k = (k + 1) & (g_live_cap - 1);
    if (!g_live[2 * k]) return;
    g_samples[g_live[2 * k + 1]].live = 0;
    g_live[2 * k] = 0;
    --g_live_n;
    for (size_t j = (k + 1) & (g_live_cap - 1); g_live[2 * j]; j = (j + 1) & (g_live_cap - 1)) {
        const size_t want = hash_of(g_live[2 * j]) & (g_live_cap - 1);
        if ((j > k && (want <= k || want > j)) || (j < k && want <= k && want > j)) {
            g_live[2 * k] = g_live[2 * j], g_live[2 * k + 1] = g_live[2 * j + 1];
            g_live[2 * j] = 0;
            k = j;
        }
    }
}

static void note(void* p, size_t size) {
    if (!p || t_busy || !atomic_load(&g_ready)) return;
    atomic_fetch_add(&g_bytes, size);
    atomic_fetch_add(&g_allocs, 1);
    if (t_until == 0) t_until = (long long)(g_rate / 2 + ((uintptr_t)&t_until >> 4) % g_rate);  // (first in the thread: somewhere in the first stretch)
    t_until -= (long long)size;
    if (t_until > 0) return;
    // Sampled: as many strides as it went past, at least its size.
    size_t weight = g_rate;
    while (t_until <= 0) t_until += (long long)g_rate, weight += g_rate;
    weight -= g_rate;
    if (weight < size) weight = size;
    t_busy = 1;
    void* frames[kFrames + 2];
    const int n = backtrace(frames, kFrames + 2);
    pthread_mutex_lock(&g_lock);
    if (g_count == g_cap) {
        const size_t cap = g_cap ? g_cap * 2 : 4096;
        Sample* s = raw(cap * sizeof(Sample));
        if (s) {
            if (g_samples) memcpy(s, g_samples, g_count * sizeof(Sample)), munmap(g_samples, g_cap * sizeof(Sample));
            g_samples = s, g_cap = cap;
        }
    }
    if (g_count < g_cap) {
        Sample* s = &g_samples[g_count];
        s->ptr = p, s->weight = weight, s->size = size, s->live = 1;
        s->n = n > 2 ? n - 2 : 0;  // (the sampler's own two frames left out)
        memcpy(s->frames, frames + 2, (size_t)s->n * sizeof(void*));
        live_put((uintptr_t)p, g_count);
        ++g_count;
    }
    pthread_mutex_unlock(&g_lock);
    t_busy = 0;
}

static void forget(void* p) {
    if (!p || !atomic_load(&g_ready)) return;
    const uint64_t h = hash_of((uintptr_t)p);
    if (!(atomic_load(&g_filter[(h >> 6) & ((1 << 14) - 1)]) & (1ull << (h & 63)))) return;  // (never sampled: the common case)
    pthread_mutex_lock(&g_lock);
    live_take((uintptr_t)p);
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- written out

static void put(int fd, const char* s, size_t n) {
    while (n) {
        const ssize_t k = write(fd, s, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return;
        }
        s += k, n -= (size_t)k;
    }
}

static void write_out(void) {
    if (atomic_exchange(&g_written, 1)) return;
    const char* path = getenv("BLIT_ALLOC_OUT");
    if (!path || !*path) return;
    t_busy = 1;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    char line[1024];
    int k = snprintf(line, sizeof line, "blit-alloc 1\nrate %zu\ntotal %llu %llu\n", g_rate, (unsigned long long)atomic_load(&g_bytes), (unsigned long long)atomic_load(&g_allocs));
    put(fd, line, (size_t)k);
    pthread_mutex_lock(&g_lock);
    // Each address once: where it is (its image and that image's base), and its symbol if dladdr has one.
    Dl_info self;
    const int have_self = dladdr((void*)&write_out, &self);
    for (size_t i = 0; i < g_count; ++i) {
        const Sample* s = &g_samples[i];
        k = snprintf(line, sizeof line, "s %zu %zu %d", s->weight, s->size, s->live);
        put(fd, line, (size_t)k);
        for (int f = 0; f < s->n; ++f) {
            Dl_info info;
            if (dladdr(s->frames[f], &info) && have_self && info.dli_fbase == self.dli_fbase) continue;  // (the sampler's own)
            k = snprintf(line, sizeof line, " %llx", (unsigned long long)(uintptr_t)s->frames[f]);
            put(fd, line, (size_t)k);
        }
        put(fd, "\n", 1);
    }
    // The images the addresses are in, each address once.
    size_t frames = 0;
    for (size_t i = 0; i < g_count; ++i) frames += (size_t)g_samples[i].n;
    size_t cap = 64;
    while (cap < frames * 2) cap *= 2;
    uintptr_t* seen = raw(cap * sizeof(uintptr_t));
    for (size_t i = 0; i < g_count && seen; ++i)
        for (int f = 0; f < g_samples[i].n; ++f) {
            const uintptr_t at = (uintptr_t)g_samples[i].frames[f];
            size_t h = hash_of(at) & (cap - 1);
            while (seen[h] && seen[h] != at) h = (h + 1) & (cap - 1);
            if (seen[h]) continue;
            seen[h] = at;
            Dl_info info;
            if (!dladdr(g_samples[i].frames[f], &info) || !info.dli_fname) continue;
            k = snprintf(line, sizeof line, "a %llx %llx %s\t%s\n", (unsigned long long)(uintptr_t)g_samples[i].frames[f], (unsigned long long)(uintptr_t)info.dli_fbase,
                         info.dli_fname, info.dli_sname ? info.dli_sname : "");
            put(fd, line, (size_t)k);
        }
    if (seen) munmap(seen, cap * sizeof(uintptr_t));
    pthread_mutex_unlock(&g_lock);
    close(fd);
}

static void on_signal(int sig) {
    write_out();
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor)) static void start(void) {
    const char* rate = getenv("BLIT_ALLOC_RATE");
    if (rate && atol(rate) > 0) g_rate = (size_t)atol(rate);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(write_out);
    atomic_store(&g_ready, 1);
}

__attribute__((destructor)) static void stop(void) { write_out(); }

// ---------------------------------------------------------------- the allocator's names

#if defined(__APPLE__)
// dyld's interposing: each of these takes the place of the system's in
// every image but this one (where the names still mean the system's).
#define INTERPOSE(mine, theirs) \
    __attribute__((used)) static const struct { const void* a; const void* b; } interpose_##theirs __attribute__((section("__DATA,__interpose"))) = {(const void*)&mine, (const void*)&theirs}

static void* my_malloc(size_t n) {
    void* p = REAL_MALLOC(n);
    note(p, n);
    return p;
}
static void* my_calloc(size_t c, size_t n) {
    void* p = REAL_CALLOC(c, n);
    note(p, c * n);
    return p;
}
static void* my_realloc(void* old, size_t n) {
    forget(old);
    void* p = REAL_REALLOC(old, n);
    note(p, n);
    return p;
}
static void my_free(void* p) {
    forget(p);
    REAL_FREE(p);
}
static int my_memalign(void** out, size_t align, size_t n) {
    const int rc = REAL_MEMALIGN(out, align, n);
    if (rc == 0) note(*out, n);
    return rc;
}
INTERPOSE(my_malloc, malloc);
INTERPOSE(my_calloc, calloc);
INTERPOSE(my_realloc, realloc);
INTERPOSE(my_free, free);
INTERPOSE(my_memalign, posix_memalign);
#else
// LD_PRELOAD: these come before libc's, which they call by its own names.
void* malloc(size_t n) {
    void* p = REAL_MALLOC(n);
    note(p, n);
    return p;
}
void* calloc(size_t c, size_t n) {
    void* p = REAL_CALLOC(c, n);
    note(p, c * n);
    return p;
}
void* realloc(void* old, size_t n) {
    forget(old);
    void* p = REAL_REALLOC(old, n);
    note(p, n);
    return p;
}
void free(void* p) {
    forget(p);
    REAL_FREE(p);
}
int posix_memalign(void** out, size_t align, size_t n) {
    void* p = __libc_memalign(align, n);
    if (!p) return ENOMEM;
    *out = p;
    note(p, n);
    return 0;
}
void* aligned_alloc(size_t align, size_t n) {
    void* p = __libc_memalign(align, n);
    note(p, n);
    return p;
}
void* memalign(size_t align, size_t n) {
    void* p = __libc_memalign(align, n);
    note(p, n);
    return p;
}
#endif
