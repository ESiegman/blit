#pragma once
// Colour emoji on a Mac (emoji_mac.m): Apple Color Emoji through CoreText,
// whose headers only Clang reads, behind a C function the font calls.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The colour font's picture of a character: w by h pixels of premultiplied
// RGBA, rows top first, into out (w * h * 4 bytes, zeroed by the caller),
// the glyph as large as fits with a little room, in the middle. False if
// the font has no glyph for it.
bool blit_emoji_rgba(uint32_t cp, int w, int h, uint8_t* out);

#ifdef __cplusplus
}
#endif
