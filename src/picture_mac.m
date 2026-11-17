// Image files read by the system's own ImageIO (see picture.h): every kind
// it knows, turned the right way up (a camera's orientation tag), made
// smaller while it is read when it is larger than wanted, and drawn into
// 8-bit RGBA in sRGB with its colours multiplied by alpha, as the GPU
// blends them. An animation's frames are read whole, as they are shown. SVG,
// which ImageIO does not read, is drawn by AppKit's NSImage (the system's
// own SVG), at whatever size it is wanted.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <PDFKit/PDFKit.h>

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

// A bitmap to draw into: w by h, RGBA in sRGB, colours multiplied by alpha.
static CGContextRef bitmap(uint8_t* rgba, size_t w, size_t h) {
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(rgba, w, h, 8, w * 4, srgb, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(srgb);
    return cg;
}

// Whether these bytes are an SVG (an <svg> element near the start).
static int is_svg(const void* data, size_t size) {
    const size_t head = size < 4096 ? size : 4096;
    return head >= 4 && memmem(data, head, "<svg", 4) != NULL;
}

// An SVG drawn w by h pixels (0, 0: its own size, the longer side at most
// max_side). AppKit draws it, into a context of this thread's own (an
// NSImage may be drawn on any thread that does not share it).
static int draw_svg(const void* data, size_t size, int w, int h, int max_side, BlitPicture* out, char* err, size_t err_size) {
    memset(out, 0, sizeof *out);
    @autoreleasepool {
        NSData* bytes = [NSData dataWithBytesNoCopy:(void*)data length:size freeWhenDone:NO];
        NSImage* img = [[NSImage alloc] initWithData:bytes];
        if (!img || !img.isValid) {
            say(err, err_size, "an SVG this system cannot draw");
            return 0;
        }
        // Its own size, in points; one that says none is a browser's 300 by 150.
        NSSize own = img.size;
        if (own.width < 1 || own.height < 1) own = NSMakeSize(300, 150);
        out->full_w = (int)lround(own.width) > 0 ? (int)lround(own.width) : 1;
        out->full_h = (int)lround(own.height) > 0 ? (int)lround(own.height) : 1;
        if (w <= 0 || h <= 0) {
            const double k = fmin(1.0, (double)max_side / fmax(own.width, own.height));
            w = (int)fmax(1, lround(own.width * k));
            h = (int)fmax(1, lround(own.height * k));
        }
        out->rgba = (uint8_t*)calloc((size_t)w * (size_t)h, 4);
        CGContextRef cg = out->rgba ? bitmap(out->rgba, (size_t)w, (size_t)h) : NULL;
        if (!cg) {
            blit_picture_free(out);
            say(err, err_size, "no memory for the picture");
            return 0;
        }
        NSGraphicsContext* ns = [NSGraphicsContext graphicsContextWithCGContext:cg flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ns];
        ns.imageInterpolation = NSImageInterpolationHigh;
        [img drawInRect:NSMakeRect(0, 0, w, h) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(cg);
        out->w = w;
        out->h = h;
        out->frames = 1;
        out->vector = 1;
        snprintf(out->kind, sizeof out->kind, "SVG");
        return 1;
    }
}

int blit_picture_read(const void* data, size_t size, int max_side, BlitPicture* out, char* err, size_t err_size) {
    memset(out, 0, sizeof *out);
    @autoreleasepool {
        CFDataRef bytes = CFDataCreateWithBytesNoCopy(NULL, (const UInt8*)data, (CFIndex)size, kCFAllocatorNull);
        CGImageSourceRef src = bytes ? CGImageSourceCreateWithData(bytes, NULL) : NULL;
        if (bytes) CFRelease(bytes);
        const size_t count = src ? CGImageSourceGetCount(src) : 0;
        if (count == 0 && is_svg(data, size)) {
            if (src) CFRelease(src);
            return draw_svg(data, size, 0, 0, max_side, out, err, err_size);
        }
        if (!src) {
            say(err, err_size, "not a picture this system can read");
            return 0;
        }
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
        CGContextRef cg = out->rgba ? bitmap(out->rgba, w, h) : NULL;
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

int blit_picture_draw(const void* data, size_t size, int w, int h, BlitPicture* out, char* err, size_t err_size) {
    if (!is_svg(data, size)) {
        memset(out, 0, sizeof *out);
        say(err, err_size, "not a picture drawn from shapes");
        return 0;
    }
    return draw_svg(data, size, w > 0 ? w : 1, h > 0 ? h : 1, 0, out, err, err_size);
}

void blit_picture_free(BlitPicture* p) {
    if (!p) return;
    free(p->rgba);
    p->rgba = NULL;
}

// ---------------------------------------------------------------- animations

// The dictionary of each kind of animation ImageIO reads, and its keys for
// how long a frame shows and how many times it plays.
typedef struct {
    CFStringRef dict, delay, unclamped, loops;
    int loops_unsaid;  // what no loop count means (a GIF without one plays once)
} AnimKeys;

static int anim_keys(NSDictionary* frame0, AnimKeys* k) {
    const AnimKeys all[] = {
        {kCGImagePropertyGIFDictionary, kCGImagePropertyGIFDelayTime, kCGImagePropertyGIFUnclampedDelayTime, kCGImagePropertyGIFLoopCount, 1},
        {kCGImagePropertyPNGDictionary, kCGImagePropertyAPNGDelayTime, kCGImagePropertyAPNGUnclampedDelayTime, kCGImagePropertyAPNGLoopCount, 0},
        {kCGImagePropertyWebPDictionary, kCGImagePropertyWebPDelayTime, kCGImagePropertyWebPUnclampedDelayTime, kCGImagePropertyWebPLoopCount, 0},
        {kCGImagePropertyHEICSDictionary, kCGImagePropertyHEICSDelayTime, kCGImagePropertyHEICSUnclampedDelayTime, kCGImagePropertyHEICSLoopCount, 0},
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
        NSDictionary* d = frame0[(__bridge NSString*)all[i].dict];
        if ([d isKindOfClass:[NSDictionary class]] && (d[(__bridge NSString*)all[i].delay] || d[(__bridge NSString*)all[i].unclamped])) {
            *k = all[i];
            return 1;
        }
    }
    return 0;
}

int blit_frames_read(const void* data, size_t size, int max_side, size_t max_bytes, BlitFrames* out) {
    memset(out, 0, sizeof *out);
    @autoreleasepool {
        CFDataRef bytes = CFDataCreateWithBytesNoCopy(NULL, (const UInt8*)data, (CFIndex)size, kCFAllocatorNull);
        CGImageSourceRef src = bytes ? CGImageSourceCreateWithData(bytes, NULL) : NULL;
        if (bytes) CFRelease(bytes);
        if (!src) return 0;
        const size_t count = CGImageSourceGetCount(src);
        NSDictionary* frame0 = count >= 2 ? (__bridge_transfer NSDictionary*)CGImageSourceCopyPropertiesAtIndex(src, 0, NULL) : nil;
        AnimKeys keys;
        if (!frame0 || !anim_keys(frame0, &keys)) {  // (pages of a TIFF, sizes of an icon: not frames)
            CFRelease(src);
            return 0;
        }
        NSDictionary* whole = (__bridge_transfer NSDictionary*)CGImageSourceCopyProperties(src, NULL);
        NSDictionary* wd = whole[(__bridge NSString*)keys.dict];
        NSNumber* loops = [wd isKindOfClass:[NSDictionary class]] ? wd[(__bridge NSString*)keys.loops] : nil;
        out->loops = loops ? loops.intValue : keys.loops_unsaid;
        // A size at which all of it fits in max_bytes, and each side in max_side.
        const double fw = [frame0[(NSString*)kCGImagePropertyPixelWidth] doubleValue], fh = [frame0[(NSString*)kCGImagePropertyPixelHeight] doubleValue];
        if (fw < 1 || fh < 1) {
            CFRelease(src);
            return 0;
        }
        double k = fmin(1.0, max_side / fmax(fw, fh));
        const double all = fw * fh * 4.0 * (double)count;
        if (all * k * k > (double)max_bytes) k = sqrt((double)max_bytes / all);
        const int w = (int)fmax(1, floor(fw * k)), h = (int)fmax(1, floor(fh * k));
        const size_t one = (size_t)w * (size_t)h * 4;
        out->rgba = (uint8_t*)calloc(count, one);
        out->ms = (int*)calloc(count, sizeof(int));
        if (!out->rgba || !out->ms) {
            blit_frames_free(out);
            CFRelease(src);
            return 0;
        }
        NSDictionary* opts = @{(NSString*)kCGImageSourceShouldCacheImmediately : @NO};
        for (size_t i = 0; i < count; ++i) {
            @autoreleasepool {
                // (ImageIO gives each frame whole, the ones before it drawn under it as the file says.)
                CGImageRef img = CGImageSourceCreateImageAtIndex(src, i, (__bridge CFDictionaryRef)opts);
                NSDictionary* p = (__bridge_transfer NSDictionary*)CGImageSourceCopyPropertiesAtIndex(src, i, NULL);
                NSDictionary* d = p[(__bridge NSString*)keys.dict];
                NSNumber* t = [d isKindOfClass:[NSDictionary class]] ? (d[(__bridge NSString*)keys.unclamped] ?: d[(__bridge NSString*)keys.delay]) : nil;
                // (As browsers do: a frame said to show for 10 ms or less shows for 100.)
                const int ms = (int)lround(t.doubleValue * 1000);
                out->ms[i] = ms <= 10 ? 100 : ms;
                if (!img) {  // (a frame that cannot be read: the one before it again)
                    if (i > 0) memcpy(out->rgba + i * one, out->rgba + (i - 1) * one, one);
                    continue;
                }
                CGContextRef cg = bitmap(out->rgba + i * one, (size_t)w, (size_t)h);
                if (cg) {
                    CGContextSetBlendMode(cg, kCGBlendModeCopy);
                    CGContextSetInterpolationQuality(cg, kCGInterpolationHigh);
                    CGContextDrawImage(cg, CGRectMake(0, 0, w, h), img);
                    CGContextRelease(cg);
                }
                CGImageRelease(img);
            }
        }
        CFRelease(src);
        out->w = w;
        out->h = h;
        out->frames = (int)count;
        return 1;
    }
}

void blit_frames_free(BlitFrames* f) {
    if (!f) return;
    free(f->rgba);
    free(f->ms);
    f->rgba = NULL;
    f->ms = NULL;
}

// ---------------------------------------------------------------- the web

const char* blit_picture_readers(void) { return "ImageIO, AppKit (SVG), PDFKit and CoreGraphics (PDF)"; }

int blit_fetch(const char* url, int timeout_ms, size_t max_bytes, char** data, size_t* size, char* err, size_t err_size) {
    *data = NULL;
    *size = 0;
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:@(url)];
        if (!u || !([u.scheme isEqualToString:@"http"] || [u.scheme isEqualToString:@"https"])) {
            say(err, err_size, "not a web address");
            return 0;
        }
        // Its own session, keeping nothing (no cookies, no cache: blit keeps what it fetches).
        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = timeout_ms / 1000.0;
        cfg.timeoutIntervalForResource = timeout_ms / 1000.0;
        cfg.HTTPCookieStorage = nil;
        cfg.URLCache = nil;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSData* got = nil;
        __block NSString* why = nil;
        NSURLSessionDataTask* task = [session dataTaskWithURL:u completionHandler:^(NSData* d, NSURLResponse* r, NSError* e) {
            if (e) {
                why = e.localizedDescription;
            } else if ([r isKindOfClass:[NSHTTPURLResponse class]] && ((NSHTTPURLResponse*)r).statusCode != 200) {
                why = [NSString stringWithFormat:@"the server answered %ld", (long)((NSHTTPURLResponse*)r).statusCode];
            } else {
                got = d;
            }
            dispatch_semaphore_signal(done);
        }];
        [task resume];
        const long late = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout_ms + 1000) * NSEC_PER_MSEC));
        [session invalidateAndCancel];
        if (late) {
            say(err, err_size, "the server took too long");
            return 0;
        }
        if (!got) {
            say(err, err_size, why.UTF8String ?: "it could not be fetched");
            return 0;
        }
        if (got.length > max_bytes) {
            say(err, err_size, "it is too large");
            return 0;
        }
        *data = (char*)malloc(got.length ? got.length : 1);
        if (!*data) return 0;
        memcpy(*data, got.bytes, got.length);
        *size = got.length;
        return 1;
    }
}

// ---------------------------------------------------------------- PDF

struct BlitPdf {
    CGPDFDocumentRef doc;
    CFDataRef bytes;  // (the document reads from these)
    CFTypeRef kit;    // PDFKit's PDFDocument, for the text, links and outline (made when first wanted)
    int locked;
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
    const int locked = CGPDFDocumentIsEncrypted(doc) && !CGPDFDocumentIsUnlocked(doc) && !CGPDFDocumentUnlockWithPassword(doc, "");
    if (!locked && CGPDFDocumentGetNumberOfPages(doc) == 0) {
        CGPDFDocumentRelease(doc);
        CFRelease(bytes);
        say(err, err_size, "it has no pages");
        return NULL;
    }
    BlitPdf* pdf = (BlitPdf*)calloc(1, sizeof *pdf);
    pdf->doc = doc;
    pdf->bytes = bytes;
    pdf->locked = locked;
    return pdf;
}

void blit_pdf_close(BlitPdf* pdf) {
    if (!pdf) return;
    @autoreleasepool {
        if (pdf->kit) CFRelease(pdf->kit);
    }
    CGPDFDocumentRelease(pdf->doc);
    CFRelease(pdf->bytes);
    free(pdf);
}

int blit_pdf_locked(const BlitPdf* pdf) { return pdf && pdf->locked; }

// The document as PDFKit has it (made the first time).
static PDFDocument* kit_of(BlitPdf* pdf) {
    if (!pdf->kit && !pdf->locked) {
        PDFDocument* kit = [[PDFDocument alloc] initWithData:(__bridge NSData*)pdf->bytes];
        if (kit) pdf->kit = CFBridgingRetain(kit);
    }
    return (__bridge PDFDocument*)pdf->kit;
}

int blit_pdf_unlock(BlitPdf* pdf, const char* password) {
    if (!pdf || !pdf->locked) return pdf != NULL;
    @autoreleasepool {
        // PDFKit opens it, and its CoreGraphics document (opened with it) is
        // the one drawn from then: the password is needed once, and kept by
        // neither.
        PDFDocument* kit = [[PDFDocument alloc] initWithData:(__bridge NSData*)pdf->bytes];
        NSString* pw = password ? [[NSString alloc] initWithUTF8String:password] : nil;
        if (!kit || !pw || ![kit unlockWithPassword:pw]) return 0;
        CGPDFDocumentRef doc = kit.documentRef;
        if (!doc || !CGPDFDocumentIsUnlocked(doc) || CGPDFDocumentGetNumberOfPages(doc) == 0) return 0;
        CGPDFDocumentRetain(doc);
        CGPDFDocumentRelease(pdf->doc);
        pdf->doc = doc;
        pdf->kit = CFBridgingRetain(kit);
        pdf->locked = 0;
        return 1;
    }
}

int blit_pdf_pages(const BlitPdf* pdf) { return pdf && !pdf->locked ? (int)CGPDFDocumentGetNumberOfPages(pdf->doc) : 0; }

// (Pages count from 0 here, from 1 in CoreGraphics.)
static CGPDFPageRef page_of(const BlitPdf* pdf, int page) { return pdf && !pdf->locked && page >= 0 ? CGPDFDocumentGetPage(pdf->doc, (size_t)page + 1) : NULL; }

static CGSize shown_size(CGPDFPageRef p) {
    const CGRect box = CGPDFPageGetBoxRect(p, kCGPDFCropBox);
    const int turn = ((CGPDFPageGetRotationAngle(p) % 360) + 360) % 360;
    return turn == 90 || turn == 270 ? CGSizeMake(box.size.height, box.size.width) : box.size;
}

// From the page's own space to the page as it is shown (turned), from its top left.
typedef struct {
    CGAffineTransform t;
    CGFloat h;
} Shown;

static Shown shown_of(CGPDFPageRef p) {
    const CGSize s = shown_size(p);
    Shown sh = {CGPDFPageGetDrawingTransform(p, kCGPDFCropBox, CGRectMake(0, 0, s.width, s.height), 0, true), s.height};
    return sh;
}

static void shown_box(const Shown* sh, CGRect r, float* out) {
    const CGRect b = CGRectApplyAffineTransform(r, sh->t);
    out[0] = (float)CGRectGetMinX(b);
    out[1] = (float)(sh->h - CGRectGetMaxY(b));
    out[2] = (float)CGRectGetMaxX(b);
    out[3] = (float)(sh->h - CGRectGetMinY(b));
}

static float shown_y(const Shown* sh, CGPoint p) { return (float)(sh->h - CGPointApplyAffineTransform(p, sh->t).y); }

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
        CGContextRef cg = out->rgba ? bitmap(out->rgba, (size_t)w, (size_t)h) : NULL;
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

int blit_pdf_text(BlitPdf* pdf, int page, int boxes, BlitPdfText* out) {
    memset(out, 0, sizeof *out);
    CGPDFPageRef cgp = page_of(pdf, page);
    if (!cgp) return 0;
    @autoreleasepool {
        PDFDocument* kit = kit_of(pdf);
        PDFPage* p = kit && page >= 0 && (NSUInteger)page < kit.pageCount ? [kit pageAtIndex:(NSUInteger)page] : nil;
        if (!p) return 0;
        NSString* s = p.string ?: @"";
        const NSUInteger n = s.length;
        unichar* units = (unichar*)malloc((n ? n : 1) * sizeof(unichar));
        out->at = (uint32_t*)malloc((n + 1) * sizeof(uint32_t));
        out->text = (char*)malloc(n * 3 + 1);
        if (!units || !out->at || !out->text) {
            free(units);
            blit_pdf_text_free(out);
            return 0;
        }
        [s getCharacters:units range:NSMakeRange(0, n)];
        // UTF-8, and where each unit starts in it (the second of a pair: after the character).
        size_t b = 0;
        for (NSUInteger i = 0; i < n; ++i) {
            out->at[i] = (uint32_t)b;
            uint32_t cp = units[i];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
                char* o = out->text + b;
                o[0] = (char)(0xF0 | (cp >> 18)), o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)), o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)), o[3] = (char)(0x80 | (cp & 0x3F));
                b += 4;
                out->at[++i] = (uint32_t)b;
                continue;
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
            char* o = out->text + b;
            if (cp < 0x80) o[0] = (char)cp, b += 1;
            else if (cp < 0x800) o[0] = (char)(0xC0 | (cp >> 6)), o[1] = (char)(0x80 | (cp & 0x3F)), b += 2;
            else o[0] = (char)(0xE0 | (cp >> 12)), o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)), o[2] = (char)(0x80 | (cp & 0x3F)), b += 3;
        }
        // (A pair whose second unit was skipped above has it set already; the end, after the last.)
        out->at[n] = (uint32_t)b;
        out->text[b] = 0;
        out->count = (int)n;
        free(units);
        if (!boxes || n == 0) return 1;
        // Where each character is, and each line (PDFKit's lines: a line's
        // box is as tall as its letters), from the top left of the page as shown.
        const Shown sh = shown_of(cgp);
        out->box = (float*)malloc(n * 4 * sizeof(float));
        out->line = (int*)malloc(n * sizeof(int));
        if (!out->box || !out->line) {
            blit_pdf_text_free(out);
            return 0;
        }
        // (Each character's box is its selection's: characterBoundsAtIndex
        // counts the characters without the line breaks the text has, and
        // drifts a character a line from the text.)
        for (NSUInteger i = 0; i < n; ++i) {
            @autoreleasepool {
                const NSRect r = [[p selectionForRange:NSMakeRange(i, 1)] boundsForPage:p];
                float* bx = out->box + i * 4;
                if (NSIsEmptyRect(r) && i > 0) bx[0] = bx[2] = bx[-2], bx[1] = bx[-3], bx[3] = bx[-1];
                else shown_box(&sh, r, bx);
                out->line[i] = -1;
            }
        }
        NSArray<PDFSelection*>* lines = [[p selectionForRange:NSMakeRange(0, n)] selectionsByLine];
        out->line_box = (float*)malloc((lines.count ? lines.count : 1) * 4 * sizeof(float));
        if (!out->line_box) {
            blit_pdf_text_free(out);
            return 0;
        }
        int count = 0;
        for (PDFSelection* l in lines) {
            const NSRange r = [l rangeAtIndex:0 onPage:p];
            if (r.location == NSNotFound || r.length == 0) continue;
            shown_box(&sh, [l boundsForPage:p], out->line_box + count * 4);
            for (NSUInteger i = r.location; i < r.location + r.length && i < n; ++i) out->line[i] = count;
            ++count;
        }
        out->lines = count;
        // What is on no line (the line ends between them) goes with the
        // character before it: no wider than nothing, at its end.
        for (NSUInteger i = 0; i < n; ++i) {
            if (out->line[i] >= 0) continue;
            const int l = i > 0 ? out->line[i - 1] : (count ? 0 : -1);
            out->line[i] = l < 0 ? 0 : l;
            float* bx = out->box + i * 4;
            const float x = i > 0 ? out->box[(i - 1) * 4 + 2] : (count ? out->line_box[0] : 0);
            bx[0] = bx[2] = x;
            bx[1] = count ? out->line_box[out->line[i] * 4 + 1] : 0;
            bx[3] = count ? out->line_box[out->line[i] * 4 + 3] : 0;
        }
        if (count == 0) {  // (text, and no lines PDFKit would give: one for all of it)
            out->lines = 1;
            float* lb = out->line_box;
            lb[0] = lb[1] = 1e9f, lb[2] = lb[3] = -1e9f;
            for (NSUInteger i = 0; i < n; ++i) {
                const float* bx = out->box + i * 4;
                lb[0] = fminf(lb[0], bx[0]), lb[1] = fminf(lb[1], bx[1]), lb[2] = fmaxf(lb[2], bx[2]), lb[3] = fmaxf(lb[3], bx[3]);
            }
        }
        return 1;
    }
}

void blit_pdf_text_free(BlitPdfText* t) {
    if (!t) return;
    free(t->text);
    free(t->at);
    free(t->box);
    free(t->line);
    free(t->line_box);
    memset(t, 0, sizeof *t);
}

// Where a destination goes: its page, and how far down it (its top, if it does not say).
static int place_of(BlitPdf* pdf, PDFDocument* kit, PDFDestination* d, int* page, float* y) {
    if (!d || !d.page) return 0;
    const NSUInteger at = [kit indexForPage:d.page];
    if (at == NSNotFound) return 0;
    *page = (int)at;
    *y = 0;
    CGPDFPageRef cgp = page_of(pdf, (int)at);
    const CGPoint pt = d.point;
    if (cgp && pt.y != kPDFDestinationUnspecifiedValue) {
        const Shown sh = shown_of(cgp);
        const CGFloat x = pt.x == kPDFDestinationUnspecifiedValue ? 0 : pt.x;
        *y = fmaxf(0, shown_y(&sh, CGPointMake(x, pt.y)));
    }
    return 1;
}

int blit_pdf_links(BlitPdf* pdf, int page, BlitPdfLink** out) {
    *out = NULL;
    CGPDFPageRef cgp = page_of(pdf, page);
    if (!cgp) return 0;
    @autoreleasepool {
        PDFDocument* kit = kit_of(pdf);
        PDFPage* p = kit && page >= 0 && (NSUInteger)page < kit.pageCount ? [kit pageAtIndex:(NSUInteger)page] : nil;
        NSArray<PDFAnnotation*>* all = p.annotations;
        if (!all.count) return 0;
        BlitPdfLink* links = (BlitPdfLink*)calloc(all.count, sizeof *links);
        if (!links) return 0;
        const Shown sh = shown_of(cgp);
        int n = 0;
        for (PDFAnnotation* a in all) {
            if (![a.type isEqualToString:@"Link"]) continue;
            BlitPdfLink* l = links + n;
            l->page = -1;
            NSURL* url = [a.action isKindOfClass:[PDFActionURL class]] ? ((PDFActionURL*)a.action).URL : a.URL;
            PDFDestination* dest = [a.action isKindOfClass:[PDFActionGoTo class]] ? ((PDFActionGoTo*)a.action).destination : a.destination;
            if (url.absoluteString.length) {
                l->url = strdup(url.absoluteString.UTF8String);
            } else if (!place_of(pdf, kit, dest, &l->page, &l->y)) {
                continue;
            }
            shown_box(&sh, a.bounds, l->box);
            ++n;
        }
        if (!n) {
            free(links);
            return 0;
        }
        *out = links;
        return n;
    }
}

void blit_pdf_links_free(BlitPdfLink* links, int count) {
    if (!links) return;
    for (int i = 0; i < count; ++i) free(links[i].url);
    free(links);
}

int blit_pdf_outline(BlitPdf* pdf, BlitPdfHeading** out) {
    *out = NULL;
    if (!pdf || pdf->locked) return 0;
    @autoreleasepool {
        PDFDocument* kit = kit_of(pdf);
        PDFOutline* root = kit.outlineRoot;
        if (!root || root.numberOfChildren == 0) return 0;
        // Depth first, in order (a stack of where each level is), at most a few thousand.
        enum { kMost = 4096, kDeepest = 32 };
        BlitPdfHeading* all = (BlitPdfHeading*)calloc(kMost, sizeof *all);
        if (!all) return 0;
        PDFOutline* at[kDeepest];
        NSUInteger next[kDeepest];
        int depth = 0, n = 0;
        at[0] = root;
        next[0] = 0;
        while (depth >= 0 && n < kMost) {
            if (next[depth] >= at[depth].numberOfChildren) {
                --depth;
                continue;
            }
            PDFOutline* o = [at[depth] childAtIndex:next[depth]++];
            if (!o) continue;
            BlitPdfHeading* h = all + n;
            h->page = -1;
            PDFDestination* dest = o.destination ?: ([o.action isKindOfClass:[PDFActionGoTo class]] ? ((PDFActionGoTo*)o.action).destination : nil);
            place_of(pdf, kit, dest, &h->page, &h->y);
            h->title = strdup(o.label.UTF8String ?: "");
            h->depth = depth;
            ++n;
            if (o.numberOfChildren > 0 && depth + 1 < kDeepest) {
                ++depth;
                at[depth] = o;
                next[depth] = 0;
            }
        }
        if (!n) {
            free(all);
            return 0;
        }
        *out = all;
        return n;
    }
}

void blit_pdf_outline_free(BlitPdfHeading* headings, int count) {
    if (!headings) return;
    for (int i = 0; i < count; ++i) free(headings[i].title);
    free(headings);
}
