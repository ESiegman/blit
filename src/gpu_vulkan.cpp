// The GPU everywhere but on Apple's systems: Vulkan, used straight. See
// gpu.h, and gpu_metal.m, which this follows step for step.
//
// Each frame under way has memory of its own for the cells and the
// uniforms, which the CPU writes and the GPU reads where they are (memory
// both can see: the cells in the CPU's, cached, since making them reads
// them back too; the uniforms in the GPU's where the CPU may write there,
// as they are only copied in). The glyph
// atlas lives in GPU memory and is copied to only on the frames that add
// glyphs, from rows the frame carries in a staging buffer of its own.
// Compiled pipelines are kept in a cache folder (a pipeline cache's data),
// so a run after the first does not compile them again. Pictures are images
// of their own, with their smaller copies (mipmaps), sampled smoothly.
//
// The window is SDL's (SDL_WINDOW_VULKAN): a surface made from it, and a
// swapchain made again when the window's size, the present mode or the
// number of images changes. How many images may be drawn or waiting to be
// shown (gpu.h's blit_gpu_set_images) is not the swapchain's size, which
// would mean making it again each time the view starts or stops scrolling:
// a frame is begun only once the one shown before the last (or, with two,
// the last) has reached the screen (VK_KHR_present_wait), which is what
// Metal's limit on drawables does.
//
// The shaders are the SPIR-V builds of the same sources (shaders/gen/*.spv),
// and their set and binding numbers are SDL_GPU's: set 2 has the atlas (or
// a picture) and the cells, set 3 the uniforms. Sets 0 and 1 are empty.
#include "gpu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Vulkan's structures are made with their type alone and the rest filled in
// by name: what is not named starts at zero, as it should.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {

constexpr int kMaxFrames = 3;

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void say(char* err, size_t size, const char* what, VkResult why = VK_SUCCESS) {
    if (!err || !size) return;
    if (why != VK_SUCCESS) std::snprintf(err, size, "%s (VkResult %d)", what, static_cast<int>(why));
    else std::snprintf(err, size, "%s", what);
}

// FNV-1a, for the name of the cache: the shaders, the GPU, its driver.
uint64_t mix(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct Picture {
    Image im;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint64_t last_frame = 0;      // the last frame that draws it
    VkFence upload = VK_NULL_HANDLE;  // its copy and mipmaps, until they are done (owned by the upload)
};

// Staging memory a picture's copy is done with: a few kept, each for the
// next picture (a minimap's tile is about a megabyte), and a bigger one
// (a page, a photograph) let go.
constexpr size_t kSpareStagings = 4;
constexpr VkDeviceSize kSpareStagingMost = 4 << 20;

// A copy to the GPU that is not waited for (a picture's): freed once done.
struct Upload {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    Buffer staging;
};

// What each frame under way has of its own.
struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    bool pending = false;     // submitted, and not yet waited for
    uint64_t serial = 0;      // which frame it was
    VkSemaphore acquired = VK_NULL_HANDLE;  // signalled when its image of the window was free to draw into
    Buffer cells, uniforms, staging;
    VkDescriptorSet text_set = VK_NULL_HANDLE;     // set 2: the atlas and the cells
    VkDescriptorSet uniform_set = VK_NULL_HANDLE;  // set 3: the uniforms, at an offset per draw
    bool text_stale = true, uniform_stale = true;
};

}  // namespace

struct BlitGpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    SDL_Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    VkDeviceSize ubo_align = 256;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    // (All compatible: the pipelines are made with one. A window's image is
    // drawn from nothing the first time, and from as it was shown after.)
    VkRenderPass pass_window = VK_NULL_HANDLE, pass_shown = VK_NULL_HANDLE, pass_offscreen = VK_NULL_HANDLE;
    VkDescriptorSetLayout empty_layout = VK_NULL_HANDLE, text_layout = VK_NULL_HANDLE, uniform_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipes[BLIT_PIPE_COUNT] = {};
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, smooth = VK_NULL_HANDLE;  // (the atlas's, pictures')
    VkDescriptorPool frame_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> picture_pools;
    VkCommandPool commands = VK_NULL_HANDLE;

    // The window's images.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> drawn;  // per image: signalled when it is drawn, waited for by its present
    std::vector<uint8_t> shown;      // per image: drawn and presented before, on this swapchain
    int sc_w = 0, sc_h = 0;
    bool stale = true;                // the swapchain is to be made again before the next frame
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> modes;  // what the surface offers
    int max_images = 3;               // of the window's, drawn or waiting to be shown
    bool present_wait = false;
    PFN_vkWaitForPresentKHR wait_for_present = nullptr;  // (an extension's: the loader does not export it)
    uint64_t present_id = 0;          // the last frame presented, on this swapchain
    // An image acquired for a frame that was not drawn after all: the next
    // frame draws into it (Vulkan has no giving one back).
    bool held = false;
    uint32_t held_index = 0;
    VkSemaphore held_sem = VK_NULL_HANDLE;
    std::vector<VkSemaphore> free_sems;  // for acquiring images

    Image offscreen;
    VkFramebuffer off_fb = VK_NULL_HANDLE;
    int off_w = 0, off_h = 0;

    Image atlas;
    int atlas_w = 0, atlas_h = 0;
    Image colours;  // (colour emoji: the font's colour slots, RGBA)
    int colours_w = 0, colours_h = 0;
    bool atlas_pending = false;
    int atlas_y0 = 0, atlas_y1 = 0;

    Frame frames[kMaxFrames];
    int nframes = 2, slot = 0;
    uint64_t serial = 0, done_serial = 0;  // frames submitted, and known to be finished
    bool begun = false, acquired = false;
    uint32_t index = 0;  // the window's image this frame draws into
    VkFramebuffer target = VK_NULL_HANDLE;
    int target_w = 0, target_h = 0;

    std::unordered_map<uint32_t, Picture> pictures;
    std::vector<Picture> dropped;  // done with, but perhaps still drawn by frames under way
    uint32_t next_picture = 0;
    std::vector<Upload> uploads;
    std::vector<Buffer> spare_stagings;  // (see kSpareStagings)

    char name[288] = "vulkan";  // (room for the longest name a device may have)
    uint64_t library_ns = 0, pipelines_ns = 0;
    int cached = 0;
    std::thread caching;  // the pipeline cache, written after startup
};

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::fprintf(stderr, "blit: vulkan %s: %s\n", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", data->pMessage);
    return VK_FALSE;
}

int memory_type(const BlitGpu* g, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want) {
    for (int pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags flags = pass == 0 ? need | want : need;
        for (uint32_t i = 0; i < g->memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (g->memory.memoryTypes[i].propertyFlags & flags) == flags) return static_cast<int>(i);
    }
    return -1;
}

void buffer_free(BlitGpu* g, Buffer& b) {
    if (b.buf) vkDestroyBuffer(g->dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(g->dev, b.mem, nullptr);
    b = Buffer{};
}

// A buffer the CPU writes (or, `cpu_reads`, reads as well) and the GPU
// uses, mapped for as long as it lives.
bool buffer_make(BlitGpu* g, Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool cpu_reads = false) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g->dev, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->dev, b.buf, &req);
    // In the GPU's own memory where the CPU may write it there, unless the
    // CPU reads it too: that memory is written through, uncached, across
    // the bus, and reading it back costs a trip each time (the cells of a
    // 2560x1600 frame took 570 µs to make there, against a few here).
    const int type = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 cpu_reads ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (type < 0 || vkAllocateMemory(g->dev, &ai, nullptr, &b.mem) != VK_SUCCESS || vkBindBufferMemory(g->dev, b.buf, b.mem, 0) != VK_SUCCESS ||
        vkMapMemory(g->dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map) != VK_SUCCESS) {
        buffer_free(g, b);
        return false;
    }
    b.size = size;
    return true;
}

// The buffer, at least `bytes` long: made larger (half again what is asked)
// if it is not, and `made` set when it was (a descriptor set that points at
// it is to be written again: the new one's handle may well be the old
// one's, the driver reusing what it freed, so comparing handles does not
// tell). Only for a buffer no frame under way uses.
bool buffer_at_least(BlitGpu* g, Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage, bool cpu_reads = false, bool* made = nullptr) {
    if (b.buf && b.size >= bytes) return true;
    buffer_free(g, b);
    if (made) *made = true;
    return buffer_make(g, b, bytes + bytes / 2, usage, cpu_reads);
}

void image_free(BlitGpu* g, Image& im) {
    if (im.view) vkDestroyImageView(g->dev, im.view, nullptr);
    if (im.img) vkDestroyImage(g->dev, im.img, nullptr);
    if (im.mem) vkFreeMemory(g->dev, im.mem, nullptr);
    im = Image{};
}

bool image_make(BlitGpu* g, Image& im, VkFormat format, int w, int h, uint32_t levels, VkImageUsageFlags usage) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g->dev, &ii, nullptr, &im.img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g->dev, im.img, &req);
    const int type = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (type < 0 || vkAllocateMemory(g->dev, &ai, nullptr, &im.mem) != VK_SUCCESS || vkBindImageMemory(g->dev, im.img, im.mem, 0) != VK_SUCCESS) {
        image_free(g, im);
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (vkCreateImageView(g->dev, &vi, nullptr, &im.view) != VK_SUCCESS) {
        image_free(g, im);
        return false;
    }
    return true;
}

void barrier(VkCommandBuffer cmd, VkImage img, uint32_t level, uint32_t levels, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkCommandBuffer command_buffer(BlitGpu* g) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g->commands;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g->dev, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

VkFence fence(BlitGpu* g) {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    vkCreateFence(g->dev, &fi, nullptr, &f);
    return f;
}

// Commands outside the frames (setting things up, reading the offscreen
// image back), run now and waited for.
bool run_now(BlitGpu* g, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkFence f = fence(g);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const bool ok = f && vkQueueSubmit(g->queue, 1, &si, f) == VK_SUCCESS && vkWaitForFences(g->dev, 1, &f, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    if (f) vkDestroyFence(g->dev, f, nullptr);
    vkFreeCommandBuffers(g->dev, g->commands, 1, &cmd);
    return ok;
}

VkSemaphore semaphore(BlitGpu* g) {
    if (!g->free_sems.empty()) {
        VkSemaphore s = g->free_sems.back();
        g->free_sems.pop_back();
        return s;
    }
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(g->dev, &si, nullptr, &s);
    return s;
}

void picture_free(BlitGpu* g, Picture& p) {
    if (p.set) vkFreeDescriptorSets(g->dev, p.pool, 1, &p.set);
    image_free(g, p.im);
    p = Picture{};
}

// A finished copy's staging memory: kept for the next picture, in place of
// a smaller one kept if there are as many as are kept, or let go.
void spare_staging(BlitGpu* g, Buffer& b) {
    std::vector<Buffer>& spare = g->spare_stagings;
    if (b.size <= kSpareStagingMost && spare.size() < kSpareStagings) {
        spare.push_back(b);
        b = Buffer{};
        return;
    }
    for (Buffer& kept : spare)
        if (b.size <= kSpareStagingMost && kept.size < b.size) std::swap(kept, b);
    buffer_free(g, b);
}

// Staging memory for a picture of `bytes`: the smallest kept one that is
// large enough, else new. (Made afresh each time, its memory was allocated
// and mapped for every tile of the minimap as the view jumped.)
bool take_staging(BlitGpu* g, Buffer& b, VkDeviceSize bytes) {
    std::vector<Buffer>& spare = g->spare_stagings;
    size_t pick = spare.size();
    for (size_t k = 0; k < spare.size(); ++k)
        if (spare[k].size >= bytes && (pick == spare.size() || spare[k].size < spare[pick].size)) pick = k;
    if (pick == spare.size()) return buffer_make(g, b, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    b = spare[pick];
    spare[pick] = spare.back();
    spare.pop_back();
    return true;
}

// What frames and copies have finished: their memory and the pictures
// dropped since are let go.
void collect(BlitGpu* g) {
    for (size_t i = 0; i < g->uploads.size();) {
        Upload& u = g->uploads[i];
        if (vkGetFenceStatus(g->dev, u.done) != VK_SUCCESS) {
            ++i;
            continue;
        }
        for (auto& [id, p] : g->pictures)
            if (p.upload == u.done) p.upload = VK_NULL_HANDLE;
        for (Picture& p : g->dropped)
            if (p.upload == u.done) p.upload = VK_NULL_HANDLE;
        vkDestroyFence(g->dev, u.done, nullptr);
        vkFreeCommandBuffers(g->dev, g->commands, 1, &u.cmd);
        spare_staging(g, u.staging);
        g->uploads[i] = g->uploads.back();
        g->uploads.pop_back();
    }
    for (size_t i = 0; i < g->dropped.size();) {
        Picture& p = g->dropped[i];
        if (p.upload || p.last_frame > g->done_serial) {
            ++i;
            continue;
        }
        picture_free(g, p);
        g->dropped[i] = g->dropped.back();
        g->dropped.pop_back();
    }
}

// Waits for a frame's slot to be free again.
void finish(BlitGpu* g, Frame& f) {
    if (!f.pending) return;
    vkWaitForFences(g->dev, 1, &f.done, VK_TRUE, UINT64_MAX);
    vkResetFences(g->dev, 1, &f.done);
    f.pending = false;
    g->done_serial = std::max(g->done_serial, f.serial);
    if (f.acquired) {
        g->free_sems.push_back(f.acquired);
        f.acquired = VK_NULL_HANDLE;
    }
}

void idle(BlitGpu* g) {
    vkQueueWaitIdle(g->queue);
    for (Frame& f : g->frames) finish(g, f);
    g->done_serial = g->serial;
    collect(g);
}

// An image held from a frame not drawn: its acquire is waited out, and it
// goes back with the swapchain it belongs to.
void drop_held(BlitGpu* g) {
    if (!g->held) return;
    g->held = false;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g->held_sem;
    si.pWaitDstStageMask = &stage;
    vkQueueSubmit(g->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g->queue);
    g->free_sems.push_back(g->held_sem);
    g->held_sem = VK_NULL_HANDLE;
}

void swapchain_free_views(BlitGpu* g) {
    for (VkFramebuffer fb : g->framebuffers) vkDestroyFramebuffer(g->dev, fb, nullptr);
    for (VkImageView v : g->views) vkDestroyImageView(g->dev, v, nullptr);
    g->framebuffers.clear();
    g->views.clear();
    g->images.clear();
}

VkFramebuffer framebuffer(BlitGpu* g, VkRenderPass pass, VkImageView view, int w, int h) {
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &view;
    fi.width = static_cast<uint32_t>(w);
    fi.height = static_cast<uint32_t>(h);
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(g->dev, &fi, nullptr, &fb);
    return fb;
}

// The swapchain, made (again) at the window's size: 0 if it cannot be,
// -1 if there is nothing to draw into (a minimized window).
int swapchain_make(BlitGpu* g, int want_w, int want_h) {
    drop_held(g);
    idle(g);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->phys, g->surface, &caps) != VK_SUCCESS) return 0;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {  // (the window takes the size it is given: Wayland)
        extent.width = static_cast<uint32_t>(std::max(want_w, 1));
        extent.height = static_cast<uint32_t>(std::max(want_h, 1));
    }
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));
    if (extent.width == 0 || extent.height == 0 || want_w <= 0 || want_h <= 0) return -1;
    // Three images (as Metal has them), or as near as the surface allows.
    uint32_t count = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface = g->surface;
    si.minImageCount = count;
    si.imageFormat = g->format;
    si.imageColorSpace = g->color_space;
    si.imageExtent = extent;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) {
            si.compositeAlpha = a;
            break;
        }
    si.presentMode = g->present;
    si.clipped = VK_TRUE;
    si.oldSwapchain = g->swapchain;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(g->dev, &si, nullptr, &made);
    swapchain_free_views(g);
    if (g->swapchain) vkDestroySwapchainKHR(g->dev, g->swapchain, nullptr);
    g->swapchain = made;
    g->present_id = 0;
    if (r != VK_SUCCESS) return 0;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g->dev, made, &n, nullptr);
    g->images.resize(n);
    vkGetSwapchainImagesKHR(g->dev, made, &n, g->images.data());
    g->shown.assign(n, 0);
    for (VkImage img : g->images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g->format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(g->dev, &vi, nullptr, &v) != VK_SUCCESS) return 0;
        g->views.push_back(v);
        g->framebuffers.push_back(framebuffer(g, g->pass_window, v, static_cast<int>(extent.width), static_cast<int>(extent.height)));
    }
    // (Kept from one swapchain to the next: a present may still be waiting
    // on one when the swapchain it was for goes.)
    while (g->drawn.size() < n) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(g->dev, &sci, nullptr, &s);
        g->drawn.push_back(s);
    }
    g->sc_w = static_cast<int>(extent.width);
    g->sc_h = static_cast<int>(extent.height);
    g->stale = false;
    return 1;
}

// shown: a window's image that has been presented before, which is drawn
// over from that layout (from UNDEFINED, RADV makes the image's compression
// data afresh at every frame: beginning the pass took 10 µs more).
VkRenderPass render_pass(BlitGpu* g, bool window, bool shown = false) {
    VkAttachmentDescription a{};
    a.format = g->format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = shown ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = window ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // In: after the image is free (the window's: its acquire is waited for at
    // this stage) or, offscreen, after the frames before have written it and
    // it has been read back. Out, offscreen: before it is read back. The
    // same two for every pass: passes whose dependencies differ are not
    // compatible, and the pipelines, made with the offscreen one, draw in all.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = 1;
    ri.pAttachments = &a;
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 2;
    ri.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    vkCreateRenderPass(g->dev, &ri, nullptr, &pass);
    return pass;
}

bool has_extension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const VkExtensionProperties& e : all)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// A device that draws, and shows in the window's surface if there is one;
// a GPU of its own first, then one built in, then the rest (a CPU's).
bool pick_device(BlitGpu* g) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g->instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(g->instance, &n, all.data());
    int best = -1;
    for (VkPhysicalDevice pd : all) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        int family = -1;
        for (uint32_t i = 0; i < qn && family < 0; ++i) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 shows = VK_TRUE;
            if (g->surface) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, g->surface, &shows);
            if (shows) family = static_cast<int>(i);
        }
        if (family < 0) continue;
        const int rank = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                         : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                         : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                      : 1;
        if (rank > best) {
            best = rank;
            g->phys = pd;
            g->family = static_cast<uint32_t>(family);
        }
    }
    return g->phys != VK_NULL_HANDLE;
}

VkShaderModule shader_module(BlitGpu* g, const BlitGpuShader& s) {
    // (SPIR-V is read as 32-bit words, and the bytes baked into the program
    // need not be aligned for that.)
    std::vector<uint32_t> words((s.size + 3) / 4);
    std::memcpy(words.data(), s.code, s.size);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = s.size;
    mi.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g->dev, &mi, nullptr, &m);
    return m;
}

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        size_t k;
        while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, k);
        std::fclose(f);
    }
    return out;
}

void write_file(const std::string& path, const void* data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    if (std::fclose(f) == 0 && ok) std::rename(tmp.c_str(), path.c_str());
    else std::remove(tmp.c_str());
}

// The descriptor sets of a frame, pointed at what it has now (the atlas,
// its cells, its uniforms), where those were made since it last drew.
void frame_sets(BlitGpu* g, Frame& f, size_t stride) {
    VkWriteDescriptorSet w[4] = {};
    VkDescriptorImageInfo atlas{g->sampler, g->atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo colours{g->sampler, g->colours.view ? g->colours.view : g->atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo cells{f.cells.buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo uniforms{f.uniforms.buf, 0, stride};
    uint32_t n = 0;
    if (f.text_stale) {
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = f.text_set;
        w[n].dstBinding = 0;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n++].pImageInfo = &atlas;
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = f.text_set;
        w[n].dstBinding = 1;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[n++].pBufferInfo = &cells;
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = f.text_set;
        w[n].dstBinding = 2;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n++].pImageInfo = &colours;
        f.text_stale = false;
    }
    if (f.uniform_stale) {
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstSet = f.uniform_set;
        w[n].dstBinding = 0;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[n++].pBufferInfo = &uniforms;
        f.uniform_stale = false;
    }
    if (n) vkUpdateDescriptorSets(g->dev, n, w, 0, nullptr);
}

// The atlas rows staged for this frame, copied into the atlas ahead of the
// draws (after the frames before have finished reading it).
void record_atlas(BlitGpu* g, VkCommandBuffer cmd, const Buffer& staging) {
    barrier(cmd, g->atlas.img, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {0, g->atlas_y0, 0};
    c.imageExtent = {static_cast<uint32_t>(g->atlas_w), static_cast<uint32_t>(g->atlas_y1 - g->atlas_y0), 1};
    vkCmdCopyBufferToImage(cmd, staging.buf, g->atlas.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cmd, g->atlas.img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    g->atlas_pending = false;
}

}  // namespace

extern "C" {

BlitGpu* blit_gpu_create(void* view, const BlitGpuShaders* shaders, int frames, int debug, const char* cache_dir, char* err, size_t err_size) {
    BlitGpu* g = new BlitGpu;
    g->window = static_cast<SDL_Window*>(view);
    g->nframes = frames < 1 ? 2 : frames > kMaxFrames ? kMaxFrames : frames;
    auto fail = [&](const char* what, VkResult why = VK_SUCCESS) -> BlitGpu* {
        say(err, err_size, what, why);
        blit_gpu_destroy(g);
        return nullptr;
    };

    // The instance: what the window's surface needs, and the validation
    // layer when asked for (--debug-gpu) and installed.
    std::vector<const char*> exts;
    if (g->window) {
        Uint32 n = 0;
        const char* const* want = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!want) return fail("the window system has no Vulkan surfaces");
        exts.assign(want, want + n);
    }
    std::vector<const char*> layers;
    if (debug) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> all(n);
        vkEnumerateInstanceLayerProperties(&n, all.data());
        bool found = false;
        for (const VkLayerProperties& l : all) found = found || std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        if (found) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            std::fprintf(stderr, "blit: --debug-gpu: the Vulkan validation layers are not installed (VK_LAYER_KHRONOS_validation)\n");
        }
    }
    uint32_t version = VK_API_VERSION_1_1;
    vkEnumerateInstanceVersion(&version);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "blit";
    app.pEngineName = "blit";
    app.apiVersion = std::min<uint32_t>(version, VK_API_VERSION_1_3);
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (VkResult r = vkCreateInstance(&ici, nullptr, &g->instance); r != VK_SUCCESS) return fail("no Vulkan instance", r);
    if (!layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = on_message;
        auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g->instance, "vkCreateDebugUtilsMessengerEXT"));
        if (make) make(g->instance, &mi, nullptr, &g->messenger);
    }
    if (g->window && !SDL_Vulkan_CreateSurface(g->window, g->instance, nullptr, &g->surface)) return fail("cannot make a Vulkan surface for the window");
    if (!pick_device(g)) return fail("no Vulkan device that can draw (and show in the window)");

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g->phys, &props);
    vkGetPhysicalDeviceMemoryProperties(g->phys, &g->memory);
    g->ubo_align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 4);
    std::snprintf(g->name, sizeof g->name, "vulkan (%s)", props.deviceName);

    // The device: a queue that draws (and shows), the swapchain, waiting for
    // presents where there is that, and word of which pipelines came from
    // the cache.
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(g->phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> dev_exts(en);
    vkEnumerateDeviceExtensionProperties(g->phys, nullptr, &en, dev_exts.data());
    std::vector<const char*> want_exts;
    if (g->surface) want_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const bool feedback = props.apiVersion >= VK_API_VERSION_1_3 || has_extension(dev_exts, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    if (feedback && props.apiVersion < VK_API_VERSION_1_3) want_exts.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
    VkPhysicalDevicePresentIdFeaturesKHR id_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR wait_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    if (g->surface && has_extension(dev_exts, VK_KHR_PRESENT_ID_EXTENSION_NAME) && has_extension(dev_exts, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
        id_features.pNext = &wait_features;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &id_features;
        vkGetPhysicalDeviceFeatures2(g->phys, &f2);
        if (id_features.presentId && wait_features.presentWait) {
            g->present_wait = true;
            want_exts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            want_exts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
    }
    id_features.pNext = &wait_features;
    wait_features.pNext = nullptr;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g->family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = g->present_wait ? &id_features : nullptr;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = static_cast<uint32_t>(want_exts.size());
    di.ppEnabledExtensionNames = want_exts.data();
    if (VkResult r = vkCreateDevice(g->phys, &di, nullptr, &g->dev); r != VK_SUCCESS) return fail("no Vulkan device", r);
    vkGetDeviceQueue(g->dev, g->family, 0, &g->queue);
    if (g->present_wait) {
        g->wait_for_present = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(g->dev, "vkWaitForPresentKHR"));
        g->present_wait = g->wait_for_present != nullptr;
    }

    // The window's format: 8-bit BGRA (as Metal's), written as the shaders
    // give it; offscreen RGBA.
    if (g->surface) {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(g->phys, g->surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(g->phys, g->surface, &n, formats.data());
        if (formats.empty()) return fail("the window's surface has no formats");
        g->format = formats[0].format;
        g->color_space = formats[0].colorSpace;
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            const auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
                return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            if (it != formats.end()) {
                g->format = it->format;
                g->color_space = it->colorSpace;
                break;
            }
        }
        vkGetPhysicalDeviceSurfacePresentModesKHR(g->phys, g->surface, &n, nullptr);
        g->modes.resize(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g->phys, g->surface, &n, g->modes.data());
    }
    g->pass_window = g->surface ? render_pass(g, true) : VK_NULL_HANDLE;
    g->pass_shown = g->surface ? render_pass(g, true, true) : VK_NULL_HANDLE;
    g->pass_offscreen = render_pass(g, false);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g->family;
    if (vkCreateCommandPool(g->dev, &pi, nullptr, &g->commands) != VK_SUCCESS) return fail("no command pool");

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0;
    vkCreateSampler(g->dev, &sci, nullptr, &g->sampler);
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(g->dev, &sci, nullptr, &g->smooth);

    // The layout every pipeline shares: sets 0 and 1 empty, set 2 the
    // atlas (or a picture) and the cells, set 3 the uniforms.
    VkDescriptorSetLayoutBinding text[3] = {};
    text[0].binding = 0;
    text[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    text[0].descriptorCount = 1;
    text[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    text[1].binding = 1;
    text[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    text[1].descriptorCount = 1;
    text[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    text[2].binding = 2;  // (the colour atlas)
    text[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    text[2].descriptorCount = 1;
    text[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding uniform{};
    uniform.binding = 0;
    uniform.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniform.descriptorCount = 1;
    uniform.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    vkCreateDescriptorSetLayout(g->dev, &li, nullptr, &g->empty_layout);
    li.bindingCount = 3;
    li.pBindings = text;
    vkCreateDescriptorSetLayout(g->dev, &li, nullptr, &g->text_layout);
    li.bindingCount = 1;
    li.pBindings = &uniform;
    vkCreateDescriptorSetLayout(g->dev, &li, nullptr, &g->uniform_layout);
    const VkDescriptorSetLayout sets[4] = {g->empty_layout, g->empty_layout, g->text_layout, g->uniform_layout};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 4;
    pli.pSetLayouts = sets;
    if (vkCreatePipelineLayout(g->dev, &pli, nullptr, &g->layout) != VK_SUCCESS) return fail("no pipeline layout");

    // Each frame's descriptor sets.
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kMaxFrames},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxFrames},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kMaxFrames}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2 * kMaxFrames;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(g->dev, &dpi, nullptr, &g->frame_pool) != VK_SUCCESS) return fail("no descriptor pool");
    for (int i = 0; i < g->nframes; ++i) {
        Frame& f = g->frames[i];
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g->frame_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g->text_layout;
        vkAllocateDescriptorSets(g->dev, &ai, &f.text_set);
        ai.pSetLayouts = &g->uniform_layout;
        vkAllocateDescriptorSets(g->dev, &ai, &f.uniform_set);
        VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool = g->commands;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        vkAllocateCommandBuffers(g->dev, &ci, &f.cmd);
        f.done = fence(g);
        if (!f.text_set || !f.uniform_set || !f.cmd || !f.done) return fail("cannot set up the frames");
    }

    // The shaders.
    const uint64_t t0 = now_ns();
    VkShaderModule vmod = shader_module(g, shaders->vertex);
    VkShaderModule fmod[BLIT_PIPE_POPUP_FX + 2] = {};
    for (int i = 0; i <= BLIT_PIPE_POPUP_FX + 1; ++i) fmod[i] = shader_module(g, i <= BLIT_PIPE_POPUP_FX ? shaders->fragment[i] : shaders->image);
    const uint64_t t1 = now_ns();
    g->library_ns = t1 - t0;
    auto drop_modules = [&] {
        if (vmod) vkDestroyShaderModule(g->dev, vmod, nullptr);
        for (VkShaderModule m : fmod)
            if (m) vkDestroyShaderModule(g->dev, m, nullptr);
    };
    if (!vmod || std::find(std::begin(fmod), std::end(fmod), VkShaderModule{}) != std::end(fmod)) {
        drop_modules();
        return fail("a shader would not load");
    }

    // The pipelines, from the cache if they are in it.
    uint64_t key = 1469598103934665603ull;
    key = mix(key, shaders->vertex.code, shaders->vertex.size);
    for (int i = 0; i <= BLIT_PIPE_POPUP_FX; ++i) key = mix(key, shaders->fragment[i].code, shaders->fragment[i].size);
    key = mix(key, shaders->image.code, shaders->image.size);
    key = mix(key, props.deviceName, std::strlen(props.deviceName));
    key = mix(key, &props.driverVersion, sizeof props.driverVersion);
    key = mix(key, props.pipelineCacheUUID, sizeof props.pipelineCacheUUID);
    key = mix(key, &g->format, sizeof g->format);
    std::string cache_path, cache_data;
    if (cache_dir && *cache_dir) {
        mkdir(cache_dir, 0755);
        char file[64];
        std::snprintf(file, sizeof file, "/pipelines-%016llx.vkcache", static_cast<unsigned long long>(key));
        cache_path = std::string(cache_dir) + file;
        cache_data = read_file(cache_path);
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = cache_data.size();
    pci.pInitialData = cache_data.data();
    if (vkCreatePipelineCache(g->dev, &pci, nullptr, &g->cache) != VK_SUCCESS) {  // (a cache that will not load is made afresh)
        pci.initialDataSize = 0;
        vkCreatePipelineCache(g->dev, &pci, nullptr, &g->cache);
    }
    g->cached = !cache_data.empty() && feedback;
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineColorBlendAttachmentState blend[BLIT_PIPE_COUNT] = {};
    VkPipelineColorBlendStateCreateInfo cb[BLIT_PIPE_COUNT] = {};
    VkPipelineShaderStageCreateInfo stages[BLIT_PIPE_COUNT][2] = {};
    VkPipelineCreationFeedback whole[BLIT_PIPE_COUNT] = {}, parts[BLIT_PIPE_COUNT][2] = {};
    VkPipelineCreationFeedbackCreateInfo fb[BLIT_PIPE_COUNT] = {};
    VkGraphicsPipelineCreateInfo gpi[BLIT_PIPE_COUNT] = {};
    for (int p = 0; p < BLIT_PIPE_COUNT; ++p) {
        VkPipelineColorBlendAttachmentState& a = blend[p];
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (p == BLIT_PIPE_POPUP_BLEND || p == BLIT_PIPE_IMAGE) {  // the shader gives colour times alpha: added to what is there, which keeps 1 - alpha
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        cb[p] = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb[p].attachmentCount = 1;
        cb[p].pAttachments = &blend[p];
        const int frag = p == BLIT_PIPE_POPUP_BLEND ? BLIT_PIPE_POPUP_FX : p >= BLIT_PIPE_IMAGE ? BLIT_PIPE_POPUP_FX + 1 : p;
        stages[p][0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[p][0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[p][0].module = vmod;
        stages[p][0].pName = "main";
        stages[p][1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[p][1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[p][1].module = fmod[frag];
        stages[p][1].pName = "main";
        fb[p] = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
        fb[p].pPipelineCreationFeedback = &whole[p];
        fb[p].pipelineStageCreationFeedbackCount = 2;
        fb[p].pPipelineStageCreationFeedbacks = parts[p];
        gpi[p] = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi[p].pNext = feedback ? &fb[p] : nullptr;
        gpi[p].stageCount = 2;
        gpi[p].pStages = stages[p];
        gpi[p].pVertexInputState = &vin;
        gpi[p].pInputAssemblyState = &ia;
        gpi[p].pViewportState = &vp;
        gpi[p].pRasterizationState = &rs;
        gpi[p].pMultisampleState = &ms;
        gpi[p].pColorBlendState = &cb[p];
        gpi[p].pDynamicState = &ds;
        gpi[p].layout = g->layout;
        gpi[p].renderPass = g->pass_offscreen;
        gpi[p].subpass = 0;
    }
    const VkResult made = vkCreateGraphicsPipelines(g->dev, g->cache, BLIT_PIPE_COUNT, gpi, nullptr, g->pipes);
    drop_modules();
    if (made != VK_SUCCESS) return fail("a pipeline would not compile", made);
    g->pipelines_ns = now_ns() - t1;
    for (int p = 0; p < BLIT_PIPE_COUNT && g->cached; ++p)
        if (!(whole[p].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ||
            !(whole[p].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))
            g->cached = 0;
    // Pipelines that were not in the cache go into it, written on another
    // thread (blit_gpu_destroy() waits for it).
    if (!cache_path.empty() && !g->cached) {
        size_t size = 0;
        if (vkGetPipelineCacheData(g->dev, g->cache, &size, nullptr) == VK_SUCCESS && size) {
            std::string data(size, '\0');
            if (vkGetPipelineCacheData(g->dev, g->cache, &size, data.data()) == VK_SUCCESS) {
                data.resize(size);
                g->caching = std::thread([path = cache_path, data = std::move(data)] { write_file(path, data.data(), data.size()); });
            }
        }
    }
    g->uploads.reserve(16);
    g->dropped.reserve(16);
    g->free_sems.reserve(8);
    return g;
}

void blit_gpu_destroy(BlitGpu* g) {
    if (!g) return;
    if (g->dev) {
        blit_gpu_cancel(g);
        if (g->queue) {
            drop_held(g);
            idle(g);
        }
        vkDeviceWaitIdle(g->dev);
        for (auto& [id, p] : g->pictures) picture_free(g, p);
        for (Picture& p : g->dropped) picture_free(g, p);
        for (VkDescriptorPool pool : g->picture_pools) vkDestroyDescriptorPool(g->dev, pool, nullptr);
        for (Buffer& b : g->spare_stagings) buffer_free(g, b);
        for (Frame& f : g->frames) {
            buffer_free(g, f.cells);
            buffer_free(g, f.uniforms);
            buffer_free(g, f.staging);
            if (f.done) vkDestroyFence(g->dev, f.done, nullptr);
            if (f.acquired) vkDestroySemaphore(g->dev, f.acquired, nullptr);
        }
        swapchain_free_views(g);
        if (g->swapchain) vkDestroySwapchainKHR(g->dev, g->swapchain, nullptr);
        for (VkSemaphore s : g->drawn) vkDestroySemaphore(g->dev, s, nullptr);
        for (VkSemaphore s : g->free_sems) vkDestroySemaphore(g->dev, s, nullptr);
        if (g->off_fb) vkDestroyFramebuffer(g->dev, g->off_fb, nullptr);
        image_free(g, g->offscreen);
        image_free(g, g->atlas);
        image_free(g, g->colours);
        for (VkPipeline p : g->pipes)
            if (p) vkDestroyPipeline(g->dev, p, nullptr);
        if (g->cache) vkDestroyPipelineCache(g->dev, g->cache, nullptr);
        if (g->layout) vkDestroyPipelineLayout(g->dev, g->layout, nullptr);
        for (VkDescriptorSetLayout l : {g->empty_layout, g->text_layout, g->uniform_layout})
            if (l) vkDestroyDescriptorSetLayout(g->dev, l, nullptr);
        if (g->frame_pool) vkDestroyDescriptorPool(g->dev, g->frame_pool, nullptr);
        if (g->sampler) vkDestroySampler(g->dev, g->sampler, nullptr);
        if (g->smooth) vkDestroySampler(g->dev, g->smooth, nullptr);
        if (g->commands) vkDestroyCommandPool(g->dev, g->commands, nullptr);
        for (VkRenderPass rp : {g->pass_window, g->pass_shown, g->pass_offscreen})
            if (rp) vkDestroyRenderPass(g->dev, rp, nullptr);
        vkDestroyDevice(g->dev, nullptr);
    }
    if (g->caching.joinable()) g->caching.join();  // (the cache, written)
    if (g->surface) vkDestroySurfaceKHR(g->instance, g->surface, nullptr);
    if (g->messenger) {
        auto drop = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g->instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (drop) drop(g->instance, g->messenger, nullptr);
    }
    if (g->instance) vkDestroyInstance(g->instance, nullptr);
    delete g;
}

const char* blit_gpu_name(const BlitGpu* g) { return g ? g->name : ""; }

void blit_gpu_setup_times(const BlitGpu* g, uint64_t* library_ns, uint64_t* pipelines_ns, int* cached) {
    if (library_ns) *library_ns = g->library_ns;
    if (pipelines_ns) *pipelines_ns = g->pipelines_ns;
    if (cached) *cached = g->cached;
}

int blit_gpu_set_present(BlitGpu* g, int mode) {
    if (!g->surface) return 0;
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (mode == BLIT_PRESENT_IMMEDIATE) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (mode == BLIT_PRESENT_MAILBOX) want = VK_PRESENT_MODE_MAILBOX_KHR;
    if (std::find(g->modes.begin(), g->modes.end(), want) == g->modes.end()) return want == VK_PRESENT_MODE_FIFO_KHR;  // (FIFO is always there)
    if (want != g->present) {
        g->present = want;
        g->stale = true;
    }
    return 1;
}

int blit_gpu_set_images(BlitGpu* g, int n) {
    if (!g->surface || !g->present_wait || n < 2 || n > 3) return 0;
    g->max_images = n;
    return 1;
}

int blit_gpu_set_offscreen(BlitGpu* g, int w, int h) {
    blit_gpu_wait_idle(g);
    if (g->off_fb) vkDestroyFramebuffer(g->dev, g->off_fb, nullptr);
    g->off_fb = VK_NULL_HANDLE;
    image_free(g, g->offscreen);
    g->off_w = g->off_h = 0;
    if (w <= 0 || h <= 0) return 1;
    if (!image_make(g, g->offscreen, g->format, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return 0;
    g->off_fb = framebuffer(g, g->pass_offscreen, g->offscreen.view, w, h);
    if (!g->off_fb) return 0;
    // Black, and in the layout a frame leaves it in, so that it can be read
    // back before anything is drawn.
    VkCommandBuffer cmd = command_buffer(g);
    if (!cmd) return 0;
    barrier(cmd, g->offscreen.img, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, g->offscreen.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(cmd, g->offscreen.img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (!run_now(g, cmd)) return 0;
    g->off_w = w;
    g->off_h = h;
    return 1;
}

int blit_gpu_set_colours(BlitGpu* g, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    blit_gpu_wait_idle(g);
    image_free(g, g->colours);
    g->colours_w = g->colours_h = 0;
    for (Frame& f : g->frames) f.text_stale = true;
    if (!image_make(g, g->colours, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    VkCommandBuffer cmd = command_buffer(g);
    if (!cmd) return 0;
    barrier(cmd, g->colours.img, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue empty{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, g->colours.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &empty, 1, &all);
    barrier(cmd, g->colours.img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (!run_now(g, cmd)) return 0;
    g->colours_w = w;
    g->colours_h = h;
    return 1;
}

void blit_gpu_colour_rows(BlitGpu* g, const uint8_t* rgba, int y0, int y1) {
    if (!g->colours.img || !rgba) return;
    if (y0 < 0) y0 = 0;
    if (y1 > g->colours_h) y1 = g->colours_h;
    if (y1 <= y0) return;
    blit_gpu_wait_idle(g);  // (no frame under way reads it as it changes)
    const size_t row = static_cast<size_t>(g->colours_w) * 4, bytes = row * static_cast<size_t>(y1 - y0);
    Buffer src;
    if (!buffer_make(g, src, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
    std::memcpy(src.map, rgba + static_cast<size_t>(y0) * row, bytes);
    if (VkCommandBuffer cmd = command_buffer(g)) {
        barrier(cmd, g->colours.img, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {0, y0, 0};
        c.imageExtent = {static_cast<uint32_t>(g->colours_w), static_cast<uint32_t>(y1 - y0), 1};
        vkCmdCopyBufferToImage(cmd, src.buf, g->colours.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, g->colours.img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        run_now(g, cmd);
    }
    buffer_free(g, src);
}

int blit_gpu_set_atlas(BlitGpu* g, int w, int h) {
    blit_gpu_wait_idle(g);  // (frames under way read the old one)
    image_free(g, g->atlas);
    g->atlas_w = g->atlas_h = 0;
    g->atlas_pending = false;
    for (Frame& f : g->frames) f.text_stale = true;
    if (!image_make(g, g->atlas, VK_FORMAT_R8_UNORM, w, h, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    VkCommandBuffer cmd = command_buffer(g);
    if (!cmd) return 0;
    barrier(cmd, g->atlas.img, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue empty{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, g->atlas.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &empty, 1, &all);
    barrier(cmd, g->atlas.img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (!run_now(g, cmd)) return 0;
    g->atlas_w = w;
    g->atlas_h = h;
    return 1;
}

uint32_t blit_gpu_image_add(BlitGpu* g, const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;
    collect(g);
    // Its copies, each half the size of the one before, down to a pixel.
    uint32_t levels = 1;
    for (int side = w > h ? w : h; side > 1; side >>= 1) ++levels;
    Picture p;
    if (!image_make(g, p.im, VK_FORMAT_R8G8B8A8_UNORM, w, h, levels, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return 0;
    // Its descriptor set: the picture where the atlas is (and no cells, which
    // the picture shader does not read).
    for (int attempt = 0; attempt < 2 && !p.set; ++attempt) {
        if (attempt > 0 || g->picture_pools.empty()) {
            const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            dpi.maxSets = 64;
            dpi.poolSizeCount = 2;
            dpi.pPoolSizes = sizes;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(g->dev, &dpi, nullptr, &pool) != VK_SUCCESS) break;
            g->picture_pools.push_back(pool);
        }
        for (VkDescriptorPool pool : g->picture_pools) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &g->text_layout;
            if (vkAllocateDescriptorSets(g->dev, &ai, &p.set) == VK_SUCCESS) {
                p.pool = pool;
                break;
            }
            p.set = VK_NULL_HANDLE;
        }
    }
    Upload u;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4;
    if (!p.set || !take_staging(g, u.staging, bytes)) {
        picture_free(g, p);
        return 0;
    }
    VkDescriptorImageInfo ii{g->smooth, p.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = p.set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(g->dev, 1, &wr, 0, nullptr);
    std::memcpy(u.staging.map, rgba, static_cast<size_t>(bytes));

    // Copied, and its copies made, by the GPU ahead of the frames that draw
    // it (one queue: they come after this), and waited for. (Not waited for,
    // an upload under way while a frame was drawn lost the device on an AMD
    // card with RADV, Nov 2026: a GPUVM fault, the context lost, whether or
    // not that frame drew the picture. A plain file in a window 3600 pixels
    // wide did it, by the minimap's tiles, and so did a Markdown preview's
    // diagram at its top; waiting here, neither did. Waiting costs a
    // picture's copy, a few milliseconds for a large one, once.)
    u.cmd = command_buffer(g);
    u.done = fence(g);
    if (!u.cmd || !u.done) {
        if (u.cmd) vkFreeCommandBuffers(g->dev, g->commands, 1, &u.cmd);
        if (u.done) vkDestroyFence(g->dev, u.done, nullptr);
        buffer_free(g, u.staging);
        picture_free(g, p);
        return 0;
    }
    VkCommandBuffer cmd = u.cmd;
    barrier(cmd, p.im.img, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd, u.staging.buf, p.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    int lw = w, lh = h;
    for (uint32_t level = 1; level < levels; ++level) {
        barrier(cmd, p.im.img, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        b.srcOffsets[1] = {lw, lh, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        b.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, p.im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        lw = nw, lh = nh;
    }
    if (levels > 1)
        barrier(cmd, p.im.img, 0, levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(cmd, p.im.img, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(g->queue, 1, &si, u.done) != VK_SUCCESS) {
        vkFreeCommandBuffers(g->dev, g->commands, 1, &u.cmd);
        vkDestroyFence(g->dev, u.done, nullptr);
        buffer_free(g, u.staging);
        picture_free(g, p);
        return 0;
    }
    vkWaitForFences(g->dev, 1, &u.done, VK_TRUE, UINT64_MAX);
    p.upload = u.done;
    g->uploads.push_back(u);
    const uint32_t id = ++g->next_picture;
    g->pictures.emplace(id, p);
    return id;
}

void blit_gpu_image_drop(BlitGpu* g, uint32_t id) {
    const auto it = g->pictures.find(id);
    if (it == g->pictures.end()) return;
    g->dropped.push_back(it->second);  // (let go of once no frame under way draws it)
    g->pictures.erase(it);
    collect(g);
}

void blit_gpu_atlas_rows(BlitGpu* g, const uint8_t* pixels, int y0, int y1) {
    if (!g->atlas.img || y1 <= y0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > g->atlas_h) y1 = g->atlas_h;
    if (g->atlas_pending) {  // (more rows in the same frame: they go together)
        if (y0 > g->atlas_y0) y0 = g->atlas_y0;
        if (y1 < g->atlas_y1) y1 = g->atlas_y1;
    }
    const size_t row = static_cast<size_t>(g->atlas_w), bytes = row * static_cast<size_t>(y1 - y0);
    if (!g->begun) {
        // Outside a frame: now, and waited for.
        Buffer src;
        if (!buffer_make(g, src, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
        std::memcpy(src.map, pixels + static_cast<size_t>(y0) * row, bytes);
        VkCommandBuffer cmd = command_buffer(g);
        if (cmd) {
            g->atlas_y0 = y0;
            g->atlas_y1 = y1;
            record_atlas(g, cmd, src);
            run_now(g, cmd);
        }
        buffer_free(g, src);
        return;
    }
    Frame& f = g->frames[g->slot];
    if (!buffer_at_least(g, f.staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
    std::memcpy(f.staging.map, pixels + static_cast<size_t>(y0) * row, bytes);
    g->atlas_pending = true;
    g->atlas_y0 = y0;
    g->atlas_y1 = y1;
}

int blit_gpu_begin(BlitGpu* g, int want_w, int want_h, int* w, int* h, BlitGpuTimes* t) {
    if (g->begun) blit_gpu_cancel(g);
    const uint64_t t0 = now_ns();
    Frame& f = g->frames[g->slot];
    finish(g, f);
    collect(g);
    const uint64_t t1 = now_ns();
    uint64_t t2 = t1;
    if (g->off_fb) {
        g->target = g->off_fb;
        g->target_w = g->off_w;
        g->target_h = g->off_h;
        g->acquired = false;
    } else if (g->surface) {
        if (want_w <= 0 || want_h <= 0) {  // minimized
            if (t) t->free_ns = t1 - t0, t->target_ns = 0;
            return BLIT_FRAME_SKIPPED;
        }
        if (g->stale || (want_w != g->sc_w || want_h != g->sc_h)) {
            const int made = swapchain_make(g, want_w, want_h);
            if (made <= 0) {
                if (t) t->free_ns = t1 - t0, t->target_ns = now_ns() - t1;
                return made < 0 ? BLIT_FRAME_SKIPPED : BLIT_FRAME_FAILED;
            }
        }
        if (g->held) {
            g->held = false;
            g->index = g->held_index;
            f.acquired = g->held_sem;
            g->held_sem = VK_NULL_HANDLE;
        } else {
            // No more images under way than asked for: the frame one or two
            // before this one has reached the screen (a window left hidden
            // never shows it, hence the limit on the wait).
            const uint64_t ahead = static_cast<uint64_t>(g->max_images - 1);
            if (g->present_wait && g->present_id >= ahead)
                g->wait_for_present(g->dev, g->swapchain, g->present_id + 1 - ahead, 100'000'000);
            VkSemaphore sem = semaphore(g);
            VkResult r = vkAcquireNextImageKHR(g->dev, g->swapchain, 1'000'000'000, sem, VK_NULL_HANDLE, &g->index);
            if (r == VK_ERROR_OUT_OF_DATE_KHR && swapchain_make(g, want_w, want_h) > 0)
                r = vkAcquireNextImageKHR(g->dev, g->swapchain, 1'000'000'000, sem, VK_NULL_HANDLE, &g->index);
            if (r == VK_SUBOPTIMAL_KHR) g->stale = true;  // (drawn and shown, and made again for the next)
            else if (r != VK_SUCCESS) {
                g->free_sems.push_back(sem);  // (not signalled: nothing was acquired)
                t2 = now_ns();
                if (t) t->free_ns = t1 - t0, t->target_ns = t2 - t1;
                return r == VK_TIMEOUT || r == VK_NOT_READY || r == VK_ERROR_OUT_OF_DATE_KHR ? BLIT_FRAME_SKIPPED : BLIT_FRAME_FAILED;
            }
            f.acquired = sem;
        }
        t2 = now_ns();
        g->acquired = true;
        g->target = g->framebuffers[g->index];
        g->target_w = g->sc_w;
        g->target_h = g->sc_h;
    } else {
        return BLIT_FRAME_FAILED;
    }
    g->begun = true;
    *w = g->target_w;
    *h = g->target_h;
    if (t) t->free_ns = t1 - t0, t->target_ns = t2 - t1;
    return BLIT_FRAME_DRAWN;
}

uint32_t* blit_gpu_cells(BlitGpu* g, uint32_t count) {
    if (!g->begun) return nullptr;
    Frame& f = g->frames[g->slot];
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(count ? count : 1) * sizeof(uint32_t);
    if (!buffer_at_least(g, f.cells, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &f.text_stale)) return nullptr;  // (the grid reads what it has written)
    return static_cast<uint32_t*>(f.cells.map);
}

void blit_gpu_cancel(BlitGpu* g) {
    if (!g->begun) return;
    g->begun = false;
    Frame& f = g->frames[g->slot];
    if (g->acquired) {  // (the image is drawn into by the next frame)
        g->held = true;
        g->held_index = g->index;
        g->held_sem = f.acquired;
        f.acquired = VK_NULL_HANDLE;
        g->acquired = false;
    }
    g->target = VK_NULL_HANDLE;
    if (g->atlas_pending) {
        // Glyph rows staged for this frame go by themselves (the font has
        // forgotten they changed). Frames after this one come after them.
        vkResetCommandBuffer(f.cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);
        record_atlas(g, f.cmd, f.staging);
        vkEndCommandBuffer(f.cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        if (vkQueueSubmit(g->queue, 1, &si, f.done) == VK_SUCCESS) {
            f.pending = true;
            f.serial = ++g->serial;
            g->slot = (g->slot + 1) % g->nframes;  // (its staging buffer is in use until then)
        }
    }
}

int blit_gpu_end(BlitGpu* g, const void* uniforms, size_t stride, size_t blocks, const BlitGpuDraw* draws, uint32_t count, BlitGpuTimes* t) {
    if (!g->begun) return BLIT_FRAME_FAILED;
    const uint64_t t0 = now_ns();
    Frame& f = g->frames[g->slot];
    const uint64_t frame = g->serial + 1;

    // The uniform blocks, each where a dynamic offset can point.
    const size_t step = (stride + g->ubo_align - 1) / g->ubo_align * g->ubo_align;
    if (!buffer_at_least(g, f.uniforms, std::max<size_t>(blocks, 1) * step, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false, &f.uniform_stale)) {
        blit_gpu_cancel(g);
        return BLIT_FRAME_FAILED;
    }
    if (step == stride) {
        std::memcpy(f.uniforms.map, uniforms, blocks * stride);
    } else {
        for (size_t i = 0; i < blocks; ++i)
            std::memcpy(static_cast<uint8_t*>(f.uniforms.map) + i * step, static_cast<const uint8_t*>(uniforms) + i * stride, stride);
    }
    frame_sets(g, f, stride);

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    if (g->atlas_pending) record_atlas(g, cmd, f.staging);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = !g->acquired ? g->pass_offscreen : g->shown[g->index] ? g->pass_shown : g->pass_window;
    rp.framebuffer = g->target;
    rp.renderArea.extent = {static_cast<uint32_t>(g->target_w), static_cast<uint32_t>(g->target_h)};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, static_cast<float>(g->target_w), static_cast<float>(g->target_h), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, 2, 1, &f.text_set, 0, nullptr);
    const int tw = g->target_w, th = g->target_h;
    uint32_t bound = ~0u;
    size_t given = static_cast<size_t>(-1);  // the uniform block the last draw was given: the next with the same one is not given it again
    uint32_t shown_picture = 0;              // the picture in the atlas's place (0: the atlas is there)
    for (uint32_t i = 0; i < count; ++i) {
        const BlitGpuDraw* d = &draws[i];
        int x0 = d->x, y0 = d->y, x1 = d->x + d->w, y1 = d->y + d->h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > tw) x1 = tw;
        if (y1 > th) y1 = th;
        if (x1 <= x0 || y1 <= y0 || d->pipeline >= BLIT_PIPE_COUNT || d->uniforms >= blocks) continue;
        const uint32_t picture = d->pipeline == BLIT_PIPE_IMAGE || d->pipeline == BLIT_PIPE_IMAGE_SOLID ? d->image : 0;
        if (picture != shown_picture) {
            if (picture) {
                const auto it = g->pictures.find(picture);
                if (it == g->pictures.end()) continue;
                it->second.last_frame = frame;  // (kept until this frame is done)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, 2, 1, &it->second.set, 0, nullptr);
            } else {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, 2, 1, &f.text_set, 0, nullptr);
            }
            shown_picture = picture;
        }
        if (d->pipeline != bound) {
            bound = d->pipeline;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pipes[bound]);
        }
        if (d->uniforms != given) {
            given = d->uniforms;
            const uint32_t offset = static_cast<uint32_t>(given * step);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, 3, 1, &f.uniform_set, 1, &offset);
        }
        const VkRect2D scissor{{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    const uint64_t t1 = now_ns();

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g->acquired) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &g->drawn[g->index];
    }
    const VkResult r = vkQueueSubmit(g->queue, 1, &si, f.done);
    if (r != VK_SUCCESS) {
        g->begun = false;
        return BLIT_FRAME_FAILED;
    }
    f.pending = true;
    f.serial = ++g->serial;
    if (g->acquired) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        const uint64_t id = g->present_id + 1;
        VkPresentIdKHR ids{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        ids.swapchainCount = 1;
        ids.pPresentIds = &id;
        pi.pNext = g->present_wait ? &ids : nullptr;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g->drawn[g->index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g->swapchain;
        pi.pImageIndices = &g->index;
        const VkResult shown = vkQueuePresentKHR(g->queue, &pi);
        if (shown == VK_SUCCESS || shown == VK_SUBOPTIMAL_KHR) g->present_id = id, g->shown[g->index] = 1;
        if (shown != VK_SUCCESS) g->stale = true;  // (out of date, or no longer the best fit: made again before the next frame)
    }
    if (t) t->encode_ns = t1 - t0, t->submit_ns = now_ns() - t1;
    g->target = VK_NULL_HANDLE;
    g->begun = false;
    g->acquired = false;
    g->slot = (g->slot + 1) % g->nframes;
    return BLIT_FRAME_DRAWN;
}

void blit_gpu_wait_idle(BlitGpu* g) {
    if (g->queue) idle(g);
}

int blit_gpu_read_pixels(BlitGpu* g, uint8_t* rgba, int w, int h) {
    if (!g->off_fb || w != g->off_w || h != g->off_h) return 0;
    blit_gpu_wait_idle(g);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4;
    Buffer out;
    if (!buffer_make(g, out, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return 0;
    VkCommandBuffer cmd = command_buffer(g);
    bool ok = cmd != VK_NULL_HANDLE;
    if (ok) {
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        vkCmdCopyImageToBuffer(cmd, g->offscreen.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buf, 1, &c);
        ok = run_now(g, cmd);
    }
    if (ok) std::memcpy(rgba, out.map, static_cast<size_t>(bytes));
    buffer_free(g, out);
    if (!ok) return 0;
    if (g->format == VK_FORMAT_B8G8R8A8_UNORM)
        for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h) * 4; i += 4) std::swap(rgba[i], rgba[i + 2]);
    return 1;
}

}  // extern "C"
