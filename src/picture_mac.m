// Image files read by the system's own ImageIO (see picture.h): every kind
// it knows, turned the right way up (a camera's orientation tag), made
// smaller while it is read when it is larger than wanted, and drawn into
// 8-bit RGBA in sRGB with its colours multiplied by alpha, as the GPU
// blends them.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

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
