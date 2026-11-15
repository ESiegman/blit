/* The GPU, as blit uses it: one window (or an offscreen image), a glyph
 * atlas, the cells of a frame, and a list of draws. A draw is the one
 * triangle that covers the window, cut to a rectangle, through one of a few
 * pipelines, with its own uniforms.
 *
 * Metal on Apple's systems (gpu_metal.m), Vulkan everywhere else
 * (gpu_vulkan.cpp, which is not written yet). The interface is C because
 * the Metal side is Objective-C, built by Apple's compiler: nothing of C++
 * crosses it.
 *
 * A frame: blit_gpu_begin() waits until the memory of a frame is free and
 * says how big the target is; the cells are written straight into the
 * memory the GPU reads (blit_gpu_cells(), no copy); blit_gpu_end() draws
 * and shows the frame. */
#ifndef BLIT_GPU_H
#define BLIT_GPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BlitGpu BlitGpu;

/* The pipelines: the text view's shader and its two other builds, the
 * boxes' shader, its other build, and that one blended over what is
 * there. */
enum {
    BLIT_PIPE_TEXT,
    BLIT_PIPE_TEXT_FX,
    BLIT_PIPE_TEXT_DIM,
    BLIT_PIPE_POPUP,
    BLIT_PIPE_POPUP_FX,
    BLIT_PIPE_POPUP_BLEND,
    BLIT_PIPE_COUNT
};

enum { BLIT_PRESENT_VSYNC, BLIT_PRESENT_IMMEDIATE };
enum { BLIT_FRAME_FAILED = -1, BLIT_FRAME_SKIPPED = 0, BLIT_FRAME_DRAWN = 1 };

/* One shader: compiled (a Metal library, SPIR-V) or source (MSL). */
typedef struct BlitGpuShader {
    const void* code;
    size_t size;
    int source; /* 1: source text, compiled when the device is made */
} BlitGpuShader;

/* The vertex shader, then the fragment shaders of BLIT_PIPE_TEXT to
 * BLIT_PIPE_POPUP_FX (BLIT_PIPE_POPUP_BLEND uses BLIT_PIPE_POPUP_FX's). */
typedef struct BlitGpuShaders {
    BlitGpuShader vertex;
    BlitGpuShader fragment[BLIT_PIPE_POPUP_FX + 1];
} BlitGpuShaders;

typedef struct BlitGpuDraw {
    uint32_t pipeline;
    uint32_t uniforms;         /* which of the frame's uniform blocks */
    int32_t x, y, w, h;        /* the rectangle it is cut to, in pixels */
} BlitGpuDraw;

typedef struct BlitGpuTimes {
    uint64_t free_ns;    /* waiting for a frame's memory to be free (the GPU is behind) */
    uint64_t target_ns;  /* waiting for an image of the window to draw into */
    uint64_t encode_ns;  /* the glyph atlas and the draws, made into commands */
    uint64_t submit_ns;  /* the commands handed to the GPU, the image shown */
} BlitGpuTimes;

/* view: the window's CAMetalLayer (Metal) or what the Vulkan side will
 * want; null for offscreen use only. frames: how many frames may be under
 * way at once (1 to 3). cache_dir: where compiled pipelines are kept from
 * one run to the next (null: they are not). On failure: null, and why in
 * err. */
BlitGpu* blit_gpu_create(void* view, const BlitGpuShaders* shaders, int frames, int debug, const char* cache_dir, char* err, size_t err_size);
void blit_gpu_destroy(BlitGpu* g);
const char* blit_gpu_name(const BlitGpu* g);
/* What making the device took: the shader libraries, the pipelines, and
 * whether the pipelines came from the cache. */
void blit_gpu_setup_times(const BlitGpu* g, uint64_t* library_ns, uint64_t* pipelines_ns, int* cached);

int blit_gpu_set_present(BlitGpu* g, int mode);
/* How many images of the window may be drawn or waiting to be shown (2 or
 * 3). Fewer: what is drawn is on the screen sooner; more: frames drawn one
 * after another keep their pace better. 0 if there is no window. */
int blit_gpu_set_images(BlitGpu* g, int n);
int blit_gpu_set_offscreen(BlitGpu* g, int w, int h); /* 0, 0: the window again */
int blit_gpu_set_atlas(BlitGpu* g, int w, int h);
/* Rows y0 to y1 (not y1) of the atlas have changed; pixels is all of it,
 * one byte a pixel. They reach the GPU with the next frame. */
void blit_gpu_atlas_rows(BlitGpu* g, const uint8_t* pixels, int y0, int y1);

/* want_w, want_h: the window's size in pixels (ignored offscreen). Returns
 * BLIT_FRAME_DRAWN with the target's size, BLIT_FRAME_SKIPPED when there is
 * nothing to draw into (a minimized window), or BLIT_FRAME_FAILED. */
int blit_gpu_begin(BlitGpu* g, int want_w, int want_h, int* w, int* h, BlitGpuTimes* t);
/* Memory for this frame's cells, which the shaders read as they are. Null
 * if it cannot be had. */
uint32_t* blit_gpu_cells(BlitGpu* g, uint32_t count);
/* uniforms: count blocks of `stride` bytes, the draws pick theirs. */
int blit_gpu_end(BlitGpu* g, const void* uniforms, size_t stride, size_t blocks, const BlitGpuDraw* draws, uint32_t count, BlitGpuTimes* t);
/* A frame begun and not to be drawn after all. */
void blit_gpu_cancel(BlitGpu* g);

void blit_gpu_wait_idle(BlitGpu* g);
/* The offscreen target as RGBA bytes (w * h * 4 of them). */
int blit_gpu_read_pixels(BlitGpu* g, uint8_t* rgba, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
