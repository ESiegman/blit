// The GPU on Apple's systems: Metal, used straight. See gpu.h.
//
// Each frame under way has memory of its own for the cells, which the CPU
// writes and the GPU reads where it is (shared memory: one memory on Apple
// silicon; a Mac with a GPU of its own reads it across the bus). Uniforms
// go into the commands themselves (setFragmentBytes). The glyph atlas
// lives in GPU memory and is copied to only on the frames that add glyphs.
// Compiled pipelines are kept in a cache folder (a binary archive), so a
// run after the first does not compile them again.
//
// Objective-C with automatic reference counting; built by Apple's
// compiler, which can read Apple's framework headers (GCC cannot).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "gpu.h"

enum { kMaxFrames = 3 };

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void say(char* err, size_t size, NSString* what, NSError* why) {
    if (!err || !size) return;
    NSString* all = why ? [NSString stringWithFormat:@"%@: %@", what, why.localizedDescription] : what;
    snprintf(err, size, "%s", all.UTF8String ? all.UTF8String : "Metal error");
}

@interface BlitMetal : NSObject {
@public
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    CAMetalLayer* layer;
    MTLPixelFormat format;
    id<MTLRenderPipelineState> pipes[BLIT_PIPE_COUNT];
    id<MTLSamplerState> sampler;
    MTLRenderPassDescriptor* pass;  // the same one each frame, with that frame's image put in
    id<MTLTexture> atlas;
    id<MTLTexture> offscreen;
    int off_w, off_h, atlas_w, atlas_h;
    // Per frame under way: its cells, and the atlas rows it carries to the GPU.
    id<MTLBuffer> cells[kMaxFrames];
    id<MTLBuffer> staging[kMaxFrames];
    NSUInteger cells_used;
    int frames, slot;
    dispatch_semaphore_t free_frames;
    BOOL begun;
    id<CAMetalDrawable> drawable;
    id<MTLTexture> target;
    BOOL atlas_pending;
    int atlas_y0, atlas_y1;
    id<MTLCommandBuffer> last;
    char name[160];
    uint64_t library_ns, pipelines_ns;
    int cached;
    dispatch_group_t caching;  // pipelines being put into the cache, after startup
}
@end

@implementation BlitMetal
@end

static BlitMetal* M(const BlitGpu* g) { return (__bridge BlitMetal*)(const void*)g; }

static id<MTLLibrary> library(id<MTLDevice> device, BlitGpuShader s, NSError** e) {
    if (s.source) {
        NSString* src = [[NSString alloc] initWithBytes:s.code length:s.size encoding:NSUTF8StringEncoding];
        if (!src) return nil;
        return [device newLibraryWithSource:src options:nil error:e];  // (the defaults, as SDL_GPU had them)
    }
    dispatch_data_t data = dispatch_data_create(s.code, s.size, dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    return [device newLibraryWithData:data error:e];
}

// FNV-1a, for the name of the cache: the shaders, the GPU, the system.
static uint64_t mix(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

BlitGpu* blit_gpu_create(void* view, const BlitGpuShaders* shaders, int frames, int debug, const char* cache_dir, char* err, size_t err_size) {
    @autoreleasepool {
        if (debug) setenv("MTL_DEBUG_LAYER", "1", 1);
        BlitMetal* m = [BlitMetal new];
        m->layer = (__bridge CAMetalLayer*)view;
        m->device = m->layer.preferredDevice ? m->layer.preferredDevice : MTLCreateSystemDefaultDevice();
        if (!m->device) {
            say(err, err_size, @"no Metal device", nil);
            return NULL;
        }
        m->queue = [m->device newCommandQueue];
        // What SDL_GPU set, which is what the system's own windows use: 8-bit
        // BGRA in sRGB, three images, drawn only by rendering into them.
        m->format = view ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
        if (m->layer) {
            m->layer.device = m->device;
            m->layer.pixelFormat = m->format;
            m->layer.framebufferOnly = YES;
            m->layer.opaque = YES;
            CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            m->layer.colorspace = srgb;
            CGColorSpaceRelease(srgb);
            m->layer.maximumDrawableCount = 3;
            m->layer.displaySyncEnabled = YES;
            m->layer.allowsNextDrawableTimeout = YES;
        }
        m->frames = frames < 1 ? 2 : frames > kMaxFrames ? kMaxFrames : frames;
        m->free_frames = dispatch_semaphore_create(m->frames);
        snprintf(m->name, sizeof m->name, "metal (%s)", m->device.name.UTF8String);

        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
        sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
        m->sampler = [m->device newSamplerStateWithDescriptor:sd];

        // The shaders.
        NSError* e = nil;
        const uint64_t t0 = now_ns();
        id<MTLLibrary> vlib = library(m->device, shaders->vertex, &e);
        id<MTLFunction> vfn = [vlib newFunctionWithName:@"main0"];
        if (!vfn) {
            say(err, err_size, @"the vertex shader", e);
            return NULL;
        }
        id<MTLFunction> ffn[BLIT_PIPE_POPUP_FX + 1];
        for (int i = 0; i <= BLIT_PIPE_POPUP_FX; ++i) {
            id<MTLLibrary> lib = library(m->device, shaders->fragment[i], &e);
            ffn[i] = [lib newFunctionWithName:@"main0"];
            if (!ffn[i]) {
                say(err, err_size, @"a fragment shader", e);
                return NULL;
            }
        }
        const uint64_t t1 = now_ns();
        m->library_ns = t1 - t0;

        // The pipelines, from the cache if they are in it.
        uint64_t key = 1469598103934665603ull;
        key = mix(key, shaders->vertex.code, shaders->vertex.size);
        for (int i = 0; i <= BLIT_PIPE_POPUP_FX; ++i) key = mix(key, shaders->fragment[i].code, shaders->fragment[i].size);
        const char* dev = m->device.name.UTF8String;
        key = mix(key, dev, strlen(dev));
        NSString* os = [NSProcessInfo processInfo].operatingSystemVersionString;
        key = mix(key, os.UTF8String, strlen(os.UTF8String));
        key = mix(key, &m->format, sizeof m->format);
        NSURL* url = nil;
        id<MTLBinaryArchive> archive = nil;
        if (cache_dir && *cache_dir) {
            mkdir(cache_dir, 0755);
            url = [NSURL fileURLWithPath:[NSString stringWithFormat:@"%s/pipelines-%016llx.metallib", cache_dir, (unsigned long long)key]];
            MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
            struct stat st;
            if (stat(url.fileSystemRepresentation, &st) == 0) ad.url = url;
            archive = [m->device newBinaryArchiveWithDescriptor:ad error:nil];
            if (!archive && ad.url) {  // (a cache that will not load is made afresh)
                ad.url = nil;
                archive = [m->device newBinaryArchiveWithDescriptor:ad error:nil];
            }
        }
        NSMutableArray<MTLRenderPipelineDescriptor*>* missing = [NSMutableArray array];
        m->cached = archive != nil;
        for (int p = 0; p < BLIT_PIPE_COUNT; ++p) {
            MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vfn;
            pd.fragmentFunction = ffn[p == BLIT_PIPE_POPUP_BLEND ? BLIT_PIPE_POPUP_FX : p];
            pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
            MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
            ca.pixelFormat = m->format;
            if (p == BLIT_PIPE_POPUP_BLEND) {  // the shader gives colour times alpha: added to what is there, which keeps 1 - alpha
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationAdd;
            }
            if (archive) {
                pd.binaryArchives = @[ archive ];
                m->pipes[p] = [m->device newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
                if (!m->pipes[p]) {
                    m->cached = 0;
                    [missing addObject:pd];
                }
            }
            if (!m->pipes[p]) m->pipes[p] = [m->device newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!m->pipes[p]) {
                say(err, err_size, @"a pipeline", e);
                return NULL;
            }
        }
        m->pipelines_ns = now_ns() - t1;
        // Pipelines that were not in the cache go into it, compiled again
        // for it, on another thread: startup compiles each of them once.
        // (blit_gpu_destroy() waits for this, so that it is written.)
        if (archive && missing.count) {
            m->caching = dispatch_group_create();
            dispatch_group_async(m->caching, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                for (MTLRenderPipelineDescriptor* pd in missing) [archive addRenderPipelineFunctionsWithDescriptor:pd error:nil];
                [archive serializeToURL:url error:nil];
            });
        }
        return (BlitGpu*)(__bridge_retained void*)m;
    }
}

void blit_gpu_destroy(BlitGpu* g) {
    if (!g) return;
    blit_gpu_cancel(g);
    blit_gpu_wait_idle(g);
    if (M(g)->caching) dispatch_group_wait(M(g)->caching, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));  // (the cache, written)
    BlitMetal* m = (__bridge_transfer BlitMetal*)(void*)g;  // (let go of here)
    (void)m;
}

const char* blit_gpu_name(const BlitGpu* g) { return g ? M(g)->name : ""; }

void blit_gpu_setup_times(const BlitGpu* g, uint64_t* library_ns, uint64_t* pipelines_ns, int* cached) {
    BlitMetal* m = M(g);
    if (library_ns) *library_ns = m->library_ns;
    if (pipelines_ns) *pipelines_ns = m->pipelines_ns;
    if (cached) *cached = m->cached;
}

int blit_gpu_set_present(BlitGpu* g, int mode) {
    BlitMetal* m = M(g);
    if (!m->layer) return 0;
    m->layer.displaySyncEnabled = mode == BLIT_PRESENT_VSYNC;
    return 1;
}

int blit_gpu_set_images(BlitGpu* g, int n) {
    BlitMetal* m = M(g);
    if (!m->layer || n < 2 || n > 3) return 0;
    if (m->layer.maximumDrawableCount != (NSUInteger)n) m->layer.maximumDrawableCount = (NSUInteger)n;
    return 1;
}

int blit_gpu_set_offscreen(BlitGpu* g, int w, int h) {
    BlitMetal* m = M(g);
    blit_gpu_wait_idle(g);
    m->offscreen = nil;
    m->off_w = m->off_h = 0;
    if (w <= 0 || h <= 0) return 1;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:m->format width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    m->offscreen = [m->device newTextureWithDescriptor:td];
    if (!m->offscreen) return 0;
    m->off_w = w;
    m->off_h = h;
    return 1;
}

int blit_gpu_set_atlas(BlitGpu* g, int w, int h) {
    BlitMetal* m = M(g);
    blit_gpu_wait_idle(g);  // (frames under way read the old one)
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    m->atlas = [m->device newTextureWithDescriptor:td];
    m->atlas_w = w;
    m->atlas_h = h;
    m->atlas_pending = NO;
    return m->atlas != nil;
}

static id<MTLBuffer> buffer_at_least(BlitMetal* m, id<MTLBuffer> have, NSUInteger bytes) {
    if (have && have.length >= bytes) return have;
    const NSUInteger size = bytes + bytes / 2;
    return [m->device newBufferWithLength:size options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache];
}

void blit_gpu_atlas_rows(BlitGpu* g, const uint8_t* pixels, int y0, int y1) {
    BlitMetal* m = M(g);
    if (!m->atlas || y1 <= y0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > m->atlas_h) y1 = m->atlas_h;
    if (m->atlas_pending) {  // (more rows in the same frame: they go together)
        if (y0 > m->atlas_y0) y0 = m->atlas_y0;
        if (y1 < m->atlas_y1) y1 = m->atlas_y1;
    }
    const NSUInteger row = (NSUInteger)m->atlas_w, bytes = row * (NSUInteger)(y1 - y0);
    if (!m->begun) {
        // Outside a frame: now, and waited for.
        @autoreleasepool {
            id<MTLBuffer> src = [m->device newBufferWithBytes:pixels + (size_t)y0 * row length:bytes options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [m->queue commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:bytes sourceSize:MTLSizeMake(row, (NSUInteger)(y1 - y0), 1)
                    toTexture:m->atlas destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, (NSUInteger)y0, 0)];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            m->last = cb;
        }
        return;
    }
    m->staging[m->slot] = buffer_at_least(m, m->staging[m->slot], bytes);
    memcpy(m->staging[m->slot].contents, pixels + (size_t)y0 * row, bytes);
    m->atlas_pending = YES;
    m->atlas_y0 = y0;
    m->atlas_y1 = y1;
}

int blit_gpu_begin(BlitGpu* g, int want_w, int want_h, int* w, int* h, BlitGpuTimes* t) {
    BlitMetal* m = M(g);
    if (m->begun) blit_gpu_cancel(g);
    const uint64_t t0 = now_ns();
    dispatch_semaphore_wait(m->free_frames, DISPATCH_TIME_FOREVER);
    const uint64_t t1 = now_ns();
    uint64_t t2 = t1;
    if (m->offscreen) {
        m->target = m->offscreen;
    } else if (m->layer) {
        @autoreleasepool {
            if (want_w > 0 && want_h > 0 && (m->layer.drawableSize.width != want_w || m->layer.drawableSize.height != want_h))
                m->layer.drawableSize = CGSizeMake(want_w, want_h);
            m->drawable = [m->layer nextDrawable];
        }
        t2 = now_ns();
        if (!m->drawable) {  // minimized, hidden, or no image came in time
            dispatch_semaphore_signal(m->free_frames);
            if (t) t->free_ns = t1 - t0, t->target_ns = t2 - t1;
            return BLIT_FRAME_SKIPPED;
        }
        m->target = m->drawable.texture;
    } else {
        dispatch_semaphore_signal(m->free_frames);
        return BLIT_FRAME_FAILED;
    }
    m->begun = YES;
    m->cells_used = 0;
    *w = (int)m->target.width;
    *h = (int)m->target.height;
    if (t) t->free_ns = t1 - t0, t->target_ns = t2 - t1;
    return BLIT_FRAME_DRAWN;
}

uint32_t* blit_gpu_cells(BlitGpu* g, uint32_t count) {
    BlitMetal* m = M(g);
    if (!m->begun) return NULL;
    const NSUInteger bytes = (NSUInteger)(count ? count : 1) * sizeof(uint32_t);
    m->cells[m->slot] = buffer_at_least(m, m->cells[m->slot], bytes);
    if (!m->cells[m->slot]) return NULL;
    m->cells_used = bytes;
    return (uint32_t*)m->cells[m->slot].contents;
}

void blit_gpu_cancel(BlitGpu* g) {
    BlitMetal* m = M(g);
    if (!m->begun) return;
    m->begun = NO;
    m->drawable = nil;  // (an image not shown goes back to the layer)
    m->target = nil;
    if (m->atlas_pending) {
        // Glyph rows staged for this frame go by themselves (the font has
        // forgotten they changed). Frames after this one come after them.
        @autoreleasepool {
            const NSUInteger row = (NSUInteger)m->atlas_w, rows = (NSUInteger)(m->atlas_y1 - m->atlas_y0);
            id<MTLCommandBuffer> cb = [m->queue commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:m->staging[m->slot] sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * rows sourceSize:MTLSizeMake(row, rows, 1)
                    toTexture:m->atlas destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, (NSUInteger)m->atlas_y0, 0)];
            [b endEncoding];
            dispatch_semaphore_t done = m->free_frames;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
                (void)c;
                dispatch_semaphore_signal(done);
            }];
            [cb commit];
            m->last = cb;
            m->atlas_pending = NO;
            m->slot = (m->slot + 1) % m->frames;  // (its staging buffer is in use until then)
        }
        return;
    }
    dispatch_semaphore_signal(m->free_frames);
}

int blit_gpu_end(BlitGpu* g, const void* uniforms, size_t stride, size_t blocks, const BlitGpuDraw* draws, uint32_t count, BlitGpuTimes* t) {
    BlitMetal* m = M(g);
    if (!m->begun) return BLIT_FRAME_FAILED;
    const uint64_t t0 = now_ns();
    const int slot = m->slot;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [m->queue commandBufferWithUnretainedReferences];
        if (m->atlas_pending) {
            const NSUInteger row = (NSUInteger)m->atlas_w, rows = (NSUInteger)(m->atlas_y1 - m->atlas_y0);
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:m->staging[slot] sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * rows sourceSize:MTLSizeMake(row, rows, 1)
                    toTexture:m->atlas destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, (NSUInteger)m->atlas_y0, 0)];
            [b endEncoding];
            m->atlas_pending = NO;
        }

        if (!m->pass) {
            m->pass = [MTLRenderPassDescriptor renderPassDescriptor];
            m->pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;  // every pixel is written
            m->pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
        m->pass.colorAttachments[0].texture = m->target;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:m->pass];
        m->pass.colorAttachments[0].texture = nil;  // (the image is not kept past its frame)
        [e setFragmentTexture:m->atlas atIndex:0];
        [e setFragmentSamplerState:m->sampler atIndex:0];
        [e setFragmentBuffer:m->cells[slot] offset:0 atIndex:1];
        const NSInteger tw = (NSInteger)m->target.width, th = (NSInteger)m->target.height;
        uint32_t bound = ~0u;
        size_t given = (size_t)-1;  // the uniform block the last draw was given: the next with the same one is not given it again
        const uint8_t* u = (const uint8_t*)uniforms;
        for (uint32_t i = 0; i < count; ++i) {
            const BlitGpuDraw* d = &draws[i];
            NSInteger x0 = d->x, y0 = d->y, x1 = (NSInteger)d->x + d->w, y1 = (NSInteger)d->y + d->h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > tw) x1 = tw;
            if (y1 > th) y1 = th;
            if (x1 <= x0 || y1 <= y0 || d->pipeline >= BLIT_PIPE_COUNT || d->uniforms >= blocks) continue;
            if (d->pipeline != bound) {
                bound = d->pipeline;
                [e setRenderPipelineState:m->pipes[bound]];
            }
            if (d->uniforms != given) {
                given = d->uniforms;
                [e setFragmentBytes:u + (size_t)d->uniforms * stride length:stride atIndex:0];
            }
            [e setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [e endEncoding];
        const uint64_t t1 = now_ns();
        id<CAMetalDrawable> shown = m->drawable;
        if (shown) [cb presentDrawable:shown];
        // The command buffer does not keep what it uses alive (it is cheaper
        // that way): this does, until the GPU is done with it.
        dispatch_semaphore_t done = m->free_frames;
        id<MTLBuffer> cells = m->cells[slot], staged = m->staging[slot];
        id<MTLTexture> atlas = m->atlas, target = m->target;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            (void)c;
            (void)shown, (void)cells, (void)staged, (void)atlas, (void)target;
            dispatch_semaphore_signal(done);
        }];
        [cb commit];
        m->last = cb;
        if (t) t->encode_ns = t1 - t0, t->submit_ns = now_ns() - t1;
    }
    m->drawable = nil;
    m->target = nil;
    m->begun = NO;
    m->slot = (m->slot + 1) % m->frames;
    return BLIT_FRAME_DRAWN;
}

void blit_gpu_wait_idle(BlitGpu* g) {
    BlitMetal* m = M(g);
    if (m->last) [m->last waitUntilCompleted];
}

int blit_gpu_read_pixels(BlitGpu* g, uint8_t* rgba, int w, int h) {
    BlitMetal* m = M(g);
    if (!m->offscreen || w != m->off_w || h != m->off_h) return 0;
    blit_gpu_wait_idle(g);
    @autoreleasepool {
        const NSUInteger row = (NSUInteger)w * 4, bytes = row * (NSUInteger)h;
        id<MTLBuffer> out = [m->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [m->queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:m->offscreen sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                  toBuffer:out destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:bytes];
        [b endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        memcpy(rgba, out.contents, bytes);
    }
    if (m->format == MTLPixelFormatBGRA8Unorm)
        for (size_t i = 0; i < (size_t)w * (size_t)h * 4; i += 4) {
            const uint8_t r = rgba[i + 2];
            rgba[i + 2] = rgba[i];
            rgba[i] = r;
        }
    return 1;
}
