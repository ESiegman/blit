/* Image files read into pixels: PNG, JPEG, GIF, BMP, TIFF, WebP, HEIC and
 * the rest the system can read. On Apple's systems, ImageIO's (picture_mac.m);
 * elsewhere there is no reader yet. C, like gpu.h, since the Apple side is
 * Objective-C. */
#ifndef BLIT_PICTURE_H
#define BLIT_PICTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BlitPicture {
    uint8_t* rgba;       /* w * h pixels of 4 bytes, colours multiplied by alpha (malloc'd) */
    int w, h;            /* of rgba: the picture, made smaller if it was larger than asked for */
    int full_w, full_h;  /* the picture's own size, the right way up */
    int frames;          /* more than 1: an animation (rgba is its first frame) */
    char kind[24];       /* what kind of file: "PNG", "JPEG" */
} BlitPicture;

/* The picture in these bytes, its longer side at most max_side pixels. 0,
 * and why in err, if it cannot be read. */
int blit_picture_read(const void* data, size_t size, int max_side, BlitPicture* out, char* err, size_t err_size);
void blit_picture_free(BlitPicture* p);

/* PDF documents (the system's CoreGraphics on Apple's systems). Not to be
 * used from two threads at once. */
typedef struct BlitPdf BlitPdf;
/* The document in these bytes (copied). Null, and why in err, if it
 * cannot be read (or is locked with a password). */
BlitPdf* blit_pdf_open(const void* data, size_t size, char* err, size_t err_size);
void blit_pdf_close(BlitPdf* pdf);
int blit_pdf_pages(const BlitPdf* pdf);
/* A page's size in points, as it is shown (turned, if the page says so). */
void blit_pdf_page_size(const BlitPdf* pdf, int page, double* w, double* h);
/* Part of a page drawn on white, `scale` pixels to a point: the pixels from
 * (x, y) (from its top left) w by h. 0 if it cannot be. */
int blit_pdf_render(const BlitPdf* pdf, int page, double scale, int x, int y, int w, int h, BlitPicture* out);

#ifdef __cplusplus
}
#endif

#endif
