#include "font.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SYNTHESIS_H

#include "utf8.hpp"

namespace blit {

namespace {

const char* const kDefaultFonts[] = {
#if defined(__APPLE__)
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
#elif defined(_WIN32)
    "C:\\Windows\\Fonts\\consola.ttf",
    "C:\\Windows\\Fonts\\cour.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
#endif
};

// Tried in order for characters the main font does not have.
const char* const kFallbackFonts[] = {
#if defined(__APPLE__)
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/Apple Symbols.ttf",
#elif defined(_WIN32)
    "C:\\Windows\\Fonts\\seguisym.ttf",
    "C:\\Windows\\Fonts\\msgothic.ttc",
    "C:\\Windows\\Fonts\\msyh.ttc",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
#endif
};

constexpr u32 kEmpty = 0xFFFFFFFFu;
constexpr u64 kEmptyMark = ~0ull;

u32 hash_u32(u32 x) { return (x * 2654435761u) >> 7; }
u32 hash_u64(u64 x) { return static_cast<u32>((x * 0x9E3779B97F4A7C15ull) >> 40); }

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// ---------------------------------------------------------------- finding fonts

// "Victor Mono", "VictorMono-Regular" and "victor_mono" all become "victormono".
std::string squash(std::string_view s) {
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    if (!h) h = std::getenv("USERPROFILE");
    return h ? h : "";
}

std::vector<std::string> font_dirs() {
    std::vector<std::string> d;
    const std::string home = home_dir();
#if defined(__APPLE__)
    if (!home.empty()) d.push_back(home + "/Library/Fonts");
    d.push_back("/Library/Fonts");
    d.push_back("/System/Library/Fonts");
#elif defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA")) d.push_back(std::string(local) + "\\Microsoft\\Windows\\Fonts");
    if (const char* win = std::getenv("WINDIR")) d.push_back(std::string(win) + "\\Fonts");
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) d.push_back(std::string(xdg) + "/fonts");
    if (!home.empty()) {
        d.push_back(home + "/.local/share/fonts");
        d.push_back(home + "/.fonts");
    }
    d.push_back("/usr/local/share/fonts");
    d.push_back("/usr/share/fonts");
#endif
    return d;
}

bool looks_like_path(const std::string& s) {
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return true;
    const size_t dot = s.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = squash(s.substr(dot));
    return ext == "ttf" || ext == "otf" || ext == "ttc" || ext == "otc";
}

std::string expand_home(const std::string& s) {
    if (s.starts_with("~/")) return home_dir() + s.substr(1);
    return s;
}

// Calls visit(path, file name) for every file under dir, subfolders included.
// (Plain OS calls: std::filesystem would pull the whole locale library into
// the program and slow every launch.)
template <class Visit>
void walk_files(const std::string& dir, Visit&& visit, int depth = 0) {
    if (depth > 6) return;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk_files(full, visit, depth + 1);
        else visit(full, name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            struct stat st;
            is_dir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) walk_files(full, visit, depth + 1);
        else visit(full, name);
    }
    closedir(d);
#endif
}

bool find_font_in(FT_Library lib, const std::string& family, const std::string& style, std::string* path, int* index) {
    const std::string want = squash(family), want_style = squash(style.empty() ? "Regular" : style);
    if (want.empty()) return false;

    // Font files whose name mentions the family are opened first; that is
    // nearly always enough and avoids opening every font on the system.
    struct File {
        std::string path;
        int rank;
    };
    std::vector<File> named, others;
    for (const std::string& dir : font_dirs()) {
        walk_files(dir, [&](const std::string& full, const std::string& file) {
            const size_t dot = file.rfind('.');
            if (dot == std::string::npos) return;
            const std::string ext = squash(file.substr(dot));
            if (ext != "ttf" && ext != "otf" && ext != "ttc" && ext != "otc") return;
            const std::string name = squash(file.substr(0, dot));
            if (name.find(want) != std::string::npos)
                named.push_back({full, name.find(want_style) != std::string::npos ? 0 : 1});
            else
                others.push_back({full, 2});
        });
    }
    std::stable_sort(named.begin(), named.end(), [](const File& a, const File& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.path.size() < b.path.size();
    });

    int best = 0;
    auto consider = [&](const std::vector<File>& files) {
        for (const File& file : files) {
            FT_Face f;
            if (FT_New_Face(lib, file.path.c_str(), 0, &f)) continue;
            const int faces = static_cast<int>(f->num_faces);
            for (int i = 0; i < faces; ++i) {
                if (i > 0) {
                    FT_Done_Face(f);
                    if (FT_New_Face(lib, file.path.c_str(), i, &f)) {
                        f = nullptr;
                        break;
                    }
                }
                const std::string fam = squash(f->family_name ? f->family_name : "");
                const std::string sty = squash(f->style_name ? f->style_name : "");
                int score = 0;
                if (fam == want) score = 100;
                else if (fam.starts_with(want)) score = 70 - std::min<int>(20, static_cast<int>(fam.size() - want.size()));
                else if (fam.find(want) != std::string::npos) score = 40;
                if (score == 0) continue;
                if (sty == want_style) score += 30;
                else if (want_style == "regular" && (sty == "book" || sty == "roman" || sty == "normal")) score += 25;
                if (FT_IS_FIXED_WIDTH(f)) score += 10;
                if (score > best) {
                    best = score;
                    *path = file.path;
                    *index = i;
                }
            }
            if (f) FT_Done_Face(f);
            if (best >= 140) return;  // exact family, exact style, fixed width
        }
    };
    consider(named);
    if (best == 0) consider(others);  // the file name gave no hint: look inside everything
    return best > 0;
}

// Looks for another face of the same family: `family` exactly, and a style
// name out of `styles` (all squashed; the most wanted style first). A
// family's files sit together and their names start alike, so only files in
// the main face's folder whose names start like its own are opened.
bool find_sibling(FT_Library lib, const std::string& main_path, const std::string& family,
                  const std::string& main_style, const std::vector<std::string>& styles, std::string* path, int* index) {
    const size_t slash = main_path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "." : main_path.substr(0, slash);
    auto stem_of = [](const std::string& file) {
        const size_t dot = file.rfind('.');
        return squash(dot == std::string::npos ? file : file.substr(0, dot));
    };
    // "VictorMono-Regular.ttf" -> "victormono"
    std::string prefix = stem_of(main_path.substr(slash == std::string::npos ? 0 : slash + 1));
    for (const std::string& tail : {main_style, std::string("regular")}) {
        if (tail.empty() || prefix.size() <= tail.size() || !prefix.ends_with(tail)) continue;
        prefix.resize(prefix.size() - tail.size());
        break;
    }

    struct File {
        std::string path;
        int rank;  // 0: the name is exactly what a face of that style would be called
    };
    std::vector<File> files;
    walk_files(
        dir,
        [&](const std::string& full, const std::string& file) {
            const size_t dot = file.rfind('.');
            if (dot == std::string::npos) return;
            const std::string ext = squash(file.substr(dot));
            if (ext != "ttf" && ext != "otf" && ext != "ttc" && ext != "otc") return;
            const std::string name = stem_of(file);
            if (!name.starts_with(prefix)) return;
            const std::string rest = name.substr(prefix.size());
            files.push_back({full, std::find(styles.begin(), styles.end(), rest) != styles.end() ? 0 : 1});
        },
        6);  // this folder only: one level deeper is past the limit
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.path.size() < b.path.size();
    });

    size_t best = styles.size();
    for (const File& file : files) {
        FT_Face f;
        if (FT_New_Face(lib, file.path.c_str(), 0, &f)) continue;
        const int faces = static_cast<int>(f->num_faces);
        for (int i = 0; i < faces; ++i) {
            if (i > 0) {
                FT_Done_Face(f);
                if (FT_New_Face(lib, file.path.c_str(), i, &f)) {
                    f = nullptr;
                    break;
                }
            }
            if (squash(f->family_name ? f->family_name : "") != family) continue;
            const size_t at = static_cast<size_t>(
                std::find(styles.begin(), styles.end(), squash(f->style_name ? f->style_name : "")) - styles.begin());
            if (at < best) {
                best = at;
                *path = file.path;
                *index = i;
            }
        }
        if (f) FT_Done_Face(f);
        if (best == 0) break;
    }
    return best < styles.size();
}

// Moves a variable font to a bold weight. False if it has no weight axis, or
// no room on it to get noticeably bolder.
bool set_bold_weight(FT_Library lib, FT_Face f) {
    if (!FT_HAS_MULTIPLE_MASTERS(f)) return false;
    FT_MM_Var* mm = nullptr;
    if (FT_Get_MM_Var(f, &mm)) return false;
    bool ok = false;
    std::vector<FT_Fixed> coords(mm->num_axis);
    if (!FT_Get_Var_Design_Coordinates(f, mm->num_axis, coords.data())) {
        for (FT_UInt a = 0; a < mm->num_axis; ++a) {
            if (mm->axis[a].tag != FT_MAKE_TAG('w', 'g', 'h', 't')) continue;
            const double now = static_cast<double>(coords[a]) / 65536.0, top = static_cast<double>(mm->axis[a].maximum) / 65536.0;
            if (top < now + 150) break;
            coords[a] = static_cast<FT_Fixed>(std::min(top, std::max(700.0, now + 200)) * 65536.0);
            ok = !FT_Set_Var_Design_Coordinates(f, mm->num_axis, coords.data());
            break;
        }
    }
    FT_Done_MM_Var(lib, mm);
    return ok;
}

}  // namespace

bool find_font(const std::string& family, const std::string& style, std::string* path, int* face_index) {
    FT_Library lib;
    if (FT_Init_FreeType(&lib)) return false;
    const bool ok = find_font_in(lib, family, style, path, face_index);
    FT_Done_FreeType(lib);
    return ok;
}

// ---------------------------------------------------------------- setup

Font::~Font() {
    for (Style& st : styles_)
        if (st.own && st.face) FT_Done_Face(st.face);
    for (FT_Face f : fallbacks_) FT_Done_Face(f);
    if (face_) FT_Done_Face(face_);
    if (lib_) FT_Done_FreeType(lib_);
    std::free(pixels_);
    std::free(bmp_);
}

bool Font::init(const FontSpec& spec, std::string* err, std::string* warnings) {
    auto warn = [&](const std::string& w) {
        if (warnings && !w.empty()) *warnings += w + "\n";
    };
    if (FT_Init_FreeType(&lib_)) {
        if (err) *err = "FreeType failed to start";
        return false;
    }
    int index = 0;
    if (!spec.family.empty()) {
        std::string path;
        if (looks_like_path(spec.family)) path = expand_home(spec.family);
        else if (!find_font_in(lib_, spec.family, spec.style, &path, &index)) warn("font not found: " + spec.family);
        if (!path.empty()) {
            if (FT_New_Face(lib_, path.c_str(), index, &face_)) {
                face_ = nullptr;
                warn("cannot load font file: " + path);
            } else {
                path_ = path;
            }
        }
    }
    if (!face_) {
        index = 0;
        for (const char* p : kDefaultFonts) {
            if (!FT_New_Face(lib_, p, 0, &face_)) {
                path_ = p;
                break;
            }
            face_ = nullptr;
        }
    }
    if (!face_) {
        if (err) *err = "no usable font found; set [font] family in the config, or pass --font";
        return false;
    }
    family_ = face_->family_name ? face_->family_name : "";
    px_ = spec.px;
    if (FT_Set_Char_Size(face_, 0, static_cast<FT_F26Dot6>(std::lround(px_ * 64)), 72, 72)) {
        if (err) *err = "font has no scalable outlines: " + path_;
        return false;
    }
    // Small sizes get light (vertical only) hinting; big ones need none.
    load_flags_ = px_ >= 22 ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_LIGHT;

    const FT_Size_Metrics& m = face_->size->metrics;
    const double asc = m.ascender / 64.0, desc = -m.descender / 64.0, height = m.height / 64.0;
    const double spacing = std::clamp(static_cast<double>(spec.line_height), 0.5, 4.0);
    cell_h_ = std::max(1, static_cast<int>(std::lround(height * spacing)));
    baseline_ = static_cast<int>(std::lround((cell_h_ - (asc + desc)) / 2 + asc));
    cell_w_ = 0;
    if (!FT_Load_Char(face_, 'M', load_flags_)) cell_w_ = static_cast<int>((face_->glyph->advance.x + 32) >> 6);
    if (cell_w_ <= 0) cell_w_ = std::max(1, static_cast<int>(std::lround(px_ * 0.6)));
    x_height_ = baseline_ / 2;
    if (!FT_Load_Char(face_, 'x', load_flags_)) x_height_ = static_cast<int>((face_->glyph->metrics.horiBearingY + 32) >> 6);

    // Atlas: up to 4096 slots. The number of slots per row is a power of two
    // so the shader can split a slot number with a shift and a mask.
    shift_ = 6;
    while (shift_ > 4 && (cell_w_ << shift_) > 4096) --shift_;
    cols_ = 1u << shift_;
    const u32 rows = std::min<u32>(4096u >> shift_, static_cast<u32>(8192 / cell_h_));
    atlas_w_ = static_cast<int>(cols_) * cell_w_;
    atlas_h_ = static_cast<int>(rows) * cell_h_;
    slots_ = std::min(cols_ * rows, kPict - 1);
    // The styles' ASCII blocks are set aside when the atlas is big enough to
    // spare them, which it is at any reasonable size.
    first_slot_ = next_slot_ = slots_ >= 2 * kAsciiSlots ? kAsciiSlots : 128;
    pixels_ = static_cast<u8*>(std::calloc(static_cast<size_t>(atlas_w_) * atlas_h_, 1));
    if (!pixels_ || atlas_w_ > 8192 || slots_ < 256) {
        if (err) *err = "font size out of range";
        return false;
    }
    // Glyph and slice tables hold at most one entry per atlas slot, and are
    // sized so that they never fill up before the atlas does.
    map_.assign(8192, {kEmpty, 0});
    slices_.assign(8192, {kEmpty, 0});
    marks_.assign(8192, {kEmptyMark, 0});
    short_runs_.assign(1024, ShortRun{});  // grows, see ligate_run()
    bmp_ = static_cast<u16*>(std::calloc(0x10000, sizeof(u16)));
    if (!bmp_) return false;

    // Shaping is only set up when something asks for it.
    features_ = spec.features;
    want_ligatures_ = spec.ligatures;
    Style& regular = styles_[0];
    regular.face = face_;
    regular.path = path_;
    regular.index = index;
    regular.tried = regular.ready = true;
    if (spec.ligatures || !spec.features.empty()) {
        std::string serr;
        if (regular.shaper.open(path_, static_cast<unsigned>(index), px_, spec.features, spec.ligatures, &serr))
            regular.sh = &regular.shaper;
        warn(serr);
    }

    dirty_y0_ = atlas_h_;
    dirty_y1_ = 0;
    draw_tofu(kTofu, 1);
    setup_ascii(0);
    ligatures_ = std::any_of(joins_, joins_ + 128, [](bool b) { return b; });
    for (u32 s = 1; s < kStyles; ++s)
        if (spec.styles >> s & 1) ensure_style(s);
    return true;
}

// ---------------------------------------------------------------- styles

FT_Face Font::open_face(const std::string& path, int index) {
    FT_Face f;
    if (FT_New_Face(lib_, path.c_str(), index, &f)) return nullptr;
    if (FT_Set_Char_Size(f, 0, static_cast<FT_F26Dot6>(std::lround(px_ * 64)), 72, 72)) {
        FT_Done_Face(f);
        return nullptr;
    }
    return f;
}

// Decides where a style's glyphs come from. In order of preference: a face
// of the family made for it ("Victor Mono Italic"); for bold, the weight axis
// of a variable font; outlines slanted or thickened here.
void Font::open_style(u32 s) {
    Style& st = styles_[s];
    // The regular face's own style, as weight and slant: "Light Italic" is
    // "light", slanted. Italic of that is itself; bold of it is "Bold Italic".
    const std::string main_style = squash(face_->style_name ? face_->style_name : "");
    std::string base = main_style;
    bool base_italic = (face_->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    for (const std::string_view word : {"italic", "oblique"}) {
        if (const size_t at = base.find(word); at != std::string::npos) {
            base.erase(at, word.size());
            base_italic = true;
        }
    }
    if (base.empty() || base == "book" || base == "roman" || base == "normal") base = "regular";
    const bool base_bold = base.find("bold") != std::string::npos || base == "black" || base == "heavy";
    const bool italic = base_italic || (s & kItalic), bold = (s & kBold) && !base_bold;
    if (italic == base_italic && !bold) {  // the regular face already is that
        st.face = face_;
        st.path = path_;
        st.index = styles_[0].index;
        return;
    }

    const std::string weight = bold ? "bold" : base;
    std::vector<std::string> names;
    if (!italic) names = {weight};
    else if (weight == "regular") names = {"italic", "oblique", "regularitalic", "regularoblique"};
    else names = {weight + "italic", weight + "oblique"};
    if (find_sibling(lib_, path_, squash(family_), main_style, names, &st.path, &st.index)) {
        if ((st.face = open_face(st.path, st.index))) {
            st.own = true;
            return;
        }
    }

    if (!bold) {  // italic with no face for it
        st.face = face_;
        st.path = path_;
        st.index = styles_[0].index;
        st.slant = true;
        st.how = "slanted";
        return;
    }
    // Bold with no face for it: start from the same slant in normal weight.
    if (italic != base_italic) ensure_style(kItalic);
    const Style& from = styles_[italic != base_italic && styles_[kItalic].ready ? kItalic : 0];
    st.path = from.path;
    st.index = from.index;
    st.slant = from.slant;
    st.how = from.how;
    if (FT_Face f = open_face(from.path, from.index)) {
        if (set_bold_weight(lib_, f)) {
            st.face = f;
            st.own = true;
            st.how += st.how.empty() ? "bold weight" : ", bold weight";
            return;
        }
        FT_Done_Face(f);
    }
    st.face = from.face;
    st.embolden = true;
    st.how += st.how.empty() ? "thickened" : ", thickened";
}

// Works out the glyph of each ASCII character in a style, which of them can
// join into ligatures, and draws them into the style's block of slots.
void Font::setup_ascii(u32 s) {
    Style& st = styles_[s];
    for (u32 c = 33; c < 127; ++c) {
        st.base_gid[c] = FT_Get_Char_Index(st.face, c);
        if (st.sh) {
            // Alone, a character still goes through features such as
            // stylistic sets, so its glyph may differ from the plain one.
            const char ch = static_cast<char>(c);
            auto g = st.sh->shape(&ch, 1);
            if (g.size() == 1) st.base_gid[c] = g[0].gid;
        }
    }
    if (st.sh && want_ligatures_) {
        bool* joins = joins_ + s * 128;
        st.sh->contextual_ascii(joins);
        joins[static_cast<unsigned char>(' ')] = false;
    }
    for (u32 c = 33; c < 127; ++c) {
        const u32 slot = c | (s << kStyleShift), gid = st.base_gid[c];
        if (gid && render(st, gid)) {
            put_glyph(st.face, slot, 1, false);
        } else if (s != 0) {
            copy_slot(c, slot);  // not in this style's face: as in regular
        } else {
            u32 gi = 0;
            FT_Face f = gid ? nullptr : face_for(c, &gi);
            if (f) blit_glyph(f, gi, slot, 1, true);
            else draw_tofu(slot, 1);
        }
    }
}

bool Font::ensure_style(u32 s) {
    if (s == 0) return true;
    if (s >= kStyles || first_slot_ < kAsciiSlots) return false;
    Style& st = styles_[s];
    if (st.tried) return st.ready;
    st.tried = true;
    open_style(s);
    if (!st.face) return false;
    if (styles_[0].sh) {
        // Shaping is on. Two styles drawn from one font file share a shaper.
        for (Style& other : styles_)
            if (&other != &st && other.ready && other.sh == &other.shaper && other.path == st.path && other.index == st.index)
                st.sh = other.sh;
        std::string ignored;
        if (!st.sh && st.shaper.open(st.path, static_cast<unsigned>(st.index), px_, features_, want_ligatures_, &ignored))
            st.sh = &st.shaper;
    }
    setup_ascii(s);
    st.ready = true;
    return true;
}

std::string Font::style_source(u32 s) const {
    if (s >= kStyles || !styles_[s].ready) return "";
    const Style& st = styles_[s];
    return st.how.empty() ? st.path : st.path + "  (" + st.how + ")";
}

bool Font::take_dirty(int* y0, int* y1) {
    if (dirty_y1_ <= dirty_y0_) return false;
    *y0 = dirty_y0_;
    *y1 = dirty_y1_;
    dirty_y0_ = atlas_h_;
    dirty_y1_ = 0;
    return true;
}

void Font::mark_dirty(u32 slot) {
    int y = static_cast<int>(slot / cols_) * cell_h_;
    dirty_y0_ = std::min(dirty_y0_, y);
    dirty_y1_ = std::max(dirty_y1_, y + cell_h_);
}

// ---------------------------------------------------------------- slots

// Atlas full: start over. Callers notice through generation().
void Font::reset_atlas() {
    std::fill(map_.begin(), map_.end(), Ent{kEmpty, 0});
    std::fill(slices_.begin(), slices_.end(), Ent{kEmpty, 0});
    std::fill(marks_.begin(), marks_.end(), MarkEnt{kEmptyMark, 0});
    std::fill(short_runs_.begin(), short_runs_.end(), ShortRun{});
    long_runs_.clear();
    std::memset(bmp_, 0, 0x10000 * sizeof(u16));
    map_used_ = slices_used_ = short_used_ = marks_used_ = 0;
    next_slot_ = first_slot_;
    ++generation_;
}

u32 Font::alloc_slots(u32 width) {
    // A wide glyph needs two slots side by side in one atlas row.
    auto align = [&] {
        if (width == 2 && next_slot_ % cols_ == cols_ - 1) ++next_slot_;
    };
    align();
    if (next_slot_ + width > slots_) {
        reset_atlas();
        align();
    }
    const u32 slot = next_slot_;
    next_slot_ += width;
    return slot;
}

u32 Font::lookup(u32 cp) {
    const u32 width = cp_width(cp);
    const u32 mask = static_cast<u32>(map_.size()) - 1;
    if (cp >= 0x10000) {
        for (u32 h = hash_u32(cp) & mask; map_[h].key != kEmpty; h = (h + 1) & mask)
            if (map_[h].key == cp) return map_[h].slot;
        if (map_used_ * 2 >= map_.size()) reset_atlas();
    }
    // A character of no width still gets a cell's slot, for where it is
    // shown on its own; in the text it is drawn over the cell before it.
    const u32 slot = alloc_slots(std::max(width, 1u));
    const u32 packed = slot | (width == 2 ? kWide : 0) | (width == 0 || cp_regional(cp) ? kJoin : 0) | (cp_pictographic(cp) ? kPict : 0);
    if (cp < 0x10000) {
        bmp_[cp] = static_cast<u16>(packed);
    } else {
        u32 h = hash_u32(cp) & mask;
        while (map_[h].key != kEmpty) h = (h + 1) & mask;
        map_[h] = {cp, packed};
        ++map_used_;
    }
    u32 gi;
    FT_Face f = face_for(cp, &gi);
    if (f) blit_glyph(f, gi, slot, std::max(width, 1u), f != face_ || width == 0);
    else if (width) draw_tofu(slot, width);
    else clear_slot(slot);  // (a joiner, a selector: nothing to see)
    return packed;
}

u32 Font::lookup_styled(u32 cp, u32 s) {
    if (s >= kStyles || !styles_[s].ready) return glyph(cp);
    Style& st = styles_[s];
    const u32 key = cp | (s << 21);  // code points end below 1 << 21
    u32 mask = static_cast<u32>(map_.size()) - 1;
    for (u32 h = hash_u32(key) & mask; map_[h].key != kEmpty; h = (h + 1) & mask)
        if (map_[h].key == key) return map_[h].slot;
    if ((map_used_ + 1) * 2 >= map_.size()) reset_atlas();

    u32 packed;
    if (const u32 gid = FT_Get_Char_Index(st.face, cp)) {
        const u32 width = std::max(cp_width(cp), 1u), slot = alloc_slots(width);
        packed = slot | (width == 2 ? kWide : 0);
        if (render(st, gid)) put_glyph(st.face, slot, width, false);
        else draw_tofu(slot, width);
    } else {
        packed = glyph(cp);  // the style's face lacks it: the regular glyph
    }
    // (Either branch may have restarted the atlas; the entry goes in after.)
    u32 h = hash_u32(key) & mask;
    while (map_[h].key != kEmpty) h = (h + 1) & mask;
    map_[h] = {key, packed};
    ++map_used_;
    return packed;
}

u32 Font::mark(u32 base, u32 cp, u32 half) {
    base &= 0xFFFF;
    const u64 key = base | static_cast<u64>(half) << 16 | static_cast<u64>(cp) << 18;
    const u32 mask = static_cast<u32>(marks_.size()) - 1;
    for (u32 h = hash_u64(key) & mask; marks_[h].key != kEmptyMark; h = (h + 1) & mask)
        if (marks_[h].key == key) return marks_[h].slot;
    if (marks_used_ * 2 >= marks_.size()) {
        reset_atlas();
        return base;  // (the slot asked about is gone with the rest; the frame is made again)
    }
    const u32 gen = generation_;
    const u32 slot = compose_mark(base, cp, half);
    if (generation_ != gen) return slot;  // the atlas restarted while it was drawn: not kept
    u32 h = hash_u64(key) & mask;
    while (marks_[h].key != kEmptyMark) h = (h + 1) & mask;
    marks_[h] = {key, slot};
    ++marks_used_;
    return slot;
}

// Draws a mark over a copy of the cell it sits on. Most monospace fonts
// draw a mark in a cell of its own, where it would sit over a letter in
// that cell (a horn to the right, an acute a little to the right of the
// middle); that is kept. One placed some other way (with no advance,
// hanging back over the cell before, or from another font) is put in the
// middle. A mark above the letters that would run into its letter (an
// accent on a capital, a second accent over the first) goes up until it
// clears it, as far as the cell allows.
u32 Font::compose_mark(u32 base, u32 cp, u32 half) {
    if (!cp_extends(cp) || cp == 0x200C || cp == 0x200D || cp - 0xFE00 < 16 || cp - 0x1F3FB < 5 || cp - 0xE0000 < 0x1000) return base;  // (nothing to see)
    u32 gi = 0;
    FT_Face f = face_for(cp, &gi);
    if (!f || FT_Load_Glyph(f, gi, load_flags_ | FT_LOAD_RENDER) || f->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) return base;
    const FT_Bitmap& bm = f->glyph->bitmap;
    const int w = static_cast<int>(bm.width), rows = static_cast<int>(bm.rows);
    if (w == 0 || rows == 0) return base;
    const int left = f->glyph->bitmap_left, top = f->glyph->bitmap_top;
    const int advance = static_cast<int>((f->glyph->advance.x + 32) >> 6);

    const u32 gen = generation_;
    const u32 slot = alloc_slots(1);
    if (generation_ != gen) {  // (the base went with the rest of the atlas)
        clear_slot(slot);
        return slot;
    }
    copy_slot(base, slot);
    const int sx = static_cast<int>(slot % cols_) * cell_w_, sy = static_cast<int>(slot / cols_) * cell_h_;

    // Where the mark goes across the character (two cells for a wide one),
    // and which part of that this cell is.
    const int span = half ? 2 * cell_w_ : cell_w_;
    const bool own_cell = f == face_ && advance > 0 && left >= 0 && left + w <= advance;
    const int x0 = own_cell ? left + (span - cell_w_) / 2 : (span - w) / 2;
    const int from = half == 2 ? cell_w_ : 0;
    int y0 = baseline_ - top;
    if (half == 0 && top - rows >= x_height_ - 1) {
        int ink = cell_h_;  // the first row of the letter's ink
        for (int y = 0; y < cell_h_ && ink == cell_h_; ++y) {
            const u8* row = pixels_ + static_cast<size_t>(sy + y) * atlas_w_ + sx;
            for (int x = 0; x < cell_w_; ++x)
                if (row[x] >= 64) {
                    ink = y;
                    break;
                }
        }
        const int gap = std::max(1, cell_h_ / 24);
        if (y0 + rows + gap > ink) y0 -= std::min(y0 + rows + gap - ink, std::max(0, y0));
    }
    for (int y = 0; y < rows; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= cell_h_) continue;
        const u8* src = bm.buffer + static_cast<ptrdiff_t>(y) * bm.pitch;
        u8* dst = pixels_ + static_cast<size_t>(sy + dy) * atlas_w_ + sx;
        for (int x = 0; x < w; ++x) {
            const int dx = x0 + x - from;
            if (dx >= 0 && dx < cell_w_) dst[dx] = std::max(dst[dx], src[x]);
        }
    }
    mark_dirty(slot);
    return slot;
}

bool Font::has(u32 cp) const { return FT_Get_Char_Index(face_, cp) != 0; }

bool Font::can_draw(u32 cp) {
    u32 gi = 0;
    return face_for(cp, &gi) != nullptr;
}

u32 Font::icon(u32 id) {
    const u32 key = 0x7F000000u | id;  // above every character and style
    u32 mask = static_cast<u32>(map_.size()) - 1;
    for (u32 h = hash_u32(key) & mask; map_[h].key != kEmpty; h = (h + 1) & mask)
        if (map_[h].key == key) return map_[h].slot;
    if ((map_used_ + 1) * 2 >= map_.size()) reset_atlas();
    const u32 slot = alloc_slots(2);
    draw_icon(id, slot);
    u32 h = hash_u32(key) & mask;
    while (map_[h].key != kEmpty) h = (h + 1) & mask;
    map_[h] = {key, slot};
    ++map_used_;
    return slot;
}

// Icons are solid shapes with pieces cut out, described as "is this point
// inside" over a unit square and sampled sixteen times per pixel.
void Font::draw_icon(u32 id, u32 slot) {
    const int w = 2 * cell_w_, h = cell_h_;
    const int sx = static_cast<int>(slot % cols_) * cell_w_, sy = static_cast<int>(slot / cols_) * cell_h_;
    for (int y = 0; y < h; ++y) std::memset(pixels_ + static_cast<size_t>(sy + y) * atlas_w_ + sx, 0, static_cast<size_t>(w));
    // A square about as tall as a capital letter and a half, sitting just
    // below the baseline.
    const int side = std::max(4, std::min(w - 2, static_cast<int>(std::lround(h * 0.60))));
    const int x0 = (w - side) / 2, y0 = std::clamp(baseline_ - static_cast<int>(std::lround(side * 0.86)), 0, h - side);

    auto rect = [](float u, float v, float a, float b, float c, float d) { return u >= a && u < c && v >= b && v < d; };
    auto disc = [](float u, float v, float cx, float cy, float r) { return (u - cx) * (u - cx) + (v - cy) * (v - cy) < r * r; };
    auto near = [](float u, float v, float ax, float ay, float bx, float by, float t) {  // within t of a line segment
        const float dx = bx - ax, dy = by - ay;
        const float k = std::clamp(((u - ax) * dx + (v - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
        const float px = ax + k * dx - u, py = ay + k * dy - v;
        return px * px + py * py < t * t;
    };
    auto page = [&](float u, float v) {  // a sheet with its corner folded
        return rect(u, v, 0.18f, 0.06f, 0.82f, 0.94f) && !(u > 0.58f && v < 0.30f && u - 0.58f > v - 0.06f);
    };
    auto inside = [&](float u, float v) {
        switch (id) {
            case kIconFolder: return rect(u, v, 0.06f, 0.18f, 0.44f, 0.34f) || rect(u, v, 0.06f, 0.30f, 0.94f, 0.84f);
            case kIconFolderOpen:
                return rect(u, v, 0.06f, 0.18f, 0.44f, 0.34f) || (rect(u, v, 0.06f, 0.30f, 0.94f, 0.84f) && !rect(u, v, 0.14f, 0.38f, 0.86f, 0.50f));
            case kIconText:
                return page(u, v) && !(rect(u, v, 0.30f, 0.40f, 0.70f, 0.47f) || rect(u, v, 0.30f, 0.55f, 0.70f, 0.62f) ||
                                       rect(u, v, 0.30f, 0.70f, 0.58f, 0.77f));
            case kIconCode:
                return page(u, v) && !(near(u, v, 0.44f, 0.46f, 0.32f, 0.60f, 0.036f) || near(u, v, 0.32f, 0.60f, 0.44f, 0.74f, 0.036f) ||
                                       near(u, v, 0.56f, 0.46f, 0.68f, 0.60f, 0.036f) || near(u, v, 0.68f, 0.60f, 0.56f, 0.74f, 0.036f));
            case kIconData:
                return page(u, v) && !(rect(u, v, 0.30f, 0.42f, 0.44f, 0.56f) || rect(u, v, 0.56f, 0.42f, 0.70f, 0.56f) ||
                                       rect(u, v, 0.30f, 0.66f, 0.44f, 0.80f) || rect(u, v, 0.56f, 0.66f, 0.70f, 0.80f));
            case kIconImage:
                return rect(u, v, 0.08f, 0.16f, 0.92f, 0.84f) && !disc(u, v, 0.30f, 0.36f, 0.09f) &&
                       !(v < 0.76f && v > 0.46f + std::fabs(u - 0.54f) * 0.95f);
            case kIconArchive:
                return page(u, v) && !(rect(u, v, 0.45f, 0.36f, 0.55f, 0.45f) || rect(u, v, 0.45f, 0.52f, 0.55f, 0.61f) ||
                                       rect(u, v, 0.45f, 0.68f, 0.55f, 0.77f));
            case kIconBinary: return disc(u, v, 0.5f, 0.5f, 0.42f) && !disc(u, v, 0.5f, 0.5f, 0.17f);
            default: return page(u, v);
        }
    };
    for (int y = 0; y < side; ++y) {
        u8* row = pixels_ + static_cast<size_t>(sy + y0 + y) * atlas_w_ + sx + x0;
        for (int x = 0; x < side; ++x) {
            int hits = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i)
                    hits += inside((static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / 4) / static_cast<float>(side),
                                   (static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / 4) / static_cast<float>(side));
            row[x] = static_cast<u8>(hits * 255 / 16);
        }
    }
    mark_dirty(slot);
    mark_dirty(slot + 1);
}

FT_Face Font::face_for(u32 cp, u32* gi) {
    *gi = FT_Get_Char_Index(face_, cp);
    if (*gi) return face_;
    if (!fallbacks_loaded_) {
        fallbacks_loaded_ = true;
        for (const char* p : kFallbackFonts) {
            FT_Face f;
            if (FT_New_Face(lib_, p, 0, &f)) continue;
            if (FT_Set_Char_Size(f, 0, static_cast<FT_F26Dot6>(std::lround(px_ * 64)), 72, 72)) {
                FT_Done_Face(f);
                continue;
            }
            fallbacks_.push_back(f);
        }
    }
    for (FT_Face f : fallbacks_) {
        *gi = FT_Get_Char_Index(f, cp);
        if (*gi) return f;
    }
    return nullptr;
}

void Font::draw_tofu(u32 slot, u32 width) {
    const int sx = static_cast<int>(slot % cols_) * cell_w_, sy = static_cast<int>(slot / cols_) * cell_h_;
    const int w = cell_w_ * static_cast<int>(width), h = cell_h_;
    const int x0 = std::max(1, w / 6), x1 = w - x0, y0 = h / 5, y1 = h - y0;
    for (int y = 0; y < h; ++y) {
        u8* row = pixels_ + static_cast<size_t>(sy + y) * atlas_w_ + sx;
        for (int x = 0; x < w; ++x) {
            bool edge = y >= y0 && y < y1 && x >= x0 && x < x1 &&
                        (y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1);
            row[x] = edge ? 150 : 0;
        }
    }
    mark_dirty(slot);
}

// Loads one glyph of a style and renders it into the face's glyph slot,
// slanting and thickening the outline first if the style is made that way.
bool Font::render(const Style& st, u32 gid) {
    FT_Face f = st.face;
    if (!st.slant && !st.embolden)
        return !FT_Load_Glyph(f, gid, load_flags_ | FT_LOAD_RENDER) && f->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY;
    if (FT_Load_Glyph(f, gid, load_flags_ | FT_LOAD_NO_BITMAP)) return false;
    int shift = 0;
    if (f->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (st.embolden) {
            FT_GlyphSlot_Embolden(f->glyph);
            // The outline grew to the right by em/24; keep it centred.
            shift = static_cast<int>((FT_MulFix(f->units_per_EM, f->size->metrics.y_scale) / 48 + 32) >> 6);
        }
        if (st.slant) {
            // The shear pivots on the baseline, which pushes the tops of
            // letters out of the cell; pivot around mid-height instead.
            FT_GlyphSlot_Oblique(f->glyph);
            shift += static_cast<int>(std::lround(px_ * 0.07f));
        }
    }
    if (FT_Render_Glyph(f->glyph, FT_LOAD_TARGET_MODE(load_flags_))) return false;
    f->glyph->bitmap_left -= shift;
    return f->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY;
}

// Copies the glyph a face has just rendered into a slot, clipped to it.
// Fallback glyphs are centred in the cell; the main font keeps its own side
// bearings.
void Font::put_glyph(FT_Face f, u32 slot, u32 width, bool centred) {
    const FT_Bitmap& bm = f->glyph->bitmap;
    const int sx = static_cast<int>(slot % cols_) * cell_w_, sy = static_cast<int>(slot / cols_) * cell_h_;
    const int w = cell_w_ * static_cast<int>(width), h = cell_h_;
    for (int y = 0; y < h; ++y) std::memset(pixels_ + static_cast<size_t>(sy + y) * atlas_w_ + sx, 0, static_cast<size_t>(w));

    const int ox = centred ? (w - static_cast<int>(bm.width)) / 2 : f->glyph->bitmap_left;
    const int oy = baseline_ - f->glyph->bitmap_top;
    for (int y = 0; y < static_cast<int>(bm.rows); ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= h) continue;
        const u8* src = bm.buffer + static_cast<ptrdiff_t>(y) * bm.pitch;
        u8* dst = pixels_ + static_cast<size_t>(sy + dy) * atlas_w_ + sx;
        for (int x = 0; x < static_cast<int>(bm.width); ++x) {
            int dx = ox + x;
            if (dx >= 0 && dx < w) dst[dx] = src[x];
        }
    }
    mark_dirty(slot);
}

void Font::blit_glyph(FT_Face f, u32 gid, u32 slot, u32 width, bool centred) {
    if (FT_Load_Glyph(f, gid, load_flags_ | FT_LOAD_RENDER) || f->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
        draw_tofu(slot, width);
    else
        put_glyph(f, slot, width, centred);
}

void Font::clear_slot(u32 slot) {
    const int sx = static_cast<int>(slot % cols_) * cell_w_, sy = static_cast<int>(slot / cols_) * cell_h_;
    for (int y = 0; y < cell_h_; ++y) std::memset(pixels_ + static_cast<size_t>(sy + y) * atlas_w_ + sx, 0, static_cast<size_t>(cell_w_));
    mark_dirty(slot);
}

void Font::copy_slot(u32 from, u32 to) {
    const int fx = static_cast<int>(from % cols_) * cell_w_, fy = static_cast<int>(from / cols_) * cell_h_;
    const int tx = static_cast<int>(to % cols_) * cell_w_, ty = static_cast<int>(to / cols_) * cell_h_;
    for (int y = 0; y < cell_h_; ++y)
        std::memcpy(pixels_ + static_cast<size_t>(ty + y) * atlas_w_ + tx, pixels_ + static_cast<size_t>(fy + y) * atlas_w_ + fx,
                    static_cast<size_t>(cell_w_));
    mark_dirty(to);
}

// ---------------------------------------------------------------- ligatures

// Slot holding the part of a glyph that falls into the cell `k` cells away
// from the glyph's own (k < 0 is to the left). The glyph must be the one
// currently rendered in the glyph slot of the style's face; `left` and `top` say where its
// bitmap sits relative to the pen.
u32 Font::slice_slot(u32 s, u32 gid, int k, int left, int top) {
    const u32 key = (s << 22) | (gid << 6) | static_cast<u32>(k + 32);
    u32 mask = static_cast<u32>(slices_.size()) - 1;
    for (u32 h = hash_u32(key) & mask; slices_[h].key != kEmpty; h = (h + 1) & mask)
        if (slices_[h].key == key) return slices_[h].slot;
    if (slices_used_ * 2 >= slices_.size()) reset_atlas();
    const u32 slot = alloc_slots(1);
    u32 h = hash_u32(key) & mask;
    while (slices_[h].key != kEmpty) h = (h + 1) & mask;
    slices_[h] = {key, slot};
    ++slices_used_;

    const FT_Bitmap& bm = styles_[s].face->glyph->bitmap;
    const int sx = static_cast<int>(slot % cols_) * cell_w_, sy = static_cast<int>(slot / cols_) * cell_h_;
    for (int y = 0; y < cell_h_; ++y)
        std::memset(pixels_ + static_cast<size_t>(sy + y) * atlas_w_ + sx, 0, static_cast<size_t>(cell_w_));
    const int ox = left - k * cell_w_, oy = baseline_ - top;
    for (int y = 0; y < static_cast<int>(bm.rows); ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= cell_h_) continue;
        const u8* src = bm.buffer + static_cast<ptrdiff_t>(y) * bm.pitch;
        u8* dst = pixels_ + static_cast<size_t>(sy + dy) * atlas_w_ + sx;
        for (int x = std::max(0, -ox); x < static_cast<int>(bm.width) && ox + x < cell_w_; ++x) dst[ox + x] = src[x];
    }
    mark_dirty(slot);
    return slot;
}

// Shapes a run of ASCII characters in one style and works out which slot
// each of its cells shows.
void Font::shape_run(u32 s, const char* text, u32 n, u16* out) {
    Style& st = styles_[s];
    const u32 block = s << kStyleShift;
    auto plain = [&] {
        for (u32 i = 0; i < n; ++i) out[i] = static_cast<u16>(static_cast<u8>(text[i]) | block);
    };
    if (!st.sh) return plain();
    const auto glyphs = st.sh->shape(text, n);
    bool same = glyphs.size() == n;
    for (u32 i = 0; same && i < n; ++i)
        same = glyphs[i].gid == st.base_gid[static_cast<u8>(text[i])] && glyphs[i].cluster == i && !glyphs[i].dx && !glyphs[i].dy;
    if (same || glyphs.empty()) return plain();  // nothing joined

    bool inked[kMaxRun];
    for (u32 i = 0; i < n; ++i) {
        out[i] = 0;
        inked[i] = false;
    }
    const u32 gen = generation_;
    for (const ShapedGlyph& g : glyphs) {
        const u32 c = g.cluster;
        if (c >= n) continue;
        if (g.gid == st.base_gid[static_cast<u8>(text[c])] && !g.dx && !g.dy) {
            out[c] = static_cast<u16>(static_cast<u8>(text[c]) | block);  // unchanged: its usual slot
            inked[c] = true;
            continue;
        }
        if (!render(st, g.gid)) continue;
        const FT_Bitmap& bm = st.face->glyph->bitmap;
        if (bm.width == 0 || bm.rows == 0) continue;  // a blank spacer
        const int w = static_cast<int>(bm.width);
        const int left = st.face->glyph->bitmap_left + g.dx, top = st.face->glyph->bitmap_top + g.dy;
        const int k0 = std::max(-32, floor_div(left, cell_w_)), k1 = std::min(31, floor_div(left + w - 1, cell_w_));
        for (int k = k0; k <= k1; ++k) {
            const int t = static_cast<int>(c) + k;
            if (t < 0 || t >= static_cast<int>(n)) continue;
            if (k != 0) {
                // Into a neighbouring cell: only a real overlap counts, and
                // never over a cell that has ink of its own.
                const int lo = std::max(left, k * cell_w_), hi = std::min(left + w, (k + 1) * cell_w_);
                if (hi - lo < std::max(1, cell_w_ / 4) || inked[t]) continue;
            }
            out[t] = static_cast<u16>(slice_slot(s, g.gid, k, left, top));
            inked[t] = true;
            if (generation_ != gen) return plain();  // the atlas restarted; the frame is redone
        }
    }
}

// One run of joining characters, all in the same style (see ligate()).
void Font::ligate_run(u32* cells, u32 n) {
    while (n > kMaxRun) {
        ligate_run(cells, kMaxRun);
        cells += kMaxRun;
        n -= kMaxRun;
    }
    const u32 s = (cells[0] >> kStyleShift) & (kStyles - 1);
    const u32 gen = generation_;
    auto put = [&](u32 i, u32 slot) { cells[i] = (cells[i] & 0xFFFF0000u) | slot; };
    char text[kMaxRun + 1];
    text[0] = static_cast<char>('0' + s);  // the long-run cache key starts with the style
    char* const chars = text + 1;

    if (n <= 8) {
        // The characters, one per byte; the spare top bits of the first two
        // hold the style.
        u64 key = (static_cast<u64>(s & 1) << 7) | (static_cast<u64>(s >> 1) << 15);
        for (u32 i = 0; i < n; ++i) key |= static_cast<u64>(cells[i] & 0x7F) << (8 * i);
        u32 mask = static_cast<u32>(short_runs_.size()) - 1;
        auto home_of = [&](u64 k) { return static_cast<u32>((k * 0x9E3779B97F4A7C15ull) >> 40) & mask; };
        auto home = [&] { return home_of(key); };
        for (u32 h = home(); short_runs_[h].key; h = (h + 1) & mask) {
            if (short_runs_[h].key == key) {
                for (u32 i = 0; i < n; ++i) put(i, short_runs_[h].slot[i]);
                return;
            }
        }
        u16 out[8] = {};
        for (u32 i = 0; i < n; ++i) chars[i] = static_cast<char>(cells[i] & 0x7F);
        shape_run(s, chars, n, out);
        for (u32 i = 0; i < n; ++i) put(i, out[i]);
        if (generation_ != gen) return;
        if (short_used_ * 2 >= short_runs_.size()) {
            // Half full. Text in several styles, or with many kinds of
            // operator, has more distinct runs than the table starts with:
            // it doubles, up to a size no real text reaches. (Emptying it
            // instead would have every run shaped again, frame after frame.)
            if (short_runs_.size() < kMaxShortRuns) {
                std::vector<ShortRun> old(short_runs_.size() * 2);
                old.swap(short_runs_);
                mask = static_cast<u32>(short_runs_.size()) - 1;
                for (const ShortRun& r : old) {
                    if (!r.key) continue;
                    u32 at = home_of(r.key);
                    while (short_runs_[at].key) at = (at + 1) & mask;
                    short_runs_[at] = r;
                }
            } else {
                std::fill(short_runs_.begin(), short_runs_.end(), ShortRun{});
                short_used_ = 0;
            }
        }
        u32 h = home();
        while (short_runs_[h].key) h = (h + 1) & mask;
        short_runs_[h].key = key;
        std::memcpy(short_runs_[h].slot, out, sizeof out);
        ++short_used_;
        return;
    }

    for (u32 i = 0; i < n; ++i) chars[i] = static_cast<char>(cells[i] & 0x7F);
    const std::string_view sv(text, n + 1);
    if (auto it = long_runs_.find(sv); it != long_runs_.end()) {
        for (u32 i = 0; i < n; ++i) put(i, it->second[i]);
        return;
    }
    u16 out[kMaxRun];
    shape_run(s, chars, n, out);
    for (u32 i = 0; i < n; ++i) put(i, out[i]);
    if (generation_ != gen) return;
    if (long_runs_.size() >= 4096) long_runs_.clear();
    long_runs_.emplace(std::string(sv), std::vector<u16>(out, out + n));
}

}  // namespace blit
