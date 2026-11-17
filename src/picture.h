/* Image files read into pixels: PNG, JPEG, GIF, BMP, TIFF, WebP, HEIC, SVG
 * and the rest the system can read. On Apple's systems, ImageIO's (and
 * AppKit's for SVG: picture_mac.m); elsewhere there is no reader yet. C,
 * like gpu.h, since the Apple side is Objective-C. */
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
    int frames;          /* more than 1: an animation (rgba is its first frame), or several pictures in one file */
    char kind[24];       /* what kind of file: "PNG", "JPEG" */
    int vector;          /* drawn from shapes (SVG): blit_picture_draw draws it again at any size */
} BlitPicture;

/* The picture in these bytes, its longer side at most max_side pixels. 0,
 * and why in err, if it cannot be read. A picture drawn from shapes is
 * drawn at its own size (in points, a pixel each). */
int blit_picture_read(const void* data, size_t size, int max_side, BlitPicture* out, char* err, size_t err_size);
/* A picture drawn from shapes (an SVG: `vector` from blit_picture_read)
 * drawn again, w by h pixels. */
int blit_picture_draw(const void* data, size_t size, int w, int h, BlitPicture* out, char* err, size_t err_size);
void blit_picture_free(BlitPicture* p);
/* What reads them here, in words ("" where nothing does): for --info. */
const char* blit_picture_readers(void);

/* An animation's frames (a GIF, an animated PNG, WebP or HEIC): each the
 * whole picture as it is shown then, one after another. */
typedef struct BlitFrames {
    uint8_t* rgba;  /* frames * w * h pixels of 4 bytes, as BlitPicture's (malloc'd) */
    int* ms;        /* how long each frame shows, in milliseconds (malloc'd) */
    int w, h, frames;
    int loops;      /* how many times it plays through (0: for ever) */
} BlitFrames;
/* The frames of the animation in these bytes, at a size at which they take
 * at most max_bytes together and no side is longer than max_side. 0 if it
 * is not an animation, or cannot be read. */
int blit_frames_read(const void* data, size_t size, int max_side, size_t max_bytes, BlitFrames* out);
void blit_frames_free(BlitFrames* f);

/* The bytes at a web address (http or https), fetched by the system
 * (NSURLSession), at most max_bytes of them, in at most timeout_ms. 0, and
 * why in err, if they cannot be had. *data is malloc'd. */
int blit_fetch(const char* url, int timeout_ms, size_t max_bytes, char** data, size_t* size, char* err, size_t err_size);

/* PDF documents (the system's CoreGraphics on Apple's systems, and PDFKit
 * for their text, links and outline). Not to be used from two threads at
 * once. */
typedef struct BlitPdf BlitPdf;
/* The document in these bytes (copied). Null, and why in err, if it
 * cannot be read. One locked with a password opens locked
 * (blit_pdf_locked): it has no pages until blit_pdf_unlock opens it. */
BlitPdf* blit_pdf_open(const void* data, size_t size, char* err, size_t err_size);
void blit_pdf_close(BlitPdf* pdf);
int blit_pdf_locked(const BlitPdf* pdf);
/* The password tried on a locked document: 1 if it opened it. Nothing
 * keeps the password. */
int blit_pdf_unlock(BlitPdf* pdf, const char* password);
int blit_pdf_pages(const BlitPdf* pdf);
/* A page's size in points, as it is shown (turned, if the page says so). */
void blit_pdf_page_size(const BlitPdf* pdf, int page, double* w, double* h);
/* Part of a page drawn on white, `scale` pixels to a point: the pixels from
 * (x, y) (from its top left) w by h. 0 if it cannot be. */
int blit_pdf_render(const BlitPdf* pdf, int page, double scale, int x, int y, int w, int h, BlitPicture* out);

/* A page's text, as PDFKit reads it. Boxes are x0 y0 x1 y1, in points from
 * the top left of the page as it is shown. */
typedef struct BlitPdfText {
    char* text;        /* UTF-8, ended by a 0 */
    int count;         /* its characters (UTF-16 units, as PDFKit counts them) */
    uint32_t* at;      /* count + 1 of them: where each character starts in text (in bytes) */
    float* box;        /* count * 4: where each character is (null unless boxes were asked for) */
    int* line;         /* count: which line each is on (likewise) */
    int lines;
    float* line_box;   /* lines * 4: where each line is (likewise) */
} BlitPdfText;
/* A page's text, with where its characters and lines are if `boxes`. 0 if
 * it cannot be read. */
int blit_pdf_text(BlitPdf* pdf, int page, int boxes, BlitPdfText* out);
void blit_pdf_text_free(BlitPdfText* t);

/* A link on a page: to a web address, or to a place in the document. */
typedef struct BlitPdfLink {
    float box[4];  /* where it is on its page, as the text's boxes */
    char* url;     /* a web address (malloc'd), or null: */
    int page;      /* a page of the document (from 0; -1: none), */
    float y;       /* and how far down it, in points */
} BlitPdfLink;
int blit_pdf_links(BlitPdf* pdf, int page, BlitPdfLink** out);
void blit_pdf_links_free(BlitPdfLink* links, int count);

/* The document's outline (its table of contents), in order, each entry
 * with how deep it is (0: at the top) and where it goes. */
typedef struct BlitPdfHeading {
    char* title;  /* malloc'd */
    int depth, page;
    float y;
} BlitPdfHeading;
int blit_pdf_outline(BlitPdf* pdf, BlitPdfHeading** out);
void blit_pdf_outline_free(BlitPdfHeading* headings, int count);

#ifdef __cplusplus
}
#endif

#endif
