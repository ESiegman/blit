// The folder watched on a Mac: FSEvents, everything under it. (The C++ side
// is watch.cpp; this is the part whose headers only Clang can read.)
#include <CoreServices/CoreServices.h>
#include <stdlib.h>

// what: 1 a folder; 2 it and everything under it to be looked at again.
typedef void (*blit_watch_heard)(void* ctx, const char* path, int what);

typedef struct {
    FSEventStreamRef stream;
    blit_watch_heard heard;
    void* ctx;
} BlitWatch;

// (One queue for all, kept: under ARC it lives as long as the program.)
static dispatch_queue_t queue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("blit.watch", DISPATCH_QUEUE_SERIAL); });
    return q;
}

static void callback(ConstFSEventStreamRef s, void* info, size_t n, void* paths, const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    (void)s;
    (void)ids;
    BlitWatch* w = (BlitWatch*)info;
    char** p = (char**)paths;
    // (Events dropped, by the system or here, or folded together: what
    // changed under the folder named has to be found by looking.)
    const FSEventStreamEventFlags lost = kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped;
    for (size_t i = 0; i < n; ++i)
        w->heard(w->ctx, p[i], (flags[i] & lost) ? 2 : (flags[i] & kFSEventStreamEventFlagItemIsDir) ? 1 : 0);
}

void* blit_watch_start(const char* dir, blit_watch_heard heard, void* ctx) {
    BlitWatch* w = calloc(1, sizeof *w);
    w->heard = heard;
    w->ctx = ctx;
    CFStringRef s = CFStringCreateWithCString(NULL, dir, kCFStringEncodingUTF8);
    CFArrayRef paths = CFArrayCreate(NULL, (const void**)&s, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext c = {0, w, NULL, NULL, NULL};
    w->stream = FSEventStreamCreate(NULL, &callback, &c, paths, kFSEventStreamEventIdSinceNow, 0.05,
                                    kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    CFRelease(s);
    if (!w->stream) {
        free(w);
        return NULL;
    }
    FSEventStreamSetDispatchQueue(w->stream, queue());
    FSEventStreamStart(w->stream);
    return w;
}

// (After it, nothing more is heard: what the queue was doing is waited for.)
void blit_watch_stop(void* p) {
    BlitWatch* w = (BlitWatch*)p;
    if (!w) return;
    FSEventStreamStop(w->stream);
    FSEventStreamInvalidate(w->stream);
    FSEventStreamRelease(w->stream);
    dispatch_sync(queue(), ^{});
    free(w);
}
