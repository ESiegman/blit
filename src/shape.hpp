#pragma once
// Text shaping through HarfBuzz, for ligatures and OpenType font features.
//
// HarfBuzz is not linked into the program. It is loaded from its shared
// library the first time a font asks for shaping, so launches that do not
// use ligatures pay nothing for it, and those that do load it on a
// background thread (see App::init).
#include <span>
#include <string>
#include <vector>

#include "common.hpp"

namespace blit {

struct ShapedGlyph {
    u32 gid;       // glyph index in the font
    u32 cluster;   // byte offset of the character it came from
    i32 dx, dy;    // offset from the pen position, pixels
};

class Shaper {
public:
    Shaper() = default;
    Shaper(const Shaper&) = delete;
    Shaper& operator=(const Shaper&) = delete;
    ~Shaper();

    // `features` are OpenType feature strings as HarfBuzz reads them: "ss01",
    // "-calt", "zero=1". With `ligatures` false the features that join
    // characters (calt, liga, clig, rlig) are switched off.
    bool open(const std::string& font_path, unsigned face_index, float px, const std::vector<std::string>& features,
              bool ligatures, std::string* err);
    bool ok() const { return font_ != nullptr; }

    // Shapes [text, text+len). The result is valid until the next call.
    std::span<const ShapedGlyph> shape(const char* text, u32 len);

    // Marks the ASCII characters that a substitution could change depending
    // on their neighbours. Text made of other characters never needs shaping.
    void contextual_ascii(bool out[128]);

private:
    void* blob_ = nullptr;
    void* face_ = nullptr;
    void* font_ = nullptr;
    void* buffer_ = nullptr;
    std::vector<unsigned char> features_;  // hb_feature_t array, kept opaque here
    std::vector<u32> context_tags_;        // features that can depend on neighbours
    std::vector<ShapedGlyph> out_;
};

}  // namespace blit
