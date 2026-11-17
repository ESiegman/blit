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
