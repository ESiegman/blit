#include "emoji_mac.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

bool blit_emoji_rgba(uint32_t cp, int w, int h, uint8_t* out) {
    static const CGFloat kSize = 64;
    static CTFontRef base = NULL;  // (looked for once; kept for the run)
    static bool tried = false;
    if (!tried) {
        tried = true;
        base = CTFontCreateWithName(CFSTR("AppleColorEmoji"), kSize, NULL);
    }
    if (!base || w <= 0 || h <= 0) return false;
    UniChar units[2];
    CFIndex n = 1;
    if (cp >= 0x10000) {
        units[0] = (UniChar)(0xD800 + ((cp - 0x10000) >> 10));
        units[1] = (UniChar)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        n = 2;
    } else {
        units[0] = (UniChar)cp;
    }
    CGGlyph glyphs[2] = {0, 0};
    if (!CTFontGetGlyphsForCharacters(base, units, glyphs, n) || !glyphs[0]) return false;
    const CGRect box = CTFontGetBoundingRectsForGlyphs(base, kCTFontOrientationHorizontal, glyphs, NULL, 1);
    if (box.size.width <= 0 || box.size.height <= 0) return false;
    const CGFloat kx = w * 0.94 / box.size.width, ky = h * 0.94 / box.size.height, k = kx < ky ? kx : ky;
    CTFontRef sized = CTFontCreateCopyWithAttributes(base, kSize * k, NULL, NULL);
    if (!sized) return false;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out, (size_t)w, (size_t)h, 8, (size_t)w * 4, space,
                                             (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (ctx) {
        const CGPoint at = {(w - box.size.width * k) / 2 - box.origin.x * k, (h - box.size.height * k) / 2 - box.origin.y * k};
        CTFontDrawGlyphs(sized, glyphs, &at, 1, ctx);
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(space);
    CFRelease(sized);
    return ctx != NULL;
}

bool blit_emoji_seq_rgba(const uint32_t* cps, size_t n, int w, int h, uint8_t* out) {
    static const CGFloat kSize = 64;
    static CTFontRef base = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        base = CTFontCreateWithName(CFSTR("AppleColorEmoji"), kSize, NULL);
    }
    if (!base || !n || n > 16 || w <= 0 || h <= 0) return false;
    UniChar units[32];
    CFIndex len = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t cp = cps[i];
        if (cp >= 0x10000) {
            units[len++] = (UniChar)(0xD800 + ((cp - 0x10000) >> 10));
            units[len++] = (UniChar)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            units[len++] = (UniChar)cp;
        }
    }
    // Shaped as a line in the colour font: one glyph, or it is not one picture.
    CFStringRef str = CFStringCreateWithCharacters(NULL, units, len);
    if (!str) return false;
    const void* keys[] = {kCTFontAttributeName};
    const void* vals[] = {base};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = as ? CTLineCreateWithAttributedString(as) : NULL;
    bool ok = false;
    CGGlyph glyph = 0;
    CTFontRef run_font = NULL;
    if (line && CTLineGetGlyphCount(line) == 1) {
        CFArrayRef runs = CTLineGetGlyphRuns(line);
        if (CFArrayGetCount(runs) == 1) {
            CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, 0);
            CTRunGetGlyphs(run, CFRangeMake(0, 1), &glyph);
            run_font = (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
            ok = glyph != 0 && run_font != NULL;
        }
    }
    if (ok) {
        const CGRect box = CTFontGetBoundingRectsForGlyphs(run_font, kCTFontOrientationHorizontal, &glyph, NULL, 1);
        ok = box.size.width > 0 && box.size.height > 0;
        if (ok) {
            const CGFloat fs = CTFontGetSize(run_font);
            const CGFloat kx = w * 0.94 / box.size.width, ky = h * 0.94 / box.size.height, k = kx < ky ? kx : ky;
            CTFontRef sized = CTFontCreateCopyWithAttributes(run_font, fs * k, NULL, NULL);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = sized ? CGBitmapContextCreate(out, (size_t)w, (size_t)h, 8, (size_t)w * 4, space, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
            if (ctx) {
                const CGPoint at = {(w - box.size.width * k) / 2 - box.origin.x * k, (h - box.size.height * k) / 2 - box.origin.y * k};
                CTFontDrawGlyphs(sized, &glyph, &at, 1, ctx);
                CGContextRelease(ctx);
            }
            ok = ctx != NULL;
            CGColorSpaceRelease(space);
            if (sized) CFRelease(sized);
        }
    }
    if (line) CFRelease(line);
    if (as) CFRelease(as);
    CFRelease(attrs);
    CFRelease(str);
    return ok;
}
