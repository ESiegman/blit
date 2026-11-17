#pragma once
// Monospace glyph atlas. The atlas is a grid of cell-sized slots in one
// 8-bit coverage image; slot N for N < 128 is ASCII character N, so plain
// text maps to slots with no lookup at all. Other characters get slots on
// first use.
//
// Styles: italic, bold and bold italic each get a block of 128 slots right
// after the regular one, so an ASCII character in style s is slot
// `character | s << 7` and a run of text is styled by or-ing bits into its
// cells. The glyphs come from the family's own italic and bold faces; where
// a family has none, from a weight axis or by slanting and thickening the
// regular outlines.
//
// Ligatures: programming fonts replace "->" with blank spacer glyphs followed
// by one wide glyph that reaches back over the spacers. A cell still holds
// exactly one slot, so such a glyph is cut into cell-wide slices, one slot
// each. ligate() rewrites a row of cells accordingly.
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.hpp"
#include "shape.hpp"

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace blit {

struct FontSpec {
    std::string family;             // family name or a font file path; empty = a system monospace font
    std::string style = "Regular";  // "Italic", "Medium", "Bold Italic", ...
    float px = 26;                  // em size in pixels
    float line_height = 1.0f;       // multiplies the font's own line spacing
    bool ligatures = false;
    std::vector<std::string> features;  // OpenType features, e.g. "ss01", "zero"
    u32 styles = 0;                 // bit s set: prepare style s right away (see Font::kItalic)
    // The cell a 26-pixel font has, whatever the font's own (other sizes in
    // proportion): the tests', so that a machine's fonts do not move their
    // mouse positions and rows (BLIT_CELL, Debug builds). 0: the font's.
    int cell_w26 = 0, cell_h26 = 0;
};

class Font {
public:
    static constexpr u32 kTofu = 1;  // slot of the "missing glyph" box

    // A style number is kItalic, kBold, kUnderline and kStrike or-ed
    // together; 0 is regular. The first four (kFaces) are faces; a line
    // under or through is drawn over the face's glyph, in slots of its own,
    // so a cell still shows one slot and the shader does not know.
    static constexpr u32 kItalic = 1, kBold = 2, kUnderline = 4, kStrike = 8, kStyles = 16, kFaces = 4;
    static constexpr u32 kStyleShift = 7;  // where the style sits in an ASCII slot number
    static constexpr u32 kStyleMask = (kStyles - 1) << kStyleShift;
    static constexpr u32 kLineBits = (kUnderline | kStrike) << kStyleShift;  // (a face's ASCII block has no room for these: looked up)

    Font() = default;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    ~Font();

    // Problems that are not fatal (font not found so a default was used, no
    // HarfBuzz) are appended to *warnings.
    bool init(const FontSpec& spec, std::string* err, std::string* warnings = nullptr);

    int cell_w() const { return cell_w_; }
    int cell_h() const { return cell_h_; }
    int atlas_w() const { return atlas_w_; }
    int atlas_h() const { return atlas_h_; }
    u32 atlas_cols() const { return cols_; }
    u32 atlas_shift() const { return shift_; }  // log2(atlas_cols)
    const u8* pixels() const { return pixels_; }
    const std::string& path() const { return path_; }
    const std::string& family() const { return family_; }
    float px() const { return px_; }

    // Glyph for a code point: slot number in the low 13 bits, and bit 15 set
    // when the character is two cells wide (its right half is slot + 1).
    static constexpr u32 kWide = 0x8000;
    u32 glyph(u32 cp) { return cell_glyph(cp) & ~(kJoin | kPict); }
    // The same, with bit 14 set when the character may make one with the
    // one before it: it has no width of its own (a combining mark, a
    // joiner: see cp_width()), or, with bit 15, it is a regional indicator,
    // two of which are a flag; and bit 13 when it is an emoji (one that
    // joins the one before after a joiner). Its slot shows it on its own; in
    // the text a mark goes over the cell before it, by mark().
    static constexpr u32 kJoin = 0x4000, kPict = 0x2000;
    u32 cell_glyph(u32 cp) {
        if (cp < 128) return cp;
        if (cp < 0x10000 && bmp_[cp]) return bmp_[cp];
        return lookup(cp);
    }
    // The slot of a cell showing `base` (a slot) with the character of no
    // width `cp` drawn over it: a combining mark in its place, lifted clear
    // of a capital; a joiner or a selector leaves it as it is. Over a
    // character two cells wide, `half` is 1 for its left cell and 2 for its
    // right one, and the mark is placed over the whole of it. Each pair is
    // drawn once and then served from a table.
    u32 mark(u32 base, u32 cp, u32 half = 0);
    // The same in a style that ensure_style() said yes to. A character the
    // style's face lacks comes back in regular.
    u32 glyph(u32 cp, u32 style) {
        if (style == 0) return glyph(cp);
        if (cp < 128 && style < kFaces) return cp | (style << kStyleShift);
        return lookup_styled(cp, style);
    }

    // Makes a style usable: finds or makes its face and draws its ASCII
    // characters, once. False if this font cannot do styles (the atlas has
    // no room for them at enormous sizes); then everything stays regular.
    bool ensure_style(u32 style);
    bool has_style(u32 style) const { return style < kStyles && styles_[style].ready; }

    // True if the font itself (not a fallback) has this character. Used to
    // see whether it carries the icon glyphs of a "Nerd Font".
    bool has(u32 cp) const;
    // True if it can be drawn at all: by the font, or one of the fallbacks.
    bool can_draw(u32 cp);

    // Icons blit draws itself, for fonts that bring none. Each is two cells
    // wide: the slot of its left half comes back, the right half is the
    // next slot.
    enum Icon : u32 { kIconFolder, kIconFolderOpen, kIconFile, kIconText, kIconCode, kIconData, kIconImage, kIconArchive, kIconBinary, kIconCount };
    u32 icon(u32 id);
    // Where a style's glyphs come from, for diagnostics.
    std::string style_source(u32 style) const;

    // True when some ASCII sequences draw differently than their characters
    // one by one; then every row of cells should go through ligate().
    bool ligatures() const { return ligatures_; }
    // The ASCII characters that can take part in one, for diagnostics.
    std::string joining_characters() const {
        std::string s;
        for (int c = 33; c < 127; ++c)
            if (joins_[c]) s += static_cast<char>(c);
        return s;
    }

    // Rewrites cells[0, n) in place. On entry an ASCII character is its own
    // slot (in its style's block); runs of characters that can join are
    // shaped (through a cache) and replaced by the slots of the result.
    void ligate(u32* cells, u32 n) {
        // A run needs two neighbours, so one of them sits at an odd index:
        // looking at every other cell finds every run at half the work. A
        // ligature is one glyph, so a run also ends where the style changes.
        u32 done = 0;
        for (u32 i = 1; i < n; i += 2) {
            const u32 c = cells[i];
            if (!joins(c)) continue;
            auto same = [&](u32 other) { return joins(other) && ((other ^ c) & kStyleMask) == 0; };
            u32 s = i, e = i + 1;
            while (s > done && same(cells[s - 1])) --s;
            while (e < n && same(cells[e])) ++e;
            if (e - s >= 2) ligate_run(cells + s, e - s);
            done = e;
            i = (e | 1) - 2;  // resume at the first odd index at or after e
        }
    }

    // Bumps whenever previously returned slots became invalid.
    u32 generation() const { return generation_; }
    // A frame's last try (render.cpp): the atlas starts over first, and may
    // not start over again while held. A glyph that does not fit then is a
    // box, never a slot another glyph had earlier in the frame. full() says
    // whether any did not fit.
    void hold_atlas(bool on);
    bool full() const { return full_; }

    // Rows of atlas pixels changed since the last call: [y0, y1).
    bool take_dirty(int* y0, int* y1);

private:
    static constexpr u32 kMaxRun = 256;
    static constexpr u32 kMaxShortRuns = 1u << 16;  // entries the short-run cache may grow to

    static constexpr u32 kAsciiSlots = kFaces * 128;  // (the faces' ASCII blocks; a line under or through is looked up)

    // One of the four styles: the face its glyphs come from and how.
    struct Style {
        FT_Face face = nullptr;
        bool own = false;                      // the face is closed with this style
        bool slant = false, embolden = false;  // outlines are transformed after loading
        bool tried = false, ready = false;
        std::string path;                      // the font file, and the face in it
        int index = 0;
        std::string how;                       // "weight 700", "slanted", ...: what was done to the face
        Shaper shaper;                         // shaping for this face, if it has its own
        Shaper* sh = nullptr;                  // the shaper to use (maybe another style's); null = no shaping
        u32 base_gid[128] = {};                // glyph of each ASCII character on its own
    };

    // A cell's low 16 bits are its slot; higher bits (colour) ride along.
    bool joins(u32 cell) const { return (cell & 0xFFFF) < kAsciiSlots && joins_[cell & (kAsciiSlots - 1)]; }
    u32 lookup(u32 cp);
    u32 lookup_styled(u32 cp, u32 style);
    u32 lookup_lined(u32 cp, u32 style);   // (a style with a line under or through)
    void draw_lines(u32 slot, u32 width, u32 lines);
    u32 compose_mark(u32 base, u32 cp, u32 half);
    u32 alloc_slots(u32 width);
    void reset_atlas();
    bool render(const Style& st, u32 gid);
    void put_glyph(FT_Face f, u32 slot, u32 width, bool centred);
    void blit_glyph(FT_Face f, u32 gid, u32 slot, u32 width, bool centred);
    void copy_slot(u32 from, u32 to);
    void clear_slot(u32 slot);
    void draw_tofu(u32 slot, u32 width);
    void draw_icon(u32 id, u32 slot);
    FT_Face face_for(u32 cp, u32* glyph_index);
#if BLIT_FONTCONFIG
    FT_Face fallback_for(u32 cp);  // a font fontconfig knows that has it (font.cpp)
#endif
    FT_Face open_face(const std::string& path, int index);
    void open_style(u32 style);
    void setup_ascii(u32 style);
    void mark_dirty(u32 slot);
    void ligate_run(u32* cells, u32 n);
    void shape_run(u32 style, const char* text, u32 n, u16* out);
    u32 slice_slot(u32 style, u32 gid, int k, int left, int top);

    struct Ent {
        u32 key;
        u32 slot;
    };
    struct MarkEnt {
        u64 key;  // the base slot, the half, the mark
        u32 slot;
    };
    struct ShortRun {
        u64 key = 0;  // the characters, packed; 0 = empty
        u16 slot[8];
    };
    struct StrHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };

    FT_Library lib_ = nullptr;
    FT_Face face_ = nullptr;
    std::vector<FT_Face> fallbacks_;
    bool fallbacks_loaded_ = false;
#if BLIT_FONTCONFIG
    std::unordered_set<u32> asked_;  // characters fontconfig has been asked for a font for
#endif
    std::string path_, family_;
    float px_ = 0;
    int load_flags_ = 0;

    int cell_w_ = 0, cell_h_ = 0, baseline_ = 0;
    int x_height_ = 0;  // how far above the baseline a lower-case letter reaches, in pixels
    int underline_y_ = 0, strike_y_ = 0, line_px_ = 1;  // where a line under and through the text goes (from the cell's top), and how thick
    int atlas_w_ = 0, atlas_h_ = 0;
    u32 cols_ = 0, shift_ = 0, slots_ = 0;
    u32 first_slot_ = 128, next_slot_ = 128;  // slots below first_slot_ are the ASCII blocks
    u32 tofu_wide_ = 0;  // a box two cells wide, for a wide character that did not fit while held
    bool hold_ = false, full_ = false;
    u8* pixels_ = nullptr;
    int dirty_y0_ = 0, dirty_y1_ = 0;
    u32 generation_ = 0;

    // Basic Multilingual Plane: direct table, one load per character.
    // Everything above it goes through the hash map.
    u16* bmp_ = nullptr;
    std::vector<Ent> map_;
    u32 map_used_ = 0;
    std::vector<MarkEnt> marks_;  // (base, mark) -> slot, see mark()
    u32 marks_used_ = 0;

    Style styles_[kStyles];
    std::vector<std::string> features_;
    bool want_ligatures_ = false;

    // Shaping state.
    bool ligatures_ = false;
    bool joins_[kAsciiSlots] = {};  // by ASCII slot: can it take part in a ligature
    std::vector<MarkEnt> slices_;   // (style, glyph, cell offset, where the shaper put it) -> slot
    u32 slices_used_ = 0;
    std::vector<ShortRun> short_runs_;  // runs of up to 8 characters
    u32 short_used_ = 0;
    std::unordered_map<std::string, std::vector<u16>, StrHash, std::equal_to<>> long_runs_;
};

// Finds a font file by family and style name in the system font folders.
// Returns false if nothing matches.
bool find_font(const std::string& family, const std::string& style, std::string* path, int* face_index);

}  // namespace blit
