// A program that uses Metal for a while (tests/build.sh's "Capture a
// frame"): a blit fill on its device, committed every 50 ms for 30 s.
#import <Metal/Metal.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    printf("pid %d on %s\n", getpid(), [[dev name] UTF8String]);
    fflush(stdout);
    for (int i = 0; i < 600; ++i) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e fillBuffer:buf range:NSMakeRange(0, 4096) value:(uint8_t)i];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        usleep(50000);
    }
    return 0;
}
