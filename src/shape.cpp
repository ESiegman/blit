#include "shape.hpp"

#include <cmath>
#include <cstring>

#if BLIT_HAVE_HARFBUZZ

#include <hb-ot.h>
#include <hb.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace blit {

namespace {

// Every HarfBuzz function used. The pointer types come straight from the
// declarations in hb.h, so a signature can never drift from the header.
#define BLIT_HB_FUNCS(X)                  \
    X(hb_blob_create_from_file)           \
    X(hb_blob_destroy)                    \
    X(hb_face_create)                     \
    X(hb_face_destroy)                    \
    X(hb_font_create)                     \
    X(hb_font_destroy)                    \
    X(hb_font_set_scale)                  \
    X(hb_font_get_nominal_glyph)          \
    X(hb_buffer_create)                   \
    X(hb_buffer_destroy)                  \
    X(hb_buffer_clear_contents)           \
    X(hb_buffer_add_utf8)                 \
    X(hb_buffer_set_direction)            \
    X(hb_buffer_set_script)               \
    X(hb_buffer_set_language)             \
    X(hb_buffer_get_glyph_infos)          \
    X(hb_buffer_get_glyph_positions)      \
    X(hb_language_from_string)            \
    X(hb_feature_from_string)             \
    X(hb_shape)                           \
    X(hb_set_create)                      \
    X(hb_set_destroy)                     \
    X(hb_set_next)                        \
    X(hb_set_has)                         \
    X(hb_ot_layout_collect_lookups)       \
    X(hb_ot_layout_lookup_collect_glyphs)

struct Api {
#define X(name) decltype(&::name) name = nullptr;
    BLIT_HB_FUNCS(X)
#undef X
    bool loaded = false;
    bool tried = false;
};
Api hb;

void* open_library(const char* name) {
#if defined(_WIN32)
    return LoadLibraryA(name);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

bool load_harfbuzz() {
    if (hb.tried) return hb.loaded;
    hb.tried = true;
    static const char* const names[] = {
#ifdef BLIT_HARFBUZZ_LIBRARY
        BLIT_HARFBUZZ_LIBRARY,  // where the build found it
#endif
        "libharfbuzz.0.dylib", "libharfbuzz.so.0", "libharfbuzz-0.dll", "harfbuzz.dll",
    };
    void* lib = nullptr;
    for (const char* n : names)
        if ((lib = open_library(n))) break;
    if (!lib) return false;
    bool ok = true;
#define X(name)                                                          \
    hb.name = reinterpret_cast<decltype(&::name)>(find_symbol(lib, #name)); \
    ok = ok && hb.name;
    BLIT_HB_FUNCS(X)
#undef X
    hb.loaded = ok;
    return ok;
}

// Features that replace one glyph with another whatever is around it. They
// are applied when a character is shaped alone, so they never make a
// character depend on its neighbours.
bool is_single_substitution(hb_tag_t t) {
    const char c[4] = {static_cast<char>(t >> 24), static_cast<char>(t >> 16), static_cast<char>(t >> 8),
                       static_cast<char>(t)};
    if ((c[0] == 's' && c[1] == 's') || (c[0] == 'c' && c[1] == 'v')) return true;
    static const char* const single[] = {"zero", "onum", "lnum", "pnum", "tnum", "case", "salt",
                                         "smcp", "c2sc", "sups", "subs", "sinf", "numr", "dnom"};
    for (const char* s : single)
        if (!std::memcmp(c, s, 4)) return true;
    return false;
}

}  // namespace

Shaper::~Shaper() {
    if (!hb.loaded) return;
    if (buffer_) hb.hb_buffer_destroy(static_cast<hb_buffer_t*>(buffer_));
    if (font_) hb.hb_font_destroy(static_cast<hb_font_t*>(font_));
    if (face_) hb.hb_face_destroy(static_cast<hb_face_t*>(face_));
    if (blob_) hb.hb_blob_destroy(static_cast<hb_blob_t*>(blob_));
}

bool Shaper::open(const std::string& font_path, unsigned face_index, float px, const std::vector<std::string>& features,
                  bool ligatures, std::string* err) {
    if (!load_harfbuzz()) {
        if (err) *err = "HarfBuzz library not found; ligatures and font features are off";
        return false;
    }
    hb_blob_t* blob = hb.hb_blob_create_from_file(font_path.c_str());
    hb_face_t* face = hb.hb_face_create(blob, face_index);
    hb_font_t* font = hb.hb_font_create(face);
    const int scale = static_cast<int>(std::lround(px * 64));  // positions come back in 1/64 pixel
    hb.hb_font_set_scale(font, scale, scale);
    blob_ = blob;
    face_ = face;
    font_ = font;
    buffer_ = hb.hb_buffer_create();

    static const hb_tag_t joining[] = {HB_TAG('c', 'a', 'l', 't'), HB_TAG('l', 'i', 'g', 'a'),
                                       HB_TAG('c', 'l', 'i', 'g'), HB_TAG('r', 'l', 'i', 'g')};
    std::vector<hb_feature_t> fs;
    if (ligatures) {
        context_tags_.assign(std::begin(joining), std::end(joining));
    } else {
        for (hb_tag_t t : joining) fs.push_back({t, 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
    }
    for (const std::string& s : features) {
        hb_feature_t f;
        if (!hb.hb_feature_from_string(s.c_str(), static_cast<int>(s.size()), &f)) {
            if (err) *err = "unknown font feature: " + s;
            continue;
        }
        fs.push_back(f);
        std::erase(context_tags_, f.tag);
        if (f.value && ligatures && !is_single_substitution(f.tag)) context_tags_.push_back(f.tag);
    }
    features_.resize(fs.size() * sizeof(hb_feature_t));
    if (!fs.empty()) std::memcpy(features_.data(), fs.data(), features_.size());
    return true;
}

std::span<const ShapedGlyph> Shaper::shape(const char* text, u32 len) {
    out_.clear();
    if (!font_) return out_;
    hb_buffer_t* buf = static_cast<hb_buffer_t*>(buffer_);
    hb.hb_buffer_clear_contents(buf);
    hb.hb_buffer_add_utf8(buf, text, static_cast<int>(len), 0, static_cast<int>(len));
    hb.hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb.hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb.hb_buffer_set_language(buf, hb.hb_language_from_string("en", 2));
    hb.hb_shape(static_cast<hb_font_t*>(font_), buf, reinterpret_cast<const hb_feature_t*>(features_.data()),
                static_cast<unsigned>(features_.size() / sizeof(hb_feature_t)));
    unsigned n = 0;
    const hb_glyph_info_t* info = hb.hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb.hb_buffer_get_glyph_positions(buf, &n);
    for (unsigned i = 0; i < n; ++i)
        out_.push_back({info[i].codepoint, info[i].cluster, (pos[i].x_offset + 32) >> 6, (pos[i].y_offset + 32) >> 6});
    return out_;
}

void Shaper::contextual_ascii(bool out[128]) {
    std::memset(out, 0, 128);
    if (!font_ || context_tags_.empty()) return;
    hb_face_t* face = static_cast<hb_face_t*>(face_);
    std::vector<hb_tag_t> tags(context_tags_.begin(), context_tags_.end());
    tags.push_back(HB_TAG_NONE);
    hb_set_t* lookups = hb.hb_set_create();
    hb_set_t* glyphs = hb.hb_set_create();
    hb.hb_ot_layout_collect_lookups(face, HB_OT_TAG_GSUB, nullptr, nullptr, tags.data(), lookups);
    // Every glyph a lookup reads, as input or as surrounding context.
    for (hb_codepoint_t l = HB_SET_VALUE_INVALID; hb.hb_set_next(lookups, &l);)
        hb.hb_ot_layout_lookup_collect_glyphs(face, HB_OT_TAG_GSUB, l, glyphs, glyphs, glyphs, nullptr);
    for (u32 c = 33; c < 127; ++c) {
        hb_codepoint_t nominal = 0;
        hb.hb_font_get_nominal_glyph(static_cast<hb_font_t*>(font_), c, &nominal);
        const char ch = static_cast<char>(c);
        auto alone = shape(&ch, 1);
        out[c] = hb.hb_set_has(glyphs, nominal) || (alone.size() == 1 && hb.hb_set_has(glyphs, alone[0].gid));
    }
    hb.hb_set_destroy(glyphs);
    hb.hb_set_destroy(lookups);
}

}  // namespace blit

#else  // built without HarfBuzz headers: shaping is simply unavailable

namespace blit {
Shaper::~Shaper() = default;
bool Shaper::open(const std::string&, unsigned, float, const std::vector<std::string>&, bool, std::string* err) {
    if (err) *err = "built without HarfBuzz; ligatures and font features are off";
    return false;
}
std::span<const ShapedGlyph> Shaper::shape(const char*, u32) { return {}; }
void Shaper::contextual_ascii(bool out[128]) { std::memset(out, 0, 128); }
}  // namespace blit

#endif
