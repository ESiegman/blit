#pragma once
// The renderer: what a frame shows, as cells and a few draws. Each draw is
// a triangle that covers the window, cut to a rectangle, whose fragment
// shader reads the cells and the glyph atlas. The GPU behind it is Metal on
// Apple's systems and Vulkan elsewhere (gpu.h); SDL has the window.
#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

#include "buffer.hpp"
#include "font.hpp"
#include "gpu.h"
#include "grid.hpp"

namespace blit {

struct Theme {
    float bg[4], fg[4], sel[4], cursor[4];
    float thumb[4];  // scroll thumb colour; the fourth value is its opacity
    float popup_bg[4], popup_border[4];
    float tint[4];  // washed over where something being dragged would land: rgb, and how strongly in a
    float hover[4];   // the background of a popup's cells under the mouse
    float shadow = 0.4f;  // how dark a popup's shadow is at its darkest (0..1)
    // Ink colours a cell can pick; [0] is plain text, the last eight have
    // fixed meanings (see grid.hpp).
    float palette[32][4];
};

// A box drawn over the text: a grid of cells of its own, with a margin and
// an outline. Completion lists and hover text are these, and so are the
// tab bars, the line numbers and the side panel. Lengths are in pixels.
struct Popup {
    int x = 0, y = 0, w = 0, h = 0;  // the box, in pixels
    int pad_x = 0, pad_y = 0;        // from the box's edge to its first cell
    u32 cols = 0, rows = 0;
    const u32* cells = nullptr;      // cols * rows of them
    float border_px = 1;             // width of the outline
    // Its background and the background of its "selected" cells, when they
    // are not the theme's for popups (rgba; null = the theme's).
    const float* bg = nullptr;
    const float* sel = nullptr;
    // Other backgrounds for selected cells: a selected cell whose bits 21-23
    // (kCellDecoShift) hold 1 to 7 is that colour of these eight (rgba each;
    // the first is not used). A profile's flame graph is drawn this way.
    const float* tones = nullptr;
    // Its own ink colours, in place of the theme's (32, rgba): a terminal's.
    const float* palette = nullptr;
    // Its looks. A box with none of these is drawn as it always was.
    float radius = 0;                     // of its corners
    float shadow = 0;                     // how far its shadow reaches; 0: it casts none
    float shadow_dx = 0, shadow_dy = 0;   // which way the shadow falls
    float shadow_alpha = 1;               // of the theme's darkness for shadows
    float opacity = 1;                    // of all of it: below 1 while it fades in
    // Runs of selected cells (and of cells under the mouse) as rounded
    // boxes: the radius (0: the cells themselves, square), and how far
    // inside the cells' own edges the box is (negative: it reaches out into
    // the margin).
    // A run that reaches the grid's first or last column goes on
    // into the margin by pill_out.
    float pill = 0, pill_dx = 0, pill_dy = 0, pill_out = 0;
    // What it draws outside its box (its shadow) stays inside this
    // rectangle, x0 y0 x1 y1, if it has any area.
    int clip[4] = {0, 0, 0, 0};
};

// One text view: the visible part of a document, in a rectangle of the
// window that is its alone. A window split in three has three.
struct TextView {
    const Buffer* buf = nullptr;
    GridSpec grid;
    float origin[2] = {0, 0};            // grid-space position of the rectangle's top-left pixel
    int x = 0, y = 0, w = 0, h = 0;      // the rectangle, in pixels
    u32 blank_rows = 0;                  // grid rows left empty above the text: the view is pulled down past its top
    float cursor[4] = {-1, -1, -1, -1};  // x0 y0 x1 y1 in window pixels
    float thumb[4] = {-1, -1, -1, -1};
    float tint[4] = {-1, -1, -1, -1};    // a rectangle washed with the theme's tint
    bool cursor_block = false;           // the cursor is a block over a character, which shows through it
    float thumb_alpha = 1;               // of the theme's opacity for the scroll thumb: it fades away when left alone
    // Corner radii, in pixels: of what is selected (0 while nothing is, so
    // that no pixel looks for corners), of the cursor, of a search match, of
    // the tinted rectangle.
    float sel_radius = 0, cursor_radius = 0, match_radius = 0, tint_radius = 0;
    float dim = 0;                       // how far the view is dimmed toward the background (0..1)
    // Kept by the view's owner from frame to frame; null: the renderer's own.
    AnchorCache* anchors = nullptr;
    GuideCache* guides = nullptr;
};

inline constexpr u32 kMaxViews = 8;
inline constexpr u32 kMaxPopups = 48;
inline constexpr u32 kMaxPictures = 256;  // (a PDF's pages, and a box over each line of what is chosen or found on them)

// A picture drawn in a frame (one Renderer::image_add() was given).
struct PictureDraw {
    u32 image = 0;
    float rect[4] = {};  // where all of it is: x0 y0 x1 y1, px (it may go past the clip)
    int clip[4] = {};    // what of the window it may draw in: x0 y0 x1 y1, px
    u32 after = 0;       // drawn after this many of the frame's popups (0: over the text, under every popup)
    float radius = 0;    // its corners, px
    float opacity = 1;
    bool sharp = false;  // each of its pixels a square (drawn much larger than it is), not smoothed
    bool clear = false;  // some of its pixels are see-through
    float checker = 0;   // squares of this size (px) behind its clear pixels; 0: none
    float check_a[4] = {}, check_b[4] = {};
};

// Everything the renderer needs for one frame besides the text itself.
struct FrameDesc {
    TextView views[kMaxViews];
    u32 view_count = 0;
    float guide_px = 1;                // width of an indent line
    float scale = 1;                   // pixels to a point (2 on a display of double density)
    // The wavy underline: period, amplitude, thickness, and how far its
    // middle sits above the bottom of the cell, all in pixels.
    float squiggle[4] = {8, 2, 1.5f, 3.5f};
    Popup popups[kMaxPopups];
    u32 popup_count = 0;
    PictureDraw pictures[kMaxPictures];
    u32 picture_count = 0;
    Theme theme{};
};

// Called once the target size is known; fills in the frame description.
using LayoutFn = std::function_ref<void(int w, int h, FrameDesc& fd)>;

struct FrameStats {
    u64 fill_ns = 0;   // text -> cells
    u64 layout_ns = 0; // deciding what goes where, popups included
    u64 cpu_ns = 0;    // whole draw() call minus the wait for a swapchain image
    u64 wait_ns = 0;   // blocked waiting for a swapchain image
    // Where cpu_ns went, besides fill_ns:
    u64 begin_ns = 0;   // waiting for a frame's memory to be free (the GPU is behind)
    u64 upload_ns = 0;  // new glyphs in the atlas, staged (the cells need no copying)
    u64 pass_ns = 0;    // uniforms and draws, made into commands
    u64 submit_ns = 0;  // handing the frame to the GPU
    bool uploaded = false;
};

class Renderer {
public:
    enum Result { kDrawn, kSkipped, kFailed };
    enum PresentMode { kVsync, kImmediate, kMailbox };

    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    // win may be null for offscreen-only use (benchmarks, screenshots).
    // frames_in_flight: how many frames may be under way at once (0: 2).
    bool init(SDL_Window* win, int frames_in_flight, bool debug, std::string* err);
    bool set_font(const Font& font, std::string* err);
    bool set_present_mode(int mode);   // false if the GPU has no such mode
    // How many images of the window may be under way (2 or 3; see gpu.h).
    void set_images(int n);
    bool set_offscreen(int w, int h);  // 0,0 switches back to the window

    Result draw(Font& font, LayoutFn layout, FrameStats* stats = nullptr);
    // What a frame waits for (its memory, an image of the window to draw
    // into) waited for ahead of draw(), so that input that comes in during
    // the wait can still be taken in and be in that frame. draw() then uses
    // it. kDrawn: there is a frame to draw into.
    Result acquire();
    bool acquired() const { return acquired_; }
    void release();  // a frame acquired and not to be drawn after all

    // A picture (RGBA, colours multiplied by alpha) for PictureDraw: its id,
    // or 0 if it could not be made. Dropped when it is not wanted any more.
    u32 image_add(const u8* rgba, int w, int h);
    void image_drop(u32 id);

    // Blocks until the GPU has finished everything submitted so far.
    void wait_idle();
    // Reads back the offscreen target as RGBA bytes.
    bool read_pixels(std::vector<u8>& rgba, int* w, int* h);

    const char* driver() const;
    // What making the GPU device took: compiling or loading the shaders,
    // making the pipelines, and whether those came from the cache.
    void setup_times(u64* library_ns, u64* pipelines_ns, bool* cached) const;

private:
    SDL_Window* win_ = nullptr;
    void* view_ = nullptr;  // the window's Metal view (Apple), made by SDL
    BlitGpu* gpu_ = nullptr;
    int off_w_ = 0, off_h_ = 0;
    AnchorCache anchors_;
    GuideCache guides_;
    // A frame's uniform blocks and draws, kept from frame to frame for their memory.
    std::vector<u8> uniforms_;
    std::vector<BlitGpuDraw> draws_;
    std::vector<u8> lit_rows_;     // per popup row: has it a lit cell (see draw())
    bool acquired_ = false;
    int images_ = 0;
    int acq_w_ = 0, acq_h_ = 0;
    BlitGpuTimes acq_times_{};
};

}  // namespace blit
