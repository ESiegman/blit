// Image files read by the system's own ImageIO (see picture.h): every kind
// it knows, turned the right way up (a camera's orientation tag), made
// smaller while it is read when it is larger than wanted, and drawn into
// 8-bit RGBA in sRGB with its colours multiplied by alpha, as the GPU
// blends them.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picture.h"

static void say(char* err, size_t size, const char* what) {
    if (err && size) snprintf(err, size, "%s", what);
}

// "public.png" -> "PNG", "com.compuserve.gif" -> "GIF".
static void kind_of(CGImageSourceRef src, char* out, size_t size) {
    out[0] = 0;
    CFStringRef type = CGImageSourceGetType(src);
    if (!type) return;
    NSString* t = (__bridge NSString*)type;
    NSString* last = [t pathExtension].length ? [t pathExtension] : t;
    if ([last isEqualToString:@"jpeg-2000"]) last = @"jp2";
    snprintf(out, size, "%s", last.uppercaseString.UTF8String ?: "");
}

int blit_picture_read(const void* data, size_t size, int max_side, BlitPicture* out, char* err, size_t err_size) {
    memset(out, 0, sizeof *out);
    @autoreleasepool {
        CFDataRef bytes = CFDataCreateWithBytesNoCopy(NULL, (const UInt8*)data, (CFIndex)size, kCFAllocatorNull);
        CGImageSourceRef src = bytes ? CGImageSourceCreateWithData(bytes, NULL) : NULL;
        if (bytes) CFRelease(bytes);
        if (!src) {
            say(err, err_size, "not a picture this system can read");
            return 0;
        }
        const size_t count = CGImageSourceGetCount(src);
        if (count == 0 || CGImageSourceGetStatus(src) != kCGImageStatusComplete) {
            CFRelease(src);
            say(err, err_size, count ? "the file is cut short" : "not a picture this system can read");
            return 0;
        }
        kind_of(src, out->kind, sizeof out->kind);
        out->frames = (int)count;
        // Its own size, the right way up (orientations 5 to 8 turn it on its side).
        CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
        if (props) {
            NSDictionary* p = (__bridge NSDictionary*)props;
            out->full_w = [p[(NSString*)kCGImagePropertyPixelWidth] intValue];
            out->full_h = [p[(NSString*)kCGImagePropertyPixelHeight] intValue];
            const int orient = [p[(NSString*)kCGImagePropertyOrientation] intValue];
            if (orient >= 5 && orient <= 8) {
                const int t = out->full_w;
                out->full_w = out->full_h;
                out->full_h = t;
            }
            CFRelease(props);
        }
        // Read at the size wanted (never larger than it is), turned upright.
        int longer = out->full_w > out->full_h ? out->full_w : out->full_h;
        if (longer <= 0) longer = max_side;
        NSDictionary* opts = @{
            (NSString*)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
            (NSString*)kCGImageSourceCreateThumbnailWithTransform : @YES,
            (NSString*)kCGImageSourceShouldCacheImmediately : @YES,
            (NSString*)kCGImageSourceThumbnailMaxPixelSize : @(longer < max_side ? longer : max_side),
        };
        CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)opts);
        CFRelease(src);
        if (!img) {
            say(err, err_size, "the picture could not be read");
            return 0;
        }
        const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
        if (!out->full_w || !out->full_h) out->full_w = (int)w, out->full_h = (int)h;
        out->rgba = (uint8_t*)calloc(w * h, 4);
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef cg = out->rgba ? CGBitmapContextCreate(out->rgba, w, h, 8, w * 4, srgb, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
        CGColorSpaceRelease(srgb);
        if (!cg) {
            CGImageRelease(img);
            blit_picture_free(out);
            say(err, err_size, "no memory for the picture");
            return 0;
        }
        CGContextSetBlendMode(cg, kCGBlendModeCopy);
        CGContextDrawImage(cg, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
        CGContextRelease(cg);
        CGImageRelease(img);
        out->w = (int)w;
        out->h = (int)h;
        return 1;
    }
}

void blit_picture_free(BlitPicture* p) {
    if (!p) return;
    free(p->rgba);
    p->rgba = NULL;
}

// ---------------------------------------------------------------- PDF

struct BlitPdf {
    CGPDFDocumentRef doc;
    CFDataRef bytes;  // (the document reads from these)
};

BlitPdf* blit_pdf_open(const void* data, size_t size, char* err, size_t err_size) {
    // (Its header first: CoreGraphics complains on the standard error about anything else.)
    const size_t head = size < 1024 ? size : 1024;
    if (head < 5 || !memmem(data, head, "%PDF-", 5)) {
        say(err, err_size, "not a PDF this system can read");
        return NULL;
    }
    CFDataRef bytes = CFDataCreate(NULL, (const UInt8*)data, (CFIndex)size);
    CGDataProviderRef src = bytes ? CGDataProviderCreateWithCFData(bytes) : NULL;
    CGPDFDocumentRef doc = src ? CGPDFDocumentCreateWithProvider(src) : NULL;
    if (src) CGDataProviderRelease(src);
    if (!doc) {
        if (bytes) CFRelease(bytes);
        say(err, err_size, "not a PDF this system can read");
        return NULL;
    }
    if (CGPDFDocumentIsEncrypted(doc) && !CGPDFDocumentUnlockWithPassword(doc, "")) {
        CGPDFDocumentRelease(doc);
        CFRelease(bytes);
        say(err, err_size, "it is locked with a password");
        return NULL;
    }
    if (CGPDFDocumentGetNumberOfPages(doc) == 0) {
        CGPDFDocumentRelease(doc);
        CFRelease(bytes);
        say(err, err_size, "it has no pages");
        return NULL;
    }
    BlitPdf* pdf = (BlitPdf*)calloc(1, sizeof *pdf);
    pdf->doc = doc;
    pdf->bytes = bytes;
    return pdf;
}

void blit_pdf_close(BlitPdf* pdf) {
    if (!pdf) return;
    CGPDFDocumentRelease(pdf->doc);
    CFRelease(pdf->bytes);
    free(pdf);
}

int blit_pdf_pages(const BlitPdf* pdf) { return pdf ? (int)CGPDFDocumentGetNumberOfPages(pdf->doc) : 0; }

// (Pages count from 0 here, from 1 in CoreGraphics.)
static CGPDFPageRef page_of(const BlitPdf* pdf, int page) { return pdf && page >= 0 ? CGPDFDocumentGetPage(pdf->doc, (size_t)page + 1) : NULL; }

static CGSize shown_size(CGPDFPageRef p) {
    const CGRect box = CGPDFPageGetBoxRect(p, kCGPDFCropBox);
    const int turn = ((CGPDFPageGetRotationAngle(p) % 360) + 360) % 360;
    return turn == 90 || turn == 270 ? CGSizeMake(box.size.height, box.size.width) : box.size;
}

void blit_pdf_page_size(const BlitPdf* pdf, int page, double* w, double* h) {
    CGPDFPageRef p = page_of(pdf, page);
    const CGSize s = p ? shown_size(p) : CGSizeMake(612, 792);
    *w = s.width;
    *h = s.height;
}

int blit_pdf_render(const BlitPdf* pdf, int page, double scale, int x, int y, int w, int h, BlitPicture* out) {
    memset(out, 0, sizeof *out);
    CGPDFPageRef p = page_of(pdf, page);
    if (!p || w <= 0 || h <= 0 || scale <= 0) return 0;
    @autoreleasepool {
        out->rgba = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef cg = out->rgba ? CGBitmapContextCreate(out->rgba, (size_t)w, (size_t)h, 8, (size_t)w * 4, srgb, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
        CGColorSpaceRelease(srgb);
        if (!cg) {
            blit_picture_free(out);
            return 0;
        }
        CGContextSetRGBFillColor(cg, 1, 1, 1, 1);
        CGContextFillRect(cg, CGRectMake(0, 0, w, h));
        // CoreGraphics counts from the bottom: the part wanted, moved to the
        // bitmap, then the page scaled, then turned into its box (a drawing
        // transform to a rectangle of the page's own size turns it and does
        // not scale it).
        const CGSize s = shown_size(p);
        const double full_h = s.height * scale;
        CGContextTranslateCTM(cg, -x, -(full_h - (y + h)));
        CGContextScaleCTM(cg, scale, scale);
        CGContextConcatCTM(cg, CGPDFPageGetDrawingTransform(p, kCGPDFCropBox, CGRectMake(0, 0, s.width, s.height), 0, true));
        CGContextClipToRect(cg, CGPDFPageGetBoxRect(p, kCGPDFCropBox));
        CGContextSetInterpolationQuality(cg, kCGInterpolationHigh);
        CGContextSetRenderingIntent(cg, kCGRenderingIntentDefault);
        CGContextDrawPDFPage(cg, p);
        CGContextRelease(cg);
        out->w = w;
        out->h = h;
        out->full_w = (int)lround(s.width * scale);
        out->full_h = (int)lround(s.height * scale);
        out->frames = 1;
        snprintf(out->kind, sizeof out->kind, "PDF");
        return 1;
    }
}
