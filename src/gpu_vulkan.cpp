// The GPU everywhere but on Apple's systems: Vulkan. Not written yet: every
// call fails, and blit says so when it starts. See gpu.h for what each one
// is to do, and gpu_metal.m for how the Metal side does it; the Vulkan side
// takes the SPIR-V builds of the same shaders (shaders/gen/*.spv).
//
// What it will need: an instance and a device from the window's surface
// (SDL_Vulkan_CreateSurface), a swapchain of three images in the window's
// format (FIFO for BLIT_PRESENT_VSYNC, IMMEDIATE or MAILBOX for the other),
// a descriptor set with the atlas (R8, sampled, nearest), the cells (a
// storage buffer in memory the CPU can write and the GPU read, one per frame
// under way) and the uniforms (push constants are too small for them: a
// uniform buffer, one block per draw), six pipelines, and a pipeline cache
// kept in cache_dir.
#include "gpu.h"

#include <cstdio>

namespace {
void say(char* err, size_t size) {
    if (err && size) std::snprintf(err, size, "the Vulkan renderer is not written yet");
}
}  // namespace

extern "C" {

BlitGpu* blit_gpu_create(void*, const BlitGpuShaders*, int, int, const char*, char* err, size_t err_size) {
    say(err, err_size);
    return nullptr;
}
void blit_gpu_destroy(BlitGpu*) {}
const char* blit_gpu_name(const BlitGpu*) { return "vulkan (not written yet)"; }
void blit_gpu_setup_times(const BlitGpu*, uint64_t* library_ns, uint64_t* pipelines_ns, int* cached) {
    if (library_ns) *library_ns = 0;
    if (pipelines_ns) *pipelines_ns = 0;
    if (cached) *cached = 0;
}
int blit_gpu_set_present(BlitGpu*, int) { return 0; }
int blit_gpu_set_images(BlitGpu*, int) { return 0; }
int blit_gpu_set_offscreen(BlitGpu*, int, int) { return 0; }
int blit_gpu_set_atlas(BlitGpu*, int, int) { return 0; }
void blit_gpu_atlas_rows(BlitGpu*, const uint8_t*, int, int) {}
uint32_t blit_gpu_image_add(BlitGpu*, const uint8_t*, int, int) { return 0; }
void blit_gpu_image_drop(BlitGpu*, uint32_t) {}
int blit_gpu_begin(BlitGpu*, int, int, int*, int*, BlitGpuTimes*) { return BLIT_FRAME_FAILED; }
uint32_t* blit_gpu_cells(BlitGpu*, uint32_t) { return nullptr; }
int blit_gpu_end(BlitGpu*, const void*, size_t, size_t, const BlitGpuDraw*, uint32_t, BlitGpuTimes*) { return BLIT_FRAME_FAILED; }
void blit_gpu_cancel(BlitGpu*) {}
void blit_gpu_wait_idle(BlitGpu*) {}
int blit_gpu_read_pixels(BlitGpu*, uint8_t*, int, int) { return 0; }

}  // extern "C"
