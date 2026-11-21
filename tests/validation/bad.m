// Metal programs that go wrong on purpose, for blit's samples of Metal's
// API validation (MTL_DEBUG_LAYER=1) and shader validation
// (MTL_SHADER_VALIDATION=1). "api": a render pass's draw with no vertex
// buffer bound where the vertex function reads one (argv[2] times). "shader": a kernel
// that writes past the end of its buffer.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>

static NSString* const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 p [[attribute(0)]]; };\n"
     "vertex float4 vmain(V v [[stage_in]]) { return v.p; }\n"
     "fragment float4 fmain() { return float4(1); }\n"
     "kernel void fill(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
     "    out[i + 4096] = i;\n"
     "}\n";

int main(int argc, char** argv) {
    @autoreleasepool {
        const char* what = argc > 1 ? argv[1] : "api";
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "%s\n", err.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        if (strcmp(what, "shader") == 0) {
            id<MTLComputePipelineState> pipe = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err];
            id<MTLBuffer> buf = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pipe];
            [enc setBuffer:buf offset:0 atIndex:0];
            [enc dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
            [enc endEncoding];
        } else {
            MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[0].format = MTLVertexFormatFloat4;
            vd.attributes[0].bufferIndex = 0;
            vd.layouts[0].stride = 16;
            MTLRenderPipelineDescriptor* rpd = [MTLRenderPipelineDescriptor new];
            rpd.vertexFunction = [lib newFunctionWithName:@"vmain"];
            rpd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
            rpd.vertexDescriptor = vd;
            rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id<MTLRenderPipelineState> pipe = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:8 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = tex;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            const int frames = argc > 2 ? atoi(argv[2]) : 1;
            for (int f = 0; f < frames; ++f) {  // (frame after frame)
                if (f) {
                    [cmd commit];
                    [cmd waitUntilCompleted];
                    cmd = [queue commandBuffer];
                }
                id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
                [enc setRenderPipelineState:pipe];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];  // (no vertex buffer at index 0)
                [enc endEncoding];
            }
        }
        [cmd commit];
        [cmd waitUntilCompleted];
        if (cmd.error) fprintf(stderr, "%s\n", cmd.error.localizedDescription.UTF8String);
        printf("done\n");
    }
    return 0;
}
