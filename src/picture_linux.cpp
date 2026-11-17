// Image files, SVG and PDF pages on Linux (see picture.h), as picture_mac.m
// has them from the system's ImageIO, AppKit and PDFKit.
//
// PNG (animated PNG too), JPEG, GIF (and its animations) and WebP (and its
// animations) are read by their own libraries, linked in: they are small,
// and cost nothing at startup. SVG is drawn by librsvg and PDF pages by
// poppler, both through cairo, loaded the first time one is wanted (as
// HarfBuzz is, shape.cpp): they bring GLib and much more with them, which
// a run that shows neither should not pay for. What comes out is 8-bit RGBA
// with its colours multiplied by alpha, as the GPU blends them, made
// smaller as it is read when it is larger than wanted. A web address is
// fetched by curl.
#include "picture.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <gif_lib.h>
#include <png.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <turbojpeg.h>
#include <unistd.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if BLIT_HAVE_POPPLER
#include <cairo.h>
#include <poppler.h>
#endif
#if BLIT_HAVE_RSVG
#include <cairo.h>
#include <librsvg/rsvg.h>
#endif

extern char** environ;

namespace {

void say(char* err, size_t size, const char* what) {
    if (err && size) std::snprintf(err, size, "%s", what);
}

// What is read whole at most: a picture of more pixels (a small file can
// say it is enormous) is not read, rather than take gigabytes.
constexpr size_t kMostPixels = size_t{1} << 28;

bool too_many(long long w, long long h) { return w <= 0 || h <= 0 || static_cast<unsigned long long>(w) * static_cast<unsigned long long>(h) > kMostPixels; }

uint32_t be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

// A picture being made: w by h pixels of RGBA (multiplied by alpha once
// premultiply() has been run over it), of a picture full_w by full_h (0:
// the same; a JPEG may be read smaller than it is).
struct Pixels {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    int full_w = 0, full_h = 0;
};

void premultiply(uint8_t* p, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i, p += 4) {
        const unsigned a = p[3];
        if (a == 255) continue;
        p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
        p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
        p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
    }
}

// The pixels made smaller (or copied, at the same size), each the mean of
// the pixels it stands for.
void shrink(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    if (sw == dw && sh == dh) {
        std::memcpy(dst, src, static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4);
        return;
    }
    for (int y = 0; y < dh; ++y) {
        const int y0 = static_cast<int>(static_cast<long long>(y) * sh / dh);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * sh / dh));
        for (int x = 0; x < dw; ++x) {
            const int x0 = static_cast<int>(static_cast<long long>(x) * sw / dw);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(x + 1) * sw / dw));
            unsigned long long sum[4] = {};
            for (int yy = y0; yy < y1 && yy < sh; ++yy) {
                const uint8_t* row = src + (static_cast<size_t>(yy) * static_cast<size_t>(sw) + static_cast<size_t>(x0)) * 4;
                for (int xx = x0; xx < x1 && xx < sw; ++xx, row += 4)
                    for (int c = 0; c < 4; ++c) sum[c] += row[c];
            }
            const unsigned long long n = static_cast<unsigned long long>(std::min(y1, sh) - y0) * static_cast<unsigned long long>(std::min(x1, sw) - x0);
            uint8_t* o = dst + (static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)) * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
}

// The size a picture is read at: its own, or smaller so that its longer
// side is at most max_side.
void fit(int w, int h, int max_side, int* ow, int* oh) {
    const double k = std::min(1.0, static_cast<double>(max_side) / std::max(w, h));
    *ow = std::max(1, static_cast<int>(std::lround(w * k)));
    *oh = std::max(1, static_cast<int>(std::lround(h * k)));
}

// A finished picture handed out: premultiplied pixels made smaller if need be.
int hand_out(Pixels& p, int max_side, const char* kind, int frames, BlitPicture* out, char* err, size_t err_size) {
    premultiply(p.px.data(), static_cast<size_t>(p.w) * static_cast<size_t>(p.h));
    int w, h;
    fit(p.w, p.h, max_side, &w, &h);
    out->rgba = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    if (!out->rgba) {
        say(err, err_size, "no memory for the picture");
        return 0;
    }
    shrink(p.px.data(), p.w, p.h, out->rgba, w, h);
    out->w = w;
    out->h = h;
    out->full_w = p.full_w ? p.full_w : p.w;
    out->full_h = p.full_h ? p.full_h : p.h;
    out->frames = frames;
    std::snprintf(out->kind, sizeof out->kind, "%s", kind);
    return 1;
}

// ---------------------------------------------------------------- PNG

bool decode_png(const void* data, size_t size, Pixels* p) {
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, data, size)) return false;
    if (too_many(img.width, img.height)) {
        png_image_free(&img);
        return false;
    }
    img.format = PNG_FORMAT_RGBA;
    p->px.resize(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, nullptr, p->px.data(), 0, nullptr)) {
        png_image_free(&img);
        return false;
    }
    p->w = static_cast<int>(img.width);
    p->h = static_cast<int>(img.height);
    return true;
}

// An animated PNG's frames: libpng reads none of its own, so each is made
// into a PNG of its own (the file's header with the frame's size, the
// chunks every frame shares, the frame's data as IDAT) and read as one,
// then laid on a canvas as the frame says (APNG's dispose and blend).
struct Apng {
    std::vector<uint8_t> ihdr;               // its data
    std::vector<std::vector<uint8_t>> keep;  // whole chunks every frame needs (PLTE, tRNS, gAMA ...)
    struct Frame {
        uint32_t w = 0, h = 0, x = 0, y = 0;
        int ms = 100;
        uint8_t dispose = 0, blend = 0;
        std::vector<uint8_t> data;  // its compressed image
    };
    std::vector<Frame> frames;
    uint32_t loops = 0;
};

bool parse_apng(const uint8_t* b, size_t size, Apng* a) {
    if (size < 8 || std::memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return false;
    bool animated = false, seen_idat = false;
    Apng::Frame* cur = nullptr;
    bool cur_from_idat = false;
    for (size_t at = 8; at + 12 <= size;) {
        const uint32_t len = be32(b + at);
        if (len > size - at - 12) return false;
        const uint8_t* type = b + at + 4;
        const uint8_t* d = b + at + 8;
        if (!std::memcmp(type, "IHDR", 4) && len == 13) {
            a->ihdr.assign(d, d + len);
        } else if (!std::memcmp(type, "acTL", 4) && len == 8) {
            animated = true;
            a->loops = be32(d + 4);
        } else if (!std::memcmp(type, "fcTL", 4) && len == 26) {
            Apng::Frame f;
            f.w = be32(d + 4), f.h = be32(d + 8), f.x = be32(d + 12), f.y = be32(d + 16);
            const unsigned num = be16(d + 20), den = be16(d + 22) ? be16(d + 22) : 100;
            f.ms = static_cast<int>(std::lround(1000.0 * num / den));
            f.dispose = d[24], f.blend = d[25];
            a->frames.push_back(std::move(f));
            cur = &a->frames.back();
            cur_from_idat = !seen_idat;
        } else if (!std::memcmp(type, "IDAT", 4)) {
            seen_idat = true;
            if (cur && cur_from_idat) cur->data.insert(cur->data.end(), d, d + len);  // (no fcTL before it: the default image, no frame)
        } else if (!std::memcmp(type, "fdAT", 4) && len >= 4) {
            if (cur) cur->data.insert(cur->data.end(), d + 4, d + len);
        } else if (!seen_idat && (!std::memcmp(type, "PLTE", 4) || !std::memcmp(type, "tRNS", 4) || !std::memcmp(type, "gAMA", 4) ||
                                  !std::memcmp(type, "cHRM", 4) || !std::memcmp(type, "sRGB", 4) || !std::memcmp(type, "iCCP", 4))) {
            a->keep.emplace_back(b + at, b + at + 12 + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + len;
    }
    return animated && a->ihdr.size() == 13 && !a->frames.empty();
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* d, size_t len) {
    const uint8_t l[4] = {static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
    out.insert(out.end(), l, l + 4);
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), d, d + len);
    const uLong crc = crc32(0, out.data() + start, static_cast<uInt>(4 + len));
    const uint8_t c[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16), static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
    out.insert(out.end(), c, c + 4);
}

bool decode_apng_frame(const Apng& a, const Apng::Frame& f, Pixels* p) {
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t ihdr[13];
    std::memcpy(ihdr, a.ihdr.data(), 13);
    for (int i = 0; i < 4; ++i) ihdr[i] = static_cast<uint8_t>(f.w >> (24 - 8 * i)), ihdr[4 + i] = static_cast<uint8_t>(f.h >> (24 - 8 * i));
    put_chunk(png, "IHDR", ihdr, 13);
    for (const std::vector<uint8_t>& k : a.keep) png.insert(png.end(), k.begin(), k.end());
    put_chunk(png, "IDAT", f.data.data(), f.data.size());
    put_chunk(png, "IEND", nullptr, 0);
    return decode_png(png.data(), png.size(), p);
}

// The frames laid on the canvas one after another; each, whole as it is
// shown, handed to `show` (straight colours, not yet multiplied by alpha).
template <class Show>
bool play_apng(const Apng& a, int w, int h, Show&& show) {
    std::vector<uint8_t> canvas(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0), before;
    for (size_t i = 0; i < a.frames.size(); ++i) {
        const Apng::Frame& f = a.frames[i];
        Pixels p;
        const bool ok = !f.data.empty() && f.x + f.w <= static_cast<uint32_t>(w) && f.y + f.h <= static_cast<uint32_t>(h) && decode_apng_frame(a, f, &p);
        if (f.dispose == 2) before = canvas;
        if (ok) {
            for (uint32_t y = 0; y < f.h; ++y) {
                const uint8_t* s = p.px.data() + static_cast<size_t>(y) * f.w * 4;
                uint8_t* d = canvas.data() + ((static_cast<size_t>(f.y) + y) * static_cast<size_t>(w) + f.x) * 4;
                for (uint32_t x = 0; x < f.w; ++x, s += 4, d += 4) {
                    if (f.blend == 0 || s[3] == 255) {
                        std::memcpy(d, s, 4);
                    } else if (s[3]) {  // (over what is there, in straight colours)
                        const float sa = s[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
                        for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::lround((s[c] * sa + d[c] * da * (1 - sa)) / oa));
                        d[3] = static_cast<uint8_t>(std::lround(oa * 255));
                    }
                }
            }
        }
        show(i, canvas.data(), f.ms);
        if (f.dispose == 1) {
            for (uint32_t y = 0; y < f.h && f.y + y < static_cast<uint32_t>(h); ++y)
                std::memset(canvas.data() + ((static_cast<size_t>(f.y) + y) * static_cast<size_t>(w) + f.x) * 4, 0, static_cast<size_t>(std::min<uint32_t>(f.w, static_cast<uint32_t>(w) - f.x)) * 4);
        } else if (f.dispose == 2 && !before.empty()) {
            canvas.swap(before);
        }
    }
    return true;
}

// ---------------------------------------------------------------- JPEG

// The orientation a camera wrote in a JPEG's Exif (1: upright).
int jpeg_orientation(const uint8_t* b, size_t size) {
    for (size_t at = 2; at + 4 <= size && b[at] == 0xFF;) {
        const uint8_t marker = b[at + 1];
        const size_t len = be16(b + at + 2);
        if (marker == 0xDA || len < 2 || at + 2 + len > size) break;  // (the image data: no Exif after it)
        const uint8_t* d = b + at + 4;
        if (marker == 0xE1 && len >= 16 && !std::memcmp(d, "Exif\0\0", 6)) {
            const uint8_t* t = d + 6;
            const size_t tn = len - 8;
            const bool le = t[0] == 'I';
            auto u16 = [&](size_t o) { return o + 2 <= tn ? (le ? static_cast<unsigned>(t[o] | t[o + 1] << 8) : static_cast<unsigned>(t[o] << 8 | t[o + 1])) : 0u; };
            auto u32 = [&](size_t o) { return o + 4 <= tn ? (le ? static_cast<size_t>(u16(o) | static_cast<size_t>(u16(o + 2)) << 16) : static_cast<size_t>(u16(o)) << 16 | u16(o + 2)) : 0; };
            const size_t ifd = u32(4);
            const unsigned n = u16(ifd);
            for (unsigned i = 0; i < n; ++i) {
                const size_t e = ifd + 2 + 12 * static_cast<size_t>(i);
                if (u16(e) == 0x0112) return static_cast<int>(u16(e + 8));
            }
            return 1;
        }
        at += 2 + len;
    }
    return 1;
}

// The picture turned the right way up, as the camera said it was held.
void orient(Pixels* p, int o) {
    if (o < 2 || o > 8) return;
    const bool swap = o >= 5;
    const int w = p->w, h = p->h, nw = swap ? h : w, nh = swap ? w : h;
    std::vector<uint8_t> out(p->px.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int nx = x, ny = y;
            switch (o) {
                case 2: nx = w - 1 - x; break;
                case 3: nx = w - 1 - x, ny = h - 1 - y; break;
                case 4: ny = h - 1 - y; break;
                case 5: nx = y, ny = x; break;
                case 6: nx = h - 1 - y, ny = x; break;
                case 7: nx = h - 1 - y, ny = w - 1 - x; break;
                case 8: nx = y, ny = w - 1 - x; break;
                default: break;
            }
            std::memcpy(out.data() + (static_cast<size_t>(ny) * static_cast<size_t>(nw) + static_cast<size_t>(nx)) * 4,
                        p->px.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4, 4);
        }
    p->px.swap(out);
    p->w = nw;
    p->h = nh;
}

// A JPEG, decoded at the smallest of the sizes its decoder can make
// cheaply (an eighth, a quarter, ...) that is still at least max_side on
// its longer side, where it is larger than that.
bool decode_jpeg(const void* data, size_t size, int max_side, Pixels* p) {
    // (TurboJPEG's first interface: the one libjpeg-turbo 2, which most
    // systems in use still have, knows, and which 3 keeps.)
    const auto* jpeg = static_cast<const unsigned char*>(data);
    const auto jpeg_size = static_cast<unsigned long>(size);
    tjhandle tj = tjInitDecompress();
    if (!tj) return false;
    int fw = 0, fh = 0, subsampling = 0, colours = 0;
    bool ok = tjDecompressHeader3(tj, jpeg, jpeg_size, &fw, &fh, &subsampling, &colours) == 0 && fw > 0 && fh > 0;
    if (ok) {
        int n = 0;
        tjscalingfactor* factors = tjGetScalingFactors(&n);
        tjscalingfactor best{1, 1};
        for (int i = 0; factors && i < n; ++i) {
            const tjscalingfactor& f = factors[i];
            if (f.num >= f.denom) continue;  // (never larger than it is)
            const int longer = std::max(TJSCALED(fw, f), TJSCALED(fh, f));
            if (longer >= max_side && static_cast<long long>(f.num) * best.denom < static_cast<long long>(best.num) * f.denom) best = f;
        }
        p->w = TJSCALED(fw, best);
        p->h = TJSCALED(fh, best);
        ok = !too_many(p->w, p->h);
    }
    if (ok) {
        // (Asked for exactly the scaled size, the decoder takes that factor.)
        p->px.resize(static_cast<size_t>(p->w) * static_cast<size_t>(p->h) * 4);
        ok = tjDecompress2(tj, jpeg, jpeg_size, p->px.data(), p->w, 0, p->h, TJPF_RGBA, 0) == 0;
    }
    tjDestroy(tj);
    if (!ok) return false;
    const int o = jpeg_orientation(static_cast<const uint8_t*>(data), size);
    orient(p, o);
    p->full_w = o >= 5 && o <= 8 ? fh : fw;
    p->full_h = o >= 5 && o <= 8 ? fw : fh;
    return true;
}

// ---------------------------------------------------------------- GIF

struct GifSource {
    const uint8_t* data;
    size_t size, at;
};

int gif_read(GifFileType* g, GifByteType* buf, int n) {
    GifSource* s = static_cast<GifSource*>(g->UserData);
    const size_t k = std::min(static_cast<size_t>(n), s->size - s->at);
    std::memcpy(buf, s->data + s->at, k);
    s->at += k;
    return static_cast<int>(k);
}

GifFileType* open_gif(GifSource* src) {
    int e = 0;
    GifFileType* g = DGifOpen(src, gif_read, &e);
    if (!g) return nullptr;
    if (DGifSlurp(g) != GIF_OK && g->ImageCount == 0) {
        DGifCloseFile(g, &e);
        return nullptr;
    }
    return g;
}

// How many times it plays: NETSCAPE2.0's count (0: for ever), else once.
int gif_loops(const GifFileType* g) {
    if (g->ImageCount < 1) return 1;
    const SavedImage& s = g->SavedImages[0];
    for (int i = 0; i + 1 < s.ExtensionBlockCount; ++i) {
        const ExtensionBlock& e = s.ExtensionBlocks[i];
        if (e.Function == APPLICATION_EXT_FUNC_CODE && e.ByteCount >= 11 && !std::memcmp(e.Bytes, "NETSCAPE2.0", 11)) {
            const ExtensionBlock& d = s.ExtensionBlocks[i + 1];
            if (d.ByteCount >= 3 && d.Bytes[0] == 1) return d.Bytes[1] | d.Bytes[2] << 8;
        }
    }
    return 1;
}

// The frames laid on the canvas one after another, as GIF's disposal says,
// each handed whole to `show` (straight colours).
template <class Show>
void play_gif(GifFileType* g, Show&& show) {
    const int w = g->SWidth, h = g->SHeight;
    std::vector<uint8_t> canvas(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0), before;
    for (int i = 0; i < g->ImageCount; ++i) {
        const SavedImage& s = g->SavedImages[i];
        GraphicsControlBlock gcb{};
        gcb.TransparentColor = NO_TRANSPARENT_COLOR;
        DGifSavedExtensionToGCB(g, i, &gcb);
        const ColorMapObject* map = s.ImageDesc.ColorMap ? s.ImageDesc.ColorMap : g->SColorMap;
        const int fx = s.ImageDesc.Left, fy = s.ImageDesc.Top, fw = s.ImageDesc.Width, fh = s.ImageDesc.Height;
        if (gcb.DisposalMode == DISPOSE_PREVIOUS) before = canvas;
        if (map && s.RasterBits) {
            for (int y = 0; y < fh; ++y) {
                if (fy + y < 0 || fy + y >= h) continue;
                for (int x = 0; x < fw; ++x) {
                    if (fx + x < 0 || fx + x >= w) continue;
                    const int c = s.RasterBits[static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)];
                    if (c == gcb.TransparentColor || c >= map->ColorCount) continue;
                    uint8_t* d = canvas.data() + (static_cast<size_t>(fy + y) * static_cast<size_t>(w) + static_cast<size_t>(fx + x)) * 4;
                    d[0] = map->Colors[c].Red, d[1] = map->Colors[c].Green, d[2] = map->Colors[c].Blue, d[3] = 255;
                }
            }
        }
        // (As browsers do: a frame said to show for 10 ms or less shows for 100.)
        const int ms = gcb.DelayTime * 10;
        show(i, canvas.data(), ms <= 10 ? 100 : ms);
        if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
            for (int y = std::max(0, fy); y < std::min(h, fy + fh); ++y)
                for (int x = std::max(0, fx); x < std::min(w, fx + fw); ++x) std::memset(canvas.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4, 0, 4);
        } else if (gcb.DisposalMode == DISPOSE_PREVIOUS && !before.empty()) {
            canvas.swap(before);
        }
    }
}

// ---------------------------------------------------------------- what kind

enum Kind { kNone, kPng, kJpeg, kGif, kWebp, kSvg };

// Whether these bytes are an SVG (an <svg> element near the start).
bool is_svg(const void* data, size_t size) {
    const size_t head = std::min<size_t>(size, 4096);
    return head >= 4 && memmem(data, head, "<svg", 4) != nullptr;
}

Kind kind_of(const void* data, size_t size) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    if (size >= 8 && !std::memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return kPng;
    if (size >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return kJpeg;
    if (size >= 6 && (!std::memcmp(b, "GIF87a", 6) || !std::memcmp(b, "GIF89a", 6))) return kGif;
    if (size >= 12 && !std::memcmp(b, "RIFF", 4) && !std::memcmp(b + 8, "WEBP", 4)) return kWebp;
    if (is_svg(data, size)) return kSvg;
    return kNone;
}

// ---------------------------------------------------------------- GLib, cairo, librsvg, poppler: loaded when first wanted

#if BLIT_HAVE_POPPLER || BLIT_HAVE_RSVG
void* open_first(std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (void* lib = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return lib;
    return nullptr;
}

#define BLIT_GLIB_FUNCS(X) \
    X(g_bytes_new)         \
    X(g_bytes_unref)       \
    X(g_object_unref)      \
    X(g_error_free)
#define BLIT_CAIRO_FUNCS(X)              \
    X(cairo_image_surface_create_for_data) \
    X(cairo_create)                      \
    X(cairo_destroy)                     \
    X(cairo_surface_destroy)             \
    X(cairo_surface_flush)               \
    X(cairo_set_source_rgb)              \
    X(cairo_paint)                       \
    X(cairo_translate)                   \
    X(cairo_scale)

struct Base {
#define X(name) decltype(&::name) name = nullptr;
    BLIT_GLIB_FUNCS(X)
    BLIT_CAIRO_FUNCS(X)
#undef X
    void (*gfree)(void*) = nullptr;  // g_free (a macro of GLib's own, so not by its name)
    bool ok = false;
};

const Base& base() {
    static Base b;
    static std::once_flag once;
    std::call_once(once, [] {
        void* glib = open_first({"libglib-2.0.so.0"});
        void* gobject = open_first({"libgobject-2.0.so.0"});
        void* cairo = open_first({"libcairo.so.2"});
        if (!glib || !gobject || !cairo) return;
        bool ok = true;
#define X(name) ok = ok && (b.name = reinterpret_cast<decltype(&::name)>(dlsym(glib, #name)));
        X(g_bytes_new)
        X(g_bytes_unref)
        X(g_error_free)
#undef X
        ok = ok && (b.gfree = reinterpret_cast<void (*)(void*)>(dlsym(glib, "g_free")));
#define X(name) ok = ok && (b.name = reinterpret_cast<decltype(&::name)>(dlsym(gobject, #name)));
        X(g_object_unref)
#undef X
#define X(name) ok = ok && (b.name = reinterpret_cast<decltype(&::name)>(dlsym(cairo, #name)));
        BLIT_CAIRO_FUNCS(X)
#undef X
        b.ok = ok;
    });
    return b;
}

// What cairo drew (ARGB32: native 32-bit words, colours multiplied by
// alpha) as RGBA bytes.
void cairo_to_rgba(uint8_t* px, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i, px += 4) {
        uint32_t v;
        std::memcpy(&v, px, 4);
        px[0] = static_cast<uint8_t>(v >> 16), px[1] = static_cast<uint8_t>(v >> 8), px[2] = static_cast<uint8_t>(v), px[3] = static_cast<uint8_t>(v >> 24);
    }
}
#endif

// ---------------------------------------------------------------- SVG

#if BLIT_HAVE_RSVG
#define BLIT_RSVG_FUNCS(X)                  \
    X(rsvg_handle_new_from_data)            \
    X(rsvg_handle_get_intrinsic_dimensions) \
    X(rsvg_handle_render_document)

struct Rsvg {
#define X(name) decltype(&::name) name = nullptr;
    BLIT_RSVG_FUNCS(X)
#undef X
    // librsvg 2.52 and later (RHEL 9 has 2.50): the size in pixels, units
    // and all worked out. (Declared here: older headers do not have it.)
    gboolean (*size_in_pixels)(RsvgHandle*, gdouble*, gdouble*) = nullptr;
    bool ok = false;
};

// An SVG length in pixels, at the 96 a CSS inch has (0: a length that is
// not absolute, as a percentage or an em).
double svg_pixels(const RsvgLength& l) {
    switch (l.unit) {
        case RSVG_UNIT_PX: return l.length;
        case RSVG_UNIT_IN: return l.length * 96;
        case RSVG_UNIT_CM: return l.length * 96 / 2.54;
        case RSVG_UNIT_MM: return l.length * 96 / 25.4;
        case RSVG_UNIT_PT: return l.length * 96 / 72;
        case RSVG_UNIT_PC: return l.length * 96 / 6;
        default: return 0;
    }
}

const Rsvg& rsvg() {
    static Rsvg r;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = base().ok ? open_first({"librsvg-2.so.2"}) : nullptr;
        if (!lib) return;
        bool ok = true;
#define X(name) ok = ok && (r.name = reinterpret_cast<decltype(&::name)>(dlsym(lib, #name)));
        BLIT_RSVG_FUNCS(X)
#undef X
        r.size_in_pixels = reinterpret_cast<decltype(r.size_in_pixels)>(dlsym(lib, "rsvg_handle_get_intrinsic_size_in_pixels"));
        r.ok = ok;
    });
    return r;
}
#endif

// An SVG drawn w by h pixels (0, 0: its own size, the longer side at most
// max_side).
int draw_svg(const void* data, size_t size, int w, int h, int max_side, BlitPicture* out, char* err, size_t err_size) {
    *out = BlitPicture{};
#if BLIT_HAVE_RSVG
    const Rsvg& r = rsvg();
    if (!r.ok) {
        say(err, err_size, "SVG is drawn by librsvg, which is not installed");
        return 0;
    }
    const Base& b = base();
    GError* e = nullptr;
    RsvgHandle* svg = r.rsvg_handle_new_from_data(static_cast<const guint8*>(data), size, &e);
    if (!svg) {
        if (e) b.g_error_free(e);
        say(err, err_size, "an SVG this system cannot draw");
        return 0;
    }
    // Its own size; one that says none, its viewBox's, or a browser's 300 by 150.
    double ow = 0, oh = 0;
    if (!r.size_in_pixels || !r.size_in_pixels(svg, &ow, &oh) || ow < 1 || oh < 1) {
        gboolean has_w = false, has_h = false, has_box = false;
        RsvgLength lw{}, lh{};
        RsvgRectangle box{};
        r.rsvg_handle_get_intrinsic_dimensions(svg, &has_w, &lw, &has_h, &lh, &has_box, &box);
        const bool sized = has_w && has_h && svg_pixels(lw) >= 1 && svg_pixels(lh) >= 1;
        const bool boxed = has_box && box.width >= 1 && box.height >= 1;
        ow = sized ? svg_pixels(lw) : boxed ? box.width : 300;
        oh = sized ? svg_pixels(lh) : boxed ? box.height : 150;
    }
    out->full_w = std::max(1, static_cast<int>(std::lround(ow)));
    out->full_h = std::max(1, static_cast<int>(std::lround(oh)));
    if (w <= 0 || h <= 0) {
        const double k = std::min(1.0, max_side / std::max(ow, oh));
        w = std::max(1, static_cast<int>(std::lround(ow * k)));
        h = std::max(1, static_cast<int>(std::lround(oh * k)));
    }
    out->rgba = static_cast<uint8_t*>(std::calloc(static_cast<size_t>(w) * static_cast<size_t>(h), 4));
    cairo_surface_t* s = out->rgba ? b.cairo_image_surface_create_for_data(out->rgba, CAIRO_FORMAT_ARGB32, w, h, w * 4) : nullptr;
    cairo_t* cr = s ? b.cairo_create(s) : nullptr;
    bool ok = cr != nullptr;
    if (ok) {
        const RsvgRectangle view{0, 0, static_cast<double>(w), static_cast<double>(h)};
        ok = r.rsvg_handle_render_document(svg, cr, &view, &e);
        if (e) b.g_error_free(e);
        b.cairo_surface_flush(s);
    }
    if (cr) b.cairo_destroy(cr);
    if (s) b.cairo_surface_destroy(s);
    b.g_object_unref(svg);
    if (!ok) {
        blit_picture_free(out);
        say(err, err_size, "an SVG this system cannot draw");
        return 0;
    }
    cairo_to_rgba(out->rgba, static_cast<size_t>(w) * static_cast<size_t>(h));
    out->w = w;
    out->h = h;
    out->frames = 1;
    out->vector = 1;
    std::snprintf(out->kind, sizeof out->kind, "SVG");
    return 1;
#else
    (void)data, (void)size, (void)w, (void)h, (void)max_side;
    say(err, err_size, "this blit was built without librsvg: no SVG");
    return 0;
#endif
}

}  // namespace

extern "C" {

int blit_picture_read(const void* data, size_t size, int max_side, BlitPicture* out, char* err, size_t err_size) {
    *out = BlitPicture{};
    const uint8_t* b = static_cast<const uint8_t*>(data);
    Pixels p;
    switch (kind_of(data, size)) {
        case kPng: {
            Apng a;
            if (parse_apng(b, size, &a)) {  // (its first frame, as it is first shown)
                const int w = static_cast<int>(be32(a.ihdr.data())), h = static_cast<int>(be32(a.ihdr.data() + 4));
                if (too_many(w, h)) break;
                p.w = w, p.h = h;
                bool got = false;
                play_apng(a, w, h, [&](size_t i, const uint8_t* canvas, int) {
                    if (i == 0) p.px.assign(canvas, canvas + static_cast<size_t>(w) * static_cast<size_t>(h) * 4), got = true;
                });
                if (got) return hand_out(p, max_side, "PNG", static_cast<int>(a.frames.size()), out, err, err_size);
                break;
            }
            if (decode_png(data, size, &p)) return hand_out(p, max_side, "PNG", 1, out, err, err_size);
            say(err, err_size, "the picture could not be read");
            return 0;
        }
        case kJpeg:
            if (decode_jpeg(data, size, max_side, &p)) return hand_out(p, max_side, "JPEG", 1, out, err, err_size);
            say(err, err_size, "the picture could not be read");
            return 0;
        case kGif: {
            GifSource src{b, size, 0};
            GifFileType* g = open_gif(&src);
            if (!g) break;
            p.w = g->SWidth, p.h = g->SHeight;
            const int frames = g->ImageCount;
            if (!too_many(p.w, p.h) && frames > 0) {
                play_gif(g, [&](int i, const uint8_t* canvas, int) {
                    if (i == 0) p.px.assign(canvas, canvas + static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 4);
                });
            }
            int e = 0;
            DGifCloseFile(g, &e);
            if (p.px.empty()) break;
            return hand_out(p, max_side, "GIF", frames, out, err, err_size);
        }
        case kWebp: {
            WebPData wd{b, size};
            WebPDemuxer* dmx = WebPDemux(&wd);
            const int frames = dmx ? static_cast<int>(WebPDemuxGetI(dmx, WEBP_FF_FRAME_COUNT)) : 0;
            if (dmx) WebPDemuxDelete(dmx);
            int w = 0, h = 0;
            uint8_t* rgba = nullptr;
            if (frames > 1) {  // (its first frame, as it is first shown)
                WebPAnimDecoderOptions o;
                WebPAnimDecoderOptionsInit(&o);
                o.color_mode = MODE_RGBA;
                WebPAnimDecoder* dec = WebPAnimDecoderNew(&wd, &o);
                WebPAnimInfo info;
                int ts = 0;
                uint8_t* buf = nullptr;
                if (dec && WebPAnimDecoderGetInfo(dec, &info) && WebPAnimDecoderGetNext(dec, &buf, &ts)) {
                    p.w = static_cast<int>(info.canvas_width), p.h = static_cast<int>(info.canvas_height);
                    p.px.assign(buf, buf + static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 4);
                }
                if (dec) WebPAnimDecoderDelete(dec);
                if (p.px.empty()) break;
                return hand_out(p, max_side, "WEBP", frames, out, err, err_size);
            }
            if (!(rgba = WebPDecodeRGBA(b, size, &w, &h))) break;
            p.w = w, p.h = h;
            p.px.assign(rgba, rgba + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
            WebPFree(rgba);
            return hand_out(p, max_side, "WEBP", 1, out, err, err_size);
        }
        case kSvg: return draw_svg(data, size, 0, 0, max_side, out, err, err_size);
        case kNone: break;
    }
    say(err, err_size, "not a picture this system can read");
    return 0;
}

int blit_picture_draw(const void* data, size_t size, int w, int h, BlitPicture* out, char* err, size_t err_size) {
    if (!is_svg(data, size)) {
        *out = BlitPicture{};
        say(err, err_size, "not a picture drawn from shapes");
        return 0;
    }
    return draw_svg(data, size, w > 0 ? w : 1, h > 0 ? h : 1, 0, out, err, err_size);
}

void blit_picture_free(BlitPicture* p) {
    if (!p) return;
    std::free(p->rgba);
    p->rgba = nullptr;
}

const char* blit_picture_readers(void) {
    return "libpng, libjpeg-turbo, giflib and libwebp; librsvg (SVG) and poppler (PDF), loaded when first wanted";
}

// ---------------------------------------------------------------- animations

int blit_frames_read(const void* data, size_t size, int max_side, size_t max_bytes, BlitFrames* out) {
    *out = BlitFrames{};
    const uint8_t* b = static_cast<const uint8_t*>(data);
    // Each frame as it is shown, made smaller to fit: max_bytes for all of
    // them, max_side for each side (as ImageIO's are read on a Mac).
    int fw = 0, fh = 0, count = 0;
    auto make = [&](int loops) {
        double k = std::min(1.0, static_cast<double>(max_side) / std::max(fw, fh));
        const double all = static_cast<double>(fw) * fh * 4.0 * count;
        if (all * k * k > static_cast<double>(max_bytes)) k = std::sqrt(static_cast<double>(max_bytes) / all);
        out->w = std::max(1, static_cast<int>(std::floor(fw * k)));
        out->h = std::max(1, static_cast<int>(std::floor(fh * k)));
        out->frames = count;
        out->loops = loops;
        out->rgba = static_cast<uint8_t*>(std::calloc(static_cast<size_t>(count), static_cast<size_t>(out->w) * static_cast<size_t>(out->h) * 4));
        out->ms = static_cast<int*>(std::calloc(static_cast<size_t>(count), sizeof(int)));
        if (out->rgba && out->ms) return true;
        blit_frames_free(out);
        *out = BlitFrames{};
        return false;
    };
    std::vector<uint8_t> scratch;
    auto put = [&](int i, const uint8_t* canvas, int ms) {
        if (i < 0 || i >= count) return;
        scratch.assign(canvas, canvas + static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4);
        premultiply(scratch.data(), static_cast<size_t>(fw) * static_cast<size_t>(fh));
        shrink(scratch.data(), fw, fh, out->rgba + static_cast<size_t>(i) * static_cast<size_t>(out->w) * static_cast<size_t>(out->h) * 4, out->w, out->h);
        out->ms[i] = ms <= 10 ? 100 : ms;
    };
    switch (kind_of(data, size)) {
        case kPng: {
            Apng a;
            if (!parse_apng(b, size, &a) || a.frames.size() < 2) return 0;
            fw = static_cast<int>(be32(a.ihdr.data())), fh = static_cast<int>(be32(a.ihdr.data() + 4));
            count = static_cast<int>(a.frames.size());
            if (too_many(fw, fh) || !make(static_cast<int>(a.loops))) return 0;
            play_apng(a, fw, fh, [&](size_t i, const uint8_t* canvas, int ms) { put(static_cast<int>(i), canvas, ms); });
            return 1;
        }
        case kGif: {
            GifSource src{b, size, 0};
            GifFileType* g = open_gif(&src);
            if (!g) return 0;
            fw = g->SWidth, fh = g->SHeight, count = g->ImageCount;
            const bool ok = count >= 2 && !too_many(fw, fh) && make(gif_loops(g));
            if (ok) play_gif(g, [&](int i, const uint8_t* canvas, int ms) { put(i, canvas, ms); });
            int e = 0;
            DGifCloseFile(g, &e);
            return ok;
        }
        case kWebp: {
            WebPData wd{b, size};
            WebPAnimDecoderOptions o;
            WebPAnimDecoderOptionsInit(&o);
            o.color_mode = MODE_RGBA;
            WebPAnimDecoder* dec = WebPAnimDecoderNew(&wd, &o);
            WebPAnimInfo info;
            if (!dec || !WebPAnimDecoderGetInfo(dec, &info) || info.frame_count < 2) {
                if (dec) WebPAnimDecoderDelete(dec);
                return 0;
            }
            fw = static_cast<int>(info.canvas_width), fh = static_cast<int>(info.canvas_height), count = static_cast<int>(info.frame_count);
            const bool ok = make(static_cast<int>(info.loop_count));
            int prev = 0, i = 0, ts = 0;
            uint8_t* buf = nullptr;
            while (ok && i < count && WebPAnimDecoderGetNext(dec, &buf, &ts)) {  // (each frame's time is when it ends)
                put(i++, buf, ts - prev);
                prev = ts;
            }
            WebPAnimDecoderDelete(dec);
            return ok;
        }
        default: return 0;
    }
}

void blit_frames_free(BlitFrames* f) {
    if (!f) return;
    std::free(f->rgba);
    std::free(f->ms);
    f->rgba = nullptr;
    f->ms = nullptr;
}

// ---------------------------------------------------------------- the web

// curl, in a process of its own: what it writes is the answer, what it says
// on its error stream why there is none. (blit keeps what it fetches; curl
// keeps nothing.)
int blit_fetch(const char* url, int timeout_ms, size_t max_bytes, char** data, size_t* size, char* err, size_t err_size) {
    *data = nullptr;
    *size = 0;
    const std::string_view u = url ? url : "";
    if (!u.starts_with("http://") && !u.starts_with("https://")) {
        say(err, err_size, "not a web address");
        return 0;
    }
    int out[2], er[2];
    if (pipe(out) != 0) return say(err, err_size, "cannot make a pipe"), 0;
    if (pipe(er) != 0) {
        close(out[0]), close(out[1]);
        return say(err, err_size, "cannot make a pipe"), 0;
    }
    for (const int fd : {out[0], er[0]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, er[1], 2);
#if defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (nothing else this process has open goes with it)
#endif
    const std::string secs = std::to_string(std::max(1, (timeout_ms + 999) / 1000)), most = std::to_string(max_bytes);
    std::string words[] = {"curl", "-fsSL", "--proto", "=http,https", "--max-time", secs, "--max-filesize", most, "--", std::string(u)};
    char* argv[std::size(words) + 1];
    for (size_t i = 0; i < std::size(words); ++i) argv[i] = words[i].data();
    argv[std::size(words)] = nullptr;
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, "curl", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]), close(er[1]);
    if (rc != 0) {
        close(out[0]), close(er[0]);
        say(err, err_size, "cannot run curl to fetch it");
        return 0;
    }
    std::string got, said;
    bool too_large = false, late = false;
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (bool out_open = true, er_open = true; out_open || er_open;) {
        pollfd fds[2] = {{out_open ? out[0] : -1, POLLIN, 0}, {er_open ? er[0] : -1, POLLIN, 0}};
        const int n = poll(fds, 2, 200);
        if (n < 0 && errno != EINTR) break;
        timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 > timeout_ms + 2000) {
            late = true;
            break;
        }
        char buf[65536];
        if (out_open && (fds[0].revents & (POLLIN | POLLHUP))) {
            const ssize_t k = read(out[0], buf, sizeof buf);
            if (k > 0) got.append(buf, static_cast<size_t>(k));
            else if (k == 0 || errno != EINTR) out_open = false;
            if (got.size() > max_bytes) {
                too_large = true;
                break;
            }
        }
        if (er_open && (fds[1].revents & (POLLIN | POLLHUP))) {
            const ssize_t k = read(er[0], buf, sizeof buf);
            if (k > 0 && said.size() < 4096) said.append(buf, static_cast<size_t>(k));
            else if (k == 0 || (k < 0 && errno != EINTR)) er_open = false;
        }
    }
    close(out[0]), close(er[0]);
    if (late || too_large) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (late) return say(err, err_size, "the server took too long"), 0;
    if (too_large || (WIFEXITED(status) && WEXITSTATUS(status) == 63)) return say(err, err_size, "it is too large"), 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        // "curl: (22) The requested URL returned error: 404": the part after the number.
        while (!said.empty() && (said.back() == '\n' || said.back() == '\r')) said.pop_back();
        const size_t close_paren = said.find(") ");
        if (said.starts_with("curl: (") && close_paren != std::string::npos) said.erase(0, close_paren + 2);
        say(err, err_size, said.empty() ? "it could not be fetched" : said.c_str());
        return 0;
    }
    *data = static_cast<char*>(std::malloc(got.size() ? got.size() : 1));
    if (!*data) return 0;
    std::memcpy(*data, got.data(), got.size());
    *size = got.size();
    return 1;
}

}  // extern "C"

// ---------------------------------------------------------------- PDF

#if BLIT_HAVE_POPPLER
namespace {

#define BLIT_POPPLER_FUNCS(X)          \
    X(poppler_document_new_from_bytes) \
    X(poppler_document_get_n_pages)    \
    X(poppler_document_get_page)       \
    X(poppler_document_find_dest)      \
    X(poppler_page_get_size)           \
    X(poppler_page_render)             \
    X(poppler_page_get_text)           \
    X(poppler_page_get_text_layout)    \
    X(poppler_page_get_link_mapping)   \
    X(poppler_page_free_link_mapping)  \
    X(poppler_dest_free)               \
    X(poppler_action_free)             \
    X(poppler_index_iter_new)          \
    X(poppler_index_iter_get_action)   \
    X(poppler_index_iter_get_child)    \
    X(poppler_index_iter_next)         \
    X(poppler_index_iter_free)

struct Poppler {
#define X(name) decltype(&::name) name = nullptr;
    BLIT_POPPLER_FUNCS(X)
#undef X
    bool ok = false;
};

const Poppler& poppler() {
    static Poppler p;
    static std::once_flag once;
    std::call_once(once, [] {
        void* lib = base().ok ? open_first({"libpoppler-glib.so.8"}) : nullptr;
        if (!lib) return;
        bool ok = true;
#define X(name) ok = ok && (p.name = reinterpret_cast<decltype(&::name)>(dlsym(lib, #name)));
        BLIT_POPPLER_FUNCS(X)
#undef X
        p.ok = ok;
    });
    return p;
}

}  // namespace

struct BlitPdf {
    std::string bytes;  // (kept for the password: poppler holds its own copy)
    PopplerDocument* doc = nullptr;
    int locked = 0, pages = 0;
};

namespace {

// A document from these bytes (null if it cannot be, and whether a password
// is what it wants).
PopplerDocument* open_doc(const std::string& bytes, const char* password, bool* locked) {
    const Base& b = base();
    const Poppler& p = poppler();
    GBytes* g = b.g_bytes_new(bytes.data(), bytes.size());
    GError* e = nullptr;
    PopplerDocument* doc = p.poppler_document_new_from_bytes(g, password, &e);
    b.g_bytes_unref(g);
    if (locked) *locked = !doc && e && e->code == POPPLER_ERROR_ENCRYPTED;
    if (e) b.g_error_free(e);
    return doc;
}

PopplerPage* page_of(const BlitPdf* pdf, int page) {
    if (!pdf || pdf->locked || page < 0 || page >= pdf->pages) return nullptr;
    return poppler().poppler_document_get_page(pdf->doc, page);
}

// Where a destination goes: its page (from 0), and how far down it as it is
// shown (its top, if it does not say). PDF's own y counts up from the bottom.
bool place_of(BlitPdf* pdf, PopplerDest* d, int* page, float* y) {
    if (!d) return false;
    const Poppler& p = poppler();
    PopplerDest* named = d->type == POPPLER_DEST_NAMED ? p.poppler_document_find_dest(pdf->doc, d->named_dest) : nullptr;
    const PopplerDest* dest = named ? named : d;
    bool ok = dest->type != POPPLER_DEST_NAMED && dest->type != POPPLER_DEST_UNKNOWN && dest->page_num >= 1 && dest->page_num <= pdf->pages;
    if (ok) {
        *page = dest->page_num - 1;
        *y = 0;
        if (dest->change_top || dest->type == POPPLER_DEST_FITH || dest->type == POPPLER_DEST_FITBH) {
            double w = 0, h = 0;
            if (PopplerPage* pg = page_of(pdf, *page)) {
                p.poppler_page_get_size(pg, &w, &h);
                base().g_object_unref(pg);
            }
            *y = static_cast<float>(std::max(0.0, h - dest->top));
        }
    }
    if (named) p.poppler_dest_free(named);
    return ok;
}

}  // namespace

extern "C" {

BlitPdf* blit_pdf_open(const void* data, size_t size, char* err, size_t err_size) {
    const size_t head = std::min<size_t>(size, 1024);
    if (head < 5 || !memmem(data, head, "%PDF-", 5)) {
        say(err, err_size, "not a PDF this system can read");
        return nullptr;
    }
    if (!poppler().ok) {
        say(err, err_size, "PDFs are read by poppler (poppler-glib), which is not installed");
        return nullptr;
    }
    BlitPdf* pdf = new BlitPdf;
    pdf->bytes.assign(static_cast<const char*>(data), size);
    bool locked = false;
    pdf->doc = open_doc(pdf->bytes, nullptr, &locked);
    if (!pdf->doc && !locked) {
        delete pdf;
        say(err, err_size, "not a PDF this system can read");
        return nullptr;
    }
    pdf->locked = locked;
    pdf->pages = pdf->doc ? poppler().poppler_document_get_n_pages(pdf->doc) : 0;
    if (!locked && pdf->pages <= 0) {
        blit_pdf_close(pdf);
        say(err, err_size, "it has no pages");
        return nullptr;
    }
    return pdf;
}

void blit_pdf_close(BlitPdf* pdf) {
    if (!pdf) return;
    if (pdf->doc) base().g_object_unref(pdf->doc);
    delete pdf;
}

int blit_pdf_locked(const BlitPdf* pdf) { return pdf && pdf->locked; }

int blit_pdf_unlock(BlitPdf* pdf, const char* password) {
    if (!pdf || !pdf->locked) return pdf != nullptr;
    PopplerDocument* doc = open_doc(pdf->bytes, password ? password : "", nullptr);
    if (!doc) return 0;
    const int pages = poppler().poppler_document_get_n_pages(doc);
    if (pages <= 0) {
        base().g_object_unref(doc);
        return 0;
    }
    pdf->doc = doc;
    pdf->pages = pages;
    pdf->locked = 0;
    return 1;
}

int blit_pdf_pages(const BlitPdf* pdf) { return pdf && !pdf->locked ? pdf->pages : 0; }

// (poppler's page size is as the page is shown: turned, if it says so.)
void blit_pdf_page_size(const BlitPdf* pdf, int page, double* w, double* h) {
    *w = 612, *h = 792;
    if (PopplerPage* p = page_of(pdf, page)) {
        poppler().poppler_page_get_size(p, w, h);
        base().g_object_unref(p);
    }
}

int blit_pdf_render(const BlitPdf* pdf, int page, double scale, int x, int y, int w, int h, BlitPicture* out) {
    *out = BlitPicture{};
    PopplerPage* p = w > 0 && h > 0 && scale > 0 ? page_of(pdf, page) : nullptr;
    if (!p) return 0;
    const Base& b = base();
    double pw = 0, ph = 0;
    poppler().poppler_page_get_size(p, &pw, &ph);
    out->rgba = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    cairo_surface_t* s = out->rgba ? b.cairo_image_surface_create_for_data(out->rgba, CAIRO_FORMAT_ARGB32, w, h, w * 4) : nullptr;
    cairo_t* cr = s ? b.cairo_create(s) : nullptr;
    if (cr) {
        // On white; the part wanted, from the page's top left, scale pixels to a point.
        b.cairo_set_source_rgb(cr, 1, 1, 1);
        b.cairo_paint(cr);
        b.cairo_translate(cr, -x, -y);
        b.cairo_scale(cr, scale, scale);
        poppler().poppler_page_render(p, cr);
        b.cairo_destroy(cr);
        b.cairo_surface_flush(s);
    }
    if (s) b.cairo_surface_destroy(s);
    b.g_object_unref(p);
    if (!cr) {
        blit_picture_free(out);
        return 0;
    }
    cairo_to_rgba(out->rgba, static_cast<size_t>(w) * static_cast<size_t>(h));
    out->w = w;
    out->h = h;
    out->full_w = static_cast<int>(std::lround(pw * scale));
    out->full_h = static_cast<int>(std::lround(ph * scale));
    out->frames = 1;
    std::snprintf(out->kind, sizeof out->kind, "PDF");
    return 1;
}

int blit_pdf_text(BlitPdf* pdf, int page, int boxes, BlitPdfText* out) {
    *out = BlitPdfText{};
    PopplerPage* p = page_of(pdf, page);
    if (!p) return 0;
    const Base& b = base();
    const Poppler& pp = poppler();
    char* raw = pp.poppler_page_get_text(p);
    std::string text = raw ? raw : "";
    if (raw) b.gfree(raw);
    // poppler ends each line with a line break, the last one too: as PDFKit
    // has it, there is none after the last.
    while (!text.empty() && text.back() == '\n') text.pop_back();
    // Its characters as UTF-16 units (PDFKit's count), each where it starts in the text.
    std::vector<uint32_t> at;
    std::vector<uint32_t> char_of_unit;  // (which of poppler's characters each unit is)
    at.reserve(text.size() + 1);
    uint32_t ci = 0;
    for (size_t i = 0; i < text.size(); ++ci) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        at.push_back(static_cast<uint32_t>(i));
        char_of_unit.push_back(ci);
        if (len == 4) {  // (a pair: the second unit starts after the character)
            at.push_back(static_cast<uint32_t>(std::min(i + len, text.size())));
            char_of_unit.push_back(ci);
        }
        i = std::min(i + len, text.size());
    }
    const size_t n = at.size();
    at.push_back(static_cast<uint32_t>(text.size()));
    out->text = static_cast<char*>(std::malloc(text.size() + 1));
    out->at = static_cast<uint32_t*>(std::malloc((n + 1) * sizeof(uint32_t)));
    if (!out->text || !out->at) {
        b.g_object_unref(p);
        blit_pdf_text_free(out);
        return 0;
    }
    std::memcpy(out->text, text.data(), text.size());
    out->text[text.size()] = 0;
    std::memcpy(out->at, at.data(), (n + 1) * sizeof(uint32_t));
    out->count = static_cast<int>(n);
    if (!boxes || n == 0) {
        b.g_object_unref(p);
        return 1;
    }
    // Where each character is (a box for each of poppler's characters, the
    // line breaks too), and a line for each run between breaks, as tall as
    // its letters: from the top left of the page as it is shown.
    PopplerRectangle* rects = nullptr;
    guint rn = 0;
    pp.poppler_page_get_text_layout(p, &rects, &rn);
    b.g_object_unref(p);
    out->box = static_cast<float*>(std::malloc(n * 4 * sizeof(float)));
    out->line = static_cast<int*>(std::malloc(n * sizeof(int)));
    out->line_box = static_cast<float*>(std::malloc((n + 1) * 4 * sizeof(float)));
    if (!out->box || !out->line || !out->line_box) {
        if (rects) b.gfree(rects);
        blit_pdf_text_free(out);
        return 0;
    }
    int lines = 0;
    bool open_line = false;
    for (size_t i = 0; i < n; ++i) {
        float* bx = out->box + i * 4;
        const bool brk = text[at[i]] == '\n';
        const uint32_t c = char_of_unit[i];
        if (!brk && c < rn) {
            const PopplerRectangle& r = rects[c];
            bx[0] = static_cast<float>(r.x1), bx[1] = static_cast<float>(r.y1), bx[2] = static_cast<float>(r.x2), bx[3] = static_cast<float>(r.y2);
            if (!open_line) {
                float* lb = out->line_box + lines * 4;
                lb[0] = lb[1] = 1e9f, lb[2] = lb[3] = -1e9f;
                ++lines;
                open_line = true;
            }
            float* lb = out->line_box + (lines - 1) * 4;
            lb[0] = std::min(lb[0], bx[0]), lb[1] = std::min(lb[1], bx[1]), lb[2] = std::max(lb[2], bx[2]), lb[3] = std::max(lb[3], bx[3]);
            out->line[i] = lines - 1;
        } else {
            out->line[i] = -1;  // (a break, or a character with no box: placed below)
            if (brk) open_line = false;
        }
    }
    if (rects) b.gfree(rects);
    // What is on no line goes with the character before it: no wider than
    // nothing, at its end (as on a Mac).
    for (size_t i = 0; i < n; ++i) {
        if (out->line[i] >= 0) continue;
        const int l = i > 0 ? out->line[i - 1] : (lines ? 0 : -1);
        out->line[i] = l < 0 ? 0 : l;
        float* bx = out->box + i * 4;
        const float x = i > 0 ? out->box[(i - 1) * 4 + 2] : (lines ? out->line_box[0] : 0);
        bx[0] = bx[2] = x;
        bx[1] = lines ? out->line_box[out->line[i] * 4 + 1] : 0;
        bx[3] = lines ? out->line_box[out->line[i] * 4 + 3] : 0;
    }
    if (lines == 0) {  // (no box at all: one line, nowhere)
        lines = 1;
        std::fill(out->line_box, out->line_box + 4, 0.0f);
    }
    out->lines = lines;
    return 1;
}

void blit_pdf_text_free(BlitPdfText* t) {
    if (!t) return;
    std::free(t->text);
    std::free(t->at);
    std::free(t->box);
    std::free(t->line);
    std::free(t->line_box);
    *t = BlitPdfText{};
}

int blit_pdf_links(BlitPdf* pdf, int page, BlitPdfLink** out) {
    *out = nullptr;
    PopplerPage* p = page_of(pdf, page);
    if (!p) return 0;
    const Poppler& pp = poppler();
    double pw = 0, ph = 0;
    pp.poppler_page_get_size(p, &pw, &ph);
    GList* all = pp.poppler_page_get_link_mapping(p);
    int count = 0;
    for (GList* l = all; l; l = l->next) ++count;
    BlitPdfLink* links = count ? static_cast<BlitPdfLink*>(std::calloc(static_cast<size_t>(count), sizeof *links)) : nullptr;
    int n = 0;
    for (GList* l = all; l && links; l = l->next) {
        const PopplerLinkMapping* m = static_cast<const PopplerLinkMapping*>(l->data);
        const PopplerAction* a = m->action;
        if (!a) continue;
        BlitPdfLink* k = links + n;
        k->page = -1;
        if (a->type == POPPLER_ACTION_URI && a->uri.uri && *a->uri.uri) k->url = strdup(a->uri.uri);
        else if (a->type != POPPLER_ACTION_GOTO_DEST || !place_of(pdf, a->goto_dest.dest, &k->page, &k->y)) continue;
        // (A link's area counts up from the bottom of the page, as PDF does.)
        k->box[0] = static_cast<float>(m->area.x1);
        k->box[1] = static_cast<float>(ph - m->area.y2);
        k->box[2] = static_cast<float>(m->area.x2);
        k->box[3] = static_cast<float>(ph - m->area.y1);
        ++n;
    }
    pp.poppler_page_free_link_mapping(all);
    base().g_object_unref(p);
    if (!n) {
        std::free(links);
        return 0;
    }
    *out = links;
    return n;
}

void blit_pdf_links_free(BlitPdfLink* links, int count) {
    if (!links) return;
    for (int i = 0; i < count; ++i) std::free(links[i].url);
    std::free(links);
}

int blit_pdf_outline(BlitPdf* pdf, BlitPdfHeading** out) {
    *out = nullptr;
    if (!pdf || pdf->locked) return 0;
    const Poppler& pp = poppler();
    PopplerIndexIter* root = pp.poppler_index_iter_new(pdf->doc);
    if (!root) return 0;
    // Depth first, in order, at most a few thousand (a stack of where each level is).
    enum { kMost = 4096, kDeepest = 32 };
    BlitPdfHeading* all = static_cast<BlitPdfHeading*>(std::calloc(kMost, sizeof *all));
    PopplerIndexIter* at[kDeepest] = {root};
    bool more[kDeepest] = {true};
    int depth = 0, n = 0;
    while (all && depth >= 0 && n < kMost) {
        if (!more[depth]) {
            pp.poppler_index_iter_free(at[depth]);
            if (--depth >= 0) more[depth] = pp.poppler_index_iter_next(at[depth]);
            continue;
        }
        PopplerAction* a = pp.poppler_index_iter_get_action(at[depth]);
        BlitPdfHeading* h = all + n++;
        h->page = -1;
        h->depth = depth;
        h->title = strdup(a && a->any.title ? a->any.title : "");
        if (a && a->type == POPPLER_ACTION_GOTO_DEST) place_of(pdf, a->goto_dest.dest, &h->page, &h->y);
        if (a) pp.poppler_action_free(a);
        PopplerIndexIter* kid = depth + 1 < kDeepest ? pp.poppler_index_iter_get_child(at[depth]) : nullptr;
        if (kid) {
            at[++depth] = kid;
            more[depth] = true;
        } else {
            more[depth] = pp.poppler_index_iter_next(at[depth]);
        }
    }
    for (; depth >= 0; --depth) pp.poppler_index_iter_free(at[depth]);  // (stopped at kMost)
    if (!n) {
        std::free(all);
        return 0;
    }
    *out = all;
    return n;
}

void blit_pdf_outline_free(BlitPdfHeading* headings, int count) {
    if (!headings) return;
    for (int i = 0; i < count; ++i) std::free(headings[i].title);
    std::free(headings);
}

}  // extern "C"

#else  // no poppler at build time

struct BlitPdf {};

extern "C" {
BlitPdf* blit_pdf_open(const void*, size_t, char* err, size_t err_size) {
    say(err, err_size, "this blit was built without poppler: no PDFs");
    return nullptr;
}
void blit_pdf_close(BlitPdf*) {}
int blit_pdf_locked(const BlitPdf*) { return 0; }
int blit_pdf_unlock(BlitPdf*, const char*) { return 0; }
int blit_pdf_pages(const BlitPdf*) { return 0; }
void blit_pdf_page_size(const BlitPdf*, int, double* w, double* h) { *w = 612, *h = 792; }
int blit_pdf_render(const BlitPdf*, int, double, int, int, int, int, BlitPicture* out) {
    *out = BlitPicture{};
    return 0;
}
int blit_pdf_text(BlitPdf*, int, int, BlitPdfText* out) {
    *out = BlitPdfText{};
    return 0;
}
void blit_pdf_text_free(BlitPdfText*) {}
int blit_pdf_links(BlitPdf*, int, BlitPdfLink** out) {
    *out = nullptr;
    return 0;
}
void blit_pdf_links_free(BlitPdfLink*, int) {}
int blit_pdf_outline(BlitPdf*, BlitPdfHeading** out) {
    *out = nullptr;
    return 0;
}
void blit_pdf_outline_free(BlitPdfHeading*, int) {}
}  // extern "C"

#endif
