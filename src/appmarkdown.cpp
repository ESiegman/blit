// The Markdown preview: a tab beside a Markdown file that shows it drawn
// (headings, emphasis, lists, tables, code, Mermaid diagrams: markdown.cpp,
// diagram.cpp; pictures: appimage.cpp), kept up to date as it is typed in,
// and scrolled to where the file's cursor is. Cmd+Shift+V opens it; it is
// read only. Pictures are read from files beside it (or anywhere on its
// machine); those on the web stay a link, unless markdown.web_pictures says
// to fetch them (appimage.cpp keeps them in blit's cache).
//
// The text is drawn into rows on a thread of its own (MdWorker): a key
// typed in the file only copies the text and hands it over, and the
// preview takes the new rows on a frame soon after. (Drawn in the frame, a
// README of 1,600 lines made each key cost 1.7 ms instead of 41 µs.) Code
// blocks are coloured there too, by the editor's grammars, the theme's
// colours of a few kinds of token standing for all of them.
//
// A notebook's view is one of these too (nbview.cpp draws it, on the same
// thread): beside the script of its cells, opened with it, following its
// cursor from cell to cell; a click on a cell takes the cursor there.
// Typing in one cell draws only that one again: the worker keeps the rest
// (NbCache), and each output's pictures are written to blit's cache once,
// named by what is in them, and shown from there as any picture is.
//
// Mermaid diagrams are laid out and drawn on that thread too, as pictures
// (diagram.hpp), in the theme's colours and the editor's font, at the size
// they are shown at. The worker keeps the diagrams it drew lately, so that
// typing elsewhere in the file never draws one again; the preview hands
// their pixels to the pictures (appimage.cpp) by a name made from all that
// went into them.
#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "fsio.hpp"
#include "diagram.hpp"
#include "markdown.hpp"
#include "nbview.hpp"
#include "remote.hpp"
#include "syntax.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

// A diagram drawn: its pixels (or the mistake in it).
struct MdDiagramPicture {
    std::vector<u8> rgba;
    int w = 0, h = 0;
    std::string error;
    u64 used = 0;  // (the worker's count when it was last asked for)
};

struct MdView {
    u64 source = 0;  // the document it shows
    bool notebook = false;  // a notebook's view (the source is the script of its cells)
    std::vector<MdRow> rows;
    std::vector<u32> heads;  // a notebook's: the line each of its cells starts on, as drawn
    u64 asked_nb = ~0ull;    // (and its outputs' generation, as last handed over)
    u64 followed_head = ~0ull;  // (the cell it last followed the cursor into)
    bool at_cell = false;       // (and that cell's top was put at its top: kept there as the rows change)
    // What the text was last handed to the worker at: its revision, the
    // width and height, and the pictures' generation (asked again when one
    // of them changes); how many times rows have come; whether they have
    // just come.
    u64 asked_rev = ~0ull, asked_pictures = ~0ull, asked_look = 0;
    u32 asked_width = 0, asked_tall = 0;
    bool asked_web = false;
    u64 renders = 0;
    bool fresh = false;
    // The raised and lowered characters its font can draw (markdown.hpp),
    // and the font that was asked of.
    static constexpr u8 kNoGlyphs = 0x80;
    u8 glyphs = kNoGlyphs;
    std::string glyphs_font;
    std::string base;               // the folder its pictures are found from
    std::vector<std::string> pics;  // the pictures its text names (their files)
    std::map<std::string, std::shared_ptr<const MdDiagramPicture>> diagrams;  // its diagrams' pictures, by name
    const ThemeColors* coloured = nullptr;  // the theme its colours were taken from
    float coloured_bg[3] = {-1, -1, -1};
    u32 top = 0;           // the first row shown
    u64 followed = ~0ull;  // the source's line it was last scrolled to
    double wheel = 0;      // a touchpad's fractions of a row, kept
    // As drawn: the cells, where, and the links in them.
    TextBox box;
    u64 key = 0;
    int x = 0, y = 0, pad_x = 0, pad_y = 0;
    u32 cols = 0, shown = 0;
    float palette[32][4] = {};
    float tones[8][4] = {};
    struct Hit {
        u32 row, c0, c1;
        std::string link;
    };
    std::vector<Hit> hits;
};

namespace {

constexpr u32 kMargin = 2;  // columns kept clear at either side
constexpr u32 kTop = 1;     // rows kept clear above the text (it does not start against the tab bar)

// The file a picture in the text is (relative to the folder of the file
// the text is in, `base`), or "" if it is not one to read: on the web
// (unless `web`: then its address), or in the text itself (data:).
std::string picture_source(const std::string& base, std::string_view src, bool web) {
    if (web && (src.starts_with("http://") || src.starts_with("https://"))) return std::string(src);
    if (src.starts_with("http://") || src.starts_with("https://") || src.starts_with("data:") || src.starts_with("//")) return {};
    if (src.starts_with("file://")) src.remove_prefix(7);
    src = src.substr(0, src.find_first_of("?#"));
    std::string path;
    for (size_t k = 0; k < src.size(); ++k) {  // (%20 and the like)
        if (src[k] == '%' && k + 2 < src.size() && std::isxdigit(static_cast<unsigned char>(src[k + 1])) && std::isxdigit(static_cast<unsigned char>(src[k + 2]))) {
            path += static_cast<char>(std::stoi(std::string(src.substr(k + 1, 2)), nullptr, 16));
            k += 2;
        } else {
            path += src[k];
        }
    }
    if (path.empty()) return {};
    static const std::string kept = cache_dir() + "/pictures/";
    if (path.starts_with(kept)) return path;  // (a notebook's picture, kept here whatever machine it is on)
    std::string host, there;
    const bool remote = split_remote(base, &host, &there);
    if (path.starts_with('/')) return remote ? remote_path(host, path) : path;
    if (base.empty()) return {};
    while (path.starts_with("./")) path.erase(0, 2);
    return base + "/" + path;
}

// The theme's colour for the first of these captures it gives one to (else `otherwise`).
Rgb pick(const ThemeColors& tc, std::initializer_list<const char*> captures, Rgb otherwise) {
    for (const char* c : captures) {
        const Rgb col = tc.syntax_style(c).color;
        if (col.r != tc.foreground.r || col.g != tc.foreground.g || col.b != tc.foreground.b) return col;
    }
    return otherwise;
}

void colours(const ThemeColors& tc, const float* bg, MdView& v) {
    auto set = [&](u32 i, Rgb c) {
        v.palette[i][0] = c.r, v.palette[i][1] = c.g, v.palette[i][2] = c.b, v.palette[i][3] = 1;
    };
    set(kMdText, tc.foreground);
    // The headings' colours: the one the theme names for that level
    // (markup.heading.2) where it does. Most name one for all headings, or
    // none: then the first level has it, and each level under it the first of
    // a few of the theme's other colours that no level above has, so that
    // the levels can be told apart.
    Rgb heads[4];
    int levels = 0;
    auto heading = [&](const char* capture, std::initializer_list<const char*> others, Rgb otherwise) {
        Rgb c = otherwise;
        const auto own = std::ranges::find_if(tc.syntax, [&](const ThemeColors::Entry& e) { return e.capture == capture && e.has_color; });
        if (own != tc.syntax.end()) {
            c = own->color;
        } else {
            for (const char* o : others) {
                const Rgb t = tc.syntax_style(o).color;
                const bool is_text = t.r == tc.foreground.r && t.g == tc.foreground.g && t.b == tc.foreground.b;
                const bool taken = std::any_of(heads, heads + levels, [&](const Rgb& h) { return std::abs(h.r - t.r) + std::abs(h.g - t.g) + std::abs(h.b - t.b) < 0.15f; });
                if (is_text || taken) continue;
                c = t;
                break;
            }
        }
        return heads[levels++] = c;
    };
    set(kMdHeading1, heading("markup.heading.1", {"markup.heading", "keyword"}, tc.accent));
    set(kMdHeading2, heading("markup.heading.2", {"function", "type", "keyword", "string"}, tc.info));
    set(kMdHeading3, heading("markup.heading.3", {"type", "string", "constant", "function", "keyword"}, tc.hint));
    set(kMdHeading, heading("markup.heading.4", {"keyword", "constant", "string", "type", "function"}, tc.warning));
    set(kMdCode, pick(tc, {"markup.raw", "string"}, tc.foreground));
    set(kMdLink, pick(tc, {"markup.link.url", "markup.link", "string.special.url", "function"}, tc.info));
    set(kMdQuote, pick(tc, {"markup.quote", "comment"}, tc.muted));
    set(kMdMuted, tc.muted);
    set(kMdAccent, tc.accent);
    set(kMdBorder, pick(tc, {"type", "function"}, tc.accent));
    set(kMdEdge, tc.muted);
    set(kMdLabel, pick(tc, {"string", "constant"}, tc.hint));
    set(kMdError, tc.error);
    set(kMdSlice0, pick(tc, {"function"}, tc.info));
    set(kMdSlice1, pick(tc, {"string"}, tc.warning));
    set(kMdSlice2, pick(tc, {"type"}, tc.hint));
    set(kMdSlice3, pick(tc, {"keyword"}, tc.error));
    set(kMdCodeKeyword, pick(tc, {"keyword"}, tc.accent));
    set(kMdCodeString, pick(tc, {"string"}, tc.foreground));
    set(kMdCodeComment, pick(tc, {"comment"}, tc.muted));
    set(kMdCodeFunction, pick(tc, {"function"}, tc.foreground));
    set(kMdCodeType, pick(tc, {"type"}, tc.foreground));
    set(kMdCodeConstant, pick(tc, {"number", "constant", "boolean"}, tc.foreground));
    set(kMdCodeOperator, pick(tc, {"operator", "punctuation"}, tc.foreground));
    set(kMdCodePlain, tc.foreground);
    set(kMdAnsiGreen, pick(tc, {"string", "markup.inserted"}, tc.hint));
    set(kMdAnsiYellow, tc.warning);
    set(kMdAnsiBlue, tc.info);
    set(kMdAnsiMagenta, pick(tc, {"keyword", "constant"}, tc.accent));
    set(kMdAnsiCyan, pick(tc, {"type", "function"}, tc.info));
    // Backgrounds: the text's own, a little towards its colour (an
    // error's, towards the theme's colour for errors).
    auto tone = [&](u32 i, float k, u32 to = kMdText) {
        for (int c = 0; c < 3; ++c) v.tones[i][c] = bg[c] + (v.palette[to][c] - bg[c]) * k;
        v.tones[i][3] = 1;
    };
    tone(kMdCodeTone, 0.07f);
    tone(kMdHeaderTone, 0.12f);
    tone(kMdNodeTone, 0.05f);
    tone(kMdErrorTone, 0.12f, kMdError);
}

// The preview's ink for a capture of a highlight query: by the first part
// of its name, the few kinds of token it tells apart.
u8 code_ink(std::string_view capture) {
    const std::string_view head = capture.substr(0, capture.find('.'));
    static const struct {
        std::string_view name;
        u8 ink;
    } kinds[] = {
        {"keyword", kMdCodeKeyword}, {"conditional", kMdCodeKeyword}, {"repeat", kMdCodeKeyword}, {"include", kMdCodeKeyword},
        {"exception", kMdCodeKeyword}, {"preproc", kMdCodeKeyword}, {"storageclass", kMdCodeKeyword},
        {"string", kMdCodeString}, {"character", kMdCodeString}, {"escape", kMdCodeString},
        {"comment", kMdCodeComment},
        {"function", kMdCodeFunction}, {"method", kMdCodeFunction}, {"constructor", kMdCodeFunction},
        {"type", kMdCodeType}, {"module", kMdCodeType}, {"namespace", kMdCodeType}, {"attribute", kMdCodeType},
        {"number", kMdCodeConstant}, {"float", kMdCodeConstant}, {"boolean", kMdCodeConstant}, {"constant", kMdCodeConstant},
        {"operator", kMdCodeOperator}, {"punctuation", kMdCodeOperator}, {"delimiter", kMdCodeOperator},
    };
    for (const auto& k : kinds)
        if (head == k.name) return k.ink;
    return kMdCodePlain;
}

// The size of a picture known to the worker: read (and its own size), or
// not to be shown (being read, or it cannot be).
struct PicSize {
    bool ready = false;
    int w = 0, h = 0;
    bool failed = false;
};

// A notebook's pictures in blit's cache not shown for 30 days, let go of
// (each one shown has its time set again when it is kept: those in use stay).
void prune_pictures(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::days(30);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->path().filename().string().starts_with("nb-")) continue;
        std::error_code e;
        if (const fs::file_time_type t = it->last_write_time(e); !e && t < old) fs::remove(it->path(), e);
    }
}

// A notebook's picture (an output's bytes) kept in blit's cache, named by
// what is in it: its file ("" if it cannot be written). Written once, and
// put in place whole (a reader never sees half of it, nor another blit's
// half). (On the worker; the first time in a run, those long unused go.)
std::string keep_picture(std::string_view bytes, std::string_view kind) {
    if (bytes.empty()) return {};
    u64 h = 1469598103934665603ull;
    for (const char c : bytes) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    char name[48];
    std::snprintf(name, sizeof name, "nb-%016llx.%.*s", static_cast<unsigned long long>(h), static_cast<int>(kind.size()), kind.data());
    const std::string dir = cache_dir();
    if (dir.empty()) return {};
    static std::once_flag pruned;
    std::call_once(pruned, [&] { prune_pictures(dir + "/pictures"); });
    const std::string path = dir + "/pictures/" + name;
    if (fs_kind(path) == 1) {
        std::error_code e;
        const auto now = std::filesystem::file_time_type::clock::now();
        if (const auto t = std::filesystem::last_write_time(path, e); !e && t < now - std::chrono::days(1)) std::filesystem::last_write_time(path, now, e);
        return path;
    }
    std::string err;
    const std::string part = path + ".part" + std::to_string(::getpid());
    if (!fs_mkdirs(dir + "/pictures", &err) || !fs_write(part, bytes, &err) || std::rename(part.c_str(), path.c_str()) != 0) {
        std::remove(part.c_str());
        return {};
    }
    return path;
}

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 20;
    SDL_PushEvent(&ev);
}

}  // namespace

struct App::MdWorker {
    struct Job {
        u64 view = 0;  // the preview's document
        u64 rev = 0, pictures = 0;
        u32 width = 0, tall = 0;
        std::string text, base;
        bool web = false;  // pictures on the web fetched (markdown.web_pictures)
        double density = 1;
        int cw = 1, ch = 1;
        std::map<std::string, PicSize> known;  // pictures asked about before
        // Code blocks coloured (not the first time: then it is drawn in
        // the frame, and comes again coloured), and where grammars are.
        bool colour = false;
        SyntaxOptions syntax;
        std::vector<GrammarSource> grammars;
        // A notebook's view: the text is the script of its cells, these
        // their outputs (notebook.hpp), its code in this language.
        std::shared_ptr<const std::vector<NotebookCell>> notebook;
        std::string lang;
        // Diagrams: the colours, the font (its file, and its size in pixels),
        // and the fonts for characters it lacks.
        DiagramPalette palette;
        std::string font_path;
        int font_index = 0;
        std::vector<FontFile> fallbacks;
        float font_px = 26;
        u8 glyphs = kMdAllGlyphs;  // the raised and lowered characters the font can draw
    };
    struct Done {
        u64 view = 0, rev = 0, pictures = 0;
        u32 width = 0, tall = 0;
        std::vector<MdRow> rows;
        std::vector<u32> heads;  // a notebook's: each cell's first line
        std::vector<std::string> wanted;  // pictures it names that were not known yet
        std::map<std::string, std::shared_ptr<const MdDiagramPicture>> diagrams;
    };
    std::mutex mu;
    std::condition_variable_any cv;
    std::map<u64, Job> todo;  // the newest for each preview (an older one not drawn yet is not drawn at all)
    u64 busy = 0;             // the preview being drawn (0: none)
    u64 posted = 0;           // how many have been handed over (for md_wait)
    std::vector<Done> done;
    // Code blocks coloured lately, by language and text, with their inks:
    // typing in a file redraws all of it, and its code blocks are mostly
    // as they were. Only the worker uses it.
    std::map<std::string, std::vector<u8>> coloured;
    std::map<u64, NbCache> notebooks;  // (each notebook view's: its cells as drawn last)
    // Diagrams drawn lately, by all that went into them; the font they
    // are drawn in (the worker's own). Only the worker uses them.
    std::map<u64, std::shared_ptr<MdDiagramPicture>> drawn;
    u64 asked = 0;
    std::atomic<u64> laid = 0;  // how many it has laid out (for the tests: not one again when it has not changed)
    std::atomic<i64> day = kDiagramNoDay;  // the date the tests say it is (else the clock's)
    i64 today() const {
        const i64 d = day.load();
        return d != kDiagramNoDay ? d : diagram_today();
    }
    DiagramFont font;
    std::stop_token stop;  // (the worker's: a diagram being laid out gives up when blit quits)
    std::jthread worker;

    MdWorker() {
        worker = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~MdWorker() {
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
    // The rows of a job's text. A picture's size: one of its pixels to a
    // point (or the size the text asks for), made smaller to fit across,
    // and to fit in the view.
    // A diagram's picture: the one drawn before, if nothing that went into
    // it has changed; else laid out and drawn now, as wide as the room
    // across (at most), at the display's density.
    std::shared_ptr<MdDiagramPicture> diagram(const Job& j, std::string_view src, u32 room, u64* key) {
        u64 h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
        };
        const float room_px = static_cast<float>(room) * static_cast<float>(j.cw);
        mix(src.data(), src.size());
        mix(&j.palette, sizeof j.palette);
        mix(j.font_path.data(), j.font_path.size());
        mix(&j.font_index, sizeof j.font_index), mix(&j.font_px, sizeof j.font_px), mix(&room_px, sizeof room_px), mix(&j.density, sizeof j.density);
        for (const FontFile& f : j.fallbacks) mix(f.path.data(), f.path.size() + 1), mix(&f.index, sizeof f.index);
        // (a Gantt chart that marks today: another picture each day)
        const i64 date = diagram_dated(src) ? today() : kDiagramNoDay;
        mix(&date, sizeof date);
        *key = h;
        ++asked;
        if (const auto it = drawn.find(h); it != drawn.end()) {
            it->second->used = asked;
            return it->second;
        }
        font.open(j.font_path, j.font_index, j.fallbacks);
        ++laid;
        const float density = static_cast<float>(std::max(1.0, j.density));
        DiagramStyle st;
        st.font = &font;
        st.size = j.font_px / density;
        st.room = room_px / density;
        st.palette = j.palette;
        st.stop = stop;
        st.today = date;
        Drawing d;
        auto pic = std::make_shared<MdDiagramPicture>();
        const u8 made = diagram_make(src, st, &d, &pic->error);  // (a kind not drawn: why, in error)
        if (stop.stop_requested()) return nullptr;  // (blit is quitting: nothing kept)
        if (made == kDiagramDrawn) {
            // As wide as it is, or the room; and no side more than a picture may have.
            float scale = std::min(1.0f, st.room / std::max(1.0f, d.w)) * density;
            scale = std::min({scale, 4096 / std::max(1.0f, d.w), 4096 / std::max(1.0f, d.h)});
            RasterImage img(std::max(1, static_cast<int>(std::ceil(d.w * scale))), std::max(1, static_cast<int>(std::ceil(d.h * scale))));
            diagram_paint(d, scale, font, &img);
            pic->w = img.w(), pic->h = img.h();
            pic->rgba = std::move(img.pixels());
        }
        pic->used = asked;
        // At most so many kept (the least lately asked for let go first).
        size_t bytes = pic->rgba.size();
        for (const auto& [k, p] : drawn) bytes += p->rgba.size();
        while (!drawn.empty() && (drawn.size() >= 48 || bytes > (96u << 20))) {
            auto oldest = drawn.begin();
            for (auto it = drawn.begin(); it != drawn.end(); ++it)
                if (it->second->used < oldest->second->used) oldest = it;
            bytes -= oldest->second->rgba.size();
            drawn.erase(oldest);
        }
        drawn[h] = pic;
        return pic;
    }

    static Done draw(Job& j, std::map<std::string, std::vector<u8>>* coloured = nullptr, MdWorker* self = nullptr, NbCache* nb = nullptr) {
        Done d;
        d.view = j.view, d.rev = j.rev, d.pictures = j.pictures, d.width = j.width, d.tall = j.tall;
        const NbPictureFn sized = [&](const std::string& s, u32 room, u32 want_w, u32 want_h, MdPic* out) {
            const std::string path = picture_source(j.base, s, j.web);
            if (path.empty()) return kNbPictureFailed;
            const auto it = j.known.find(path);
            if (it == j.known.end()) {
                d.wanted.push_back(path);
                j.known[path] = {};
                return kNbPictureWaiting;
            }
            if (it->second.failed) return kNbPictureFailed;
            if (!it->second.ready || it->second.w <= 0 || it->second.h <= 0) return kNbPictureWaiting;
            double pw = it->second.w, ph = it->second.h;
            if (want_w && want_h) pw = want_w, ph = want_h;
            else if (want_w) ph = ph * want_w / pw, pw = want_w;
            else if (want_h) pw = pw * want_h / ph, ph = want_h;
            pw *= j.density, ph *= j.density;
            const double k = std::min({1.0, static_cast<double>(room) * j.cw / pw, static_cast<double>(std::max<u32>(j.tall, 4) - 2) * j.ch / ph});
            out->w = static_cast<u32>(std::max(1.0, std::round(pw * k)));
            out->h = static_cast<u32>(std::max(1.0, std::round(ph * k)));
            out->cols = (out->w + static_cast<u32>(j.cw) - 1) / static_cast<u32>(j.cw);
            out->rows = (out->h + static_cast<u32>(j.ch) - 1) / static_cast<u32>(j.ch);
            return kNbPictureReady;
        };
        const MdPictureFn size = [&](const std::string& s, u32 room, u32 want_w, u32 want_h, MdPic* out) { return sized(s, room, want_w, want_h, out) == kNbPictureReady; };
        const MdCodeFn colour = [&](std::string_view word, std::string_view code, std::vector<u8>* inks) {
            const std::string lang = Syntax::language_named(word, j.grammars, j.syntax);
            if (lang.empty()) return false;
            std::string key = lang + '\n';
            key += code;
            if (const auto it = coloured->find(key); it != coloured->end()) {
                *inks = it->second;
                return true;
            }
            std::vector<u8> styles;
            std::vector<std::string> names;
            if (!Syntax::highlight_text(lang, code, j.syntax, &styles, &names)) return false;
            std::vector<u8> by_style(names.size(), kMdCodePlain);
            for (size_t s = 1; s < names.size(); ++s) by_style[s] = code_ink(names[s]);
            inks->resize(styles.size());
            for (size_t k = 0; k < styles.size(); ++k) (*inks)[k] = styles[k] < by_style.size() ? by_style[styles[k]] : static_cast<u8>(kMdCodePlain);
            if (coloured->size() >= 1024) coloured->clear();  // (a notebook's cells each a block)
            coloured->emplace(std::move(key), *inks);
            return true;
        };
        // Diagrams: drawn on the worker only (the first, quick drawing shows their code).
        const MdDiagramFn diagram = [&](std::string_view src, u32 room, MdPic* out, std::string* note) -> u8 {
            if (!self) return kMdDiagramLater;
            u64 key = 0;
            const std::shared_ptr<MdDiagramPicture> pic = self->diagram(j, src, room, &key);
            if (!pic) return kMdDiagramLater;  // (blit is quitting)
            if (!pic->error.empty() || pic->rgba.empty()) {
                *note = pic->error.empty() ? "nothing to draw" : pic->error;
                return kMdDiagramError;
            }
            char name[40];
            std::snprintf(name, sizeof name, "diagram:%016llx", static_cast<unsigned long long>(key));
            out->src = name;
            out->w = static_cast<u32>(pic->w), out->h = static_cast<u32>(pic->h);
            out->cols = (out->w + static_cast<u32>(j.cw) - 1) / static_cast<u32>(j.cw);
            out->rows = (out->h + static_cast<u32>(j.ch) - 1) / static_cast<u32>(j.ch);
            d.diagrams[name] = pic;
            return kMdDiagramPicture;
        };
        if (j.notebook) {
            NbLook look;
            look.width = j.width;
            look.lang = j.lang;
            look.colour = j.colour && coloured ? colour : MdCodeFn{};
            look.picture = sized;
            look.keep = keep_picture;
            look.glyphs = j.glyphs;
            // (the rows kept depend on the pictures known, and the room they have)
            look.key = std::to_string(j.pictures) + " " + std::to_string(j.tall) + " " + std::to_string(j.cw) + "x" + std::to_string(j.ch) + " " + std::to_string(j.density) + (j.colour ? " c" : "") + " g" + std::to_string(j.glyphs);
            d.rows = nb_render(j.text, *j.notebook, look, nb);
            for (const CellLines& c : notebook_cell_lines(j.text)) d.heads.push_back(static_cast<u32>(c.head));
            return d;
        }
        d.rows = md_render(j.text, j.width, size, j.colour && coloured ? colour : MdCodeFn{}, diagram, j.glyphs);
        return d;
    }
    void run(std::stop_token st) {
        stop = st;
        for (;;) {
            Job j;
            {
                std::unique_lock l(mu);
                if (!cv.wait(l, st, [&] { return !todo.empty(); })) return;
                const auto it = todo.begin();
                j = std::move(it->second);
                todo.erase(it);
                busy = j.view;
            }
            if (j.notebook && !notebooks.contains(j.view) && notebooks.size() >= 8) notebooks.clear();  // (views gone, mostly)
            Done d = draw(j, &coloured, this, j.notebook ? &notebooks[j.view] : nullptr);
            {
                std::lock_guard l(mu);
                busy = 0;
                done.push_back(std::move(d));
            }
            wake();
        }
    }
};

// Rows the worker has drawn, into their previews; pictures they name, asked for.
void App::md_sync() {
    if (!md_worker_) return;
    std::vector<MdWorker::Done> done;
    {
        std::lock_guard l(md_worker_->mu);
        done.swap(md_worker_->done);
    }
    for (MdWorker::Done& d : done) {
        Doc* doc = find_doc(d.view);
        if (!doc || !doc->md) continue;
        MdView& v = *doc->md;
        v.rows = std::move(d.rows);
        v.heads = std::move(d.heads);
        v.diagrams = std::move(d.diagrams);
        for (const auto& [name, pic] : v.diagrams) picture_made(name, pic->rgba.data(), pic->w, pic->h);
        ++v.renders;
        v.fresh = true;
        for (const std::string& path : d.wanted) {
            if (std::ranges::find(v.pics, path) != v.pics.end()) continue;
            v.pics.push_back(path);
            // (one read already, by another preview or a tab: drawn again with it now)
            if (picture(path)->state != PictureInfo::kReading) v.asked_pictures = ~0ull;
        }
        dirty_ = true;
    }
    // A diagram's pictures that no preview shows any more (each version of
    // one being typed in is a picture of its own, megabytes on the GPU),
    // let go of now rather than a minute later.
    if (!done.empty()) md_unmade();
}

// The diagrams' pictures no preview names, let go of.
void App::md_unmade() {
    pictures_unmade([&](const std::string& name) {
        for (const std::unique_ptr<Pane>& p : panes_)
            for (const std::shared_ptr<Doc>& d : p->docs)
                if (d->md && d->md->diagrams.count(name)) return true;
        return false;
    });
}

// For the tests: frames until every preview's text has been drawn as it is now (10 s at most).
void App::md_wait() {
    const u64 until = now_ns() + 10'000'000'000ull;
    while (now_ns() < until) {
        const u64 posted = md_worker_ ? md_worker_->posted : 0;
        frame();
        bool idle = true;
        if (md_worker_) {
            for (;;) {
                {
                    std::lock_guard l(md_worker_->mu);
                    idle = md_worker_->todo.empty() && !md_worker_->busy;
                }
                if (idle || now_ns() >= until) break;
                SDL_Delay(1);
            }
        }
        md_sync();
        if (!md_worker_ || md_worker_->posted == posted) break;
    }
    frame();  // (the rows that came, put in the cells: what a click lands on)
}

// The front file shown drawn, in a tab in a split to its right (or that
// tab brought to the front, if there is one); from the preview, back to its file.
void App::md_open() {
    Doc& src = *pane_->doc;
    if (src.md) {
        Pane* p = nullptr;
        size_t at = 0;
        if (find_doc(src.md->source, &p, &at)) {
            focus_pane(p);
            tab_show(at);
        }
        return;
    }
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->md && p->docs[i]->md->source == src.id) {
                Pane* const editing = pane_;
                pane_ = p.get();
                tab_show(i);
                focus_pane(editing);
                dirty_ = true;
                return;
            }
    const u64 id = src.id;
    const bool notebook = src.nb != nullptr;
    const std::string name = src.ed.path.empty() ? std::string("untitled") : src.ed.path.substr(src.ed.path.find_last_of('/') + 1);
    Pane* const editing = pane_;
    Pane* into = nullptr;
    if (notebook)  // (with another notebook's view: beside it)
        for (const std::unique_ptr<Pane>& p : panes_)
            if (p.get() != editing && !p->floating && std::ranges::any_of(p->docs, [](const std::shared_ptr<Doc>& d) { return d->md && d->md->notebook; })) into = p.get();
    if (!into) into = split_pane(editing, kEdgeRight);
    if (!into) into = editing;
    pane_ = into;
    Doc* d = add_doc(pane_->tab + 1);
    d->title = (notebook ? "Notebook: " : "Preview: ") + name;
    d->read_only = true;
    d->md = std::make_shared<MdView>();
    d->md->source = id;
    d->md->notebook = notebook;
    for (size_t i = 0; i < pane_->docs.size(); ++i)
        if (pane_->docs[i].get() == d) tab_show(i);
    focus_pane(editing);
    dirty_ = true;
}

// The notebook a tab is of: itself, or the one a notebook's view shows (null: neither).
Doc* App::nb_of(Doc& d) {
    if (d.nb) return &d;
    if (!d.md || !d.md->notebook) return nullptr;
    Doc* src = find_doc(d.md->source);
    return src && src->nb ? src : nullptr;
}

// Cmd+Shift+V in a notebook (its script, or its view): the view shown
// beside the script if it is not, else closed.
void App::nb_view_toggle() {
    const Doc& d = *pane_->doc;
    const u64 source = d.md ? d.md->source : d.id;
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& v : p->docs)
            if (v->md && v->md->notebook && v->md->source == source) return nb_view_close(source);
    md_open();
}

// The views of a notebook closed (their split too, if nothing else is in
// it); the keyboard stays where it was, if that is still there.
void App::nb_view_close(u64 source) {
    Pane* const editing = pane_;
    for (;;) {
        Pane* where = nullptr;
        size_t at = 0;
        for (const std::unique_ptr<Pane>& p : panes_)
            for (size_t i = 0; i < p->docs.size() && !where; ++i)
                if (p->docs[i]->md && p->docs[i]->md->notebook && p->docs[i]->md->source == source) where = p.get(), at = i;
        if (!where) break;
        Pane* const was = pane_;
        pane_ = where;
        tab_close(at, true);
        if (pane_ == where && was != where) pane_ = was;  // (the split stayed: back to the one that had the keyboard)
    }
    if (std::ranges::any_of(panes_, [&](const std::unique_ptr<Pane>& p) { return p.get() == editing; })) focus_pane(editing);
    dirty_ = true;
}

void App::md_layout(FrameDesc& fd) {
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    const ThemeColors& tc = cfg_.theme(dark());
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->md || fd.popup_count >= std::size(fd.popups)) continue;
        MdView& v = *pane->doc->md;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 8 || h < ch * 2) continue;
        const u32 cols = static_cast<u32>(w / cw), all_rows = static_cast<u32>(h / ch);
        const u32 rows = all_rows > kTop + 1 ? all_rows - kTop : all_rows;  // (the text's, under the clear row)
        const u32 top_row = all_rows - rows;
        v.x = x, v.y = y, v.cols = cols, v.shown = rows;
        v.pad_x = (w - static_cast<int>(cols) * cw) / 2;
        v.pad_y = (h - static_cast<int>(all_rows) * ch) / 2;
        if (v.coloured != &tc || !std::equal(theme_.bg, theme_.bg + 3, v.coloured_bg)) {  // (the theme's colours, looked up when it changes)
            colours(tc, theme_.bg, v);
            v.coloured = &tc;
            std::copy(theme_.bg, theme_.bg + 3, v.coloured_bg);
        }
        // Drawn again when the text or the width changes.
        Pane* src_pane = nullptr;
        size_t src_at = 0;
        Doc* src = find_doc(v.source, &src_pane, &src_at);
        const u32 width = cols > 2 * kMargin + 12 ? cols - 2 * kMargin : cols;
        u64 nb_gen = 0;  // (a notebook's outputs: how many times they have changed)
        if (v.notebook && src) nb_model(*src, &nb_gen);
        // How its diagrams look: the colours, the font, the display's density.
        u64 look = 1469598103934665603ull;
        {
            auto mix = [&](const void* p, size_t n) {
                for (size_t i = 0; i < n; ++i) look = (look ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
            };
            mix(v.palette, sizeof v.palette), mix(theme_.bg, sizeof(float) * 3), mix(font.path().data(), font.path().size());
            const float px = font.px();
            mix(&px, sizeof px), mix(&density_, sizeof density_);
            static const u64 fallbacks = [] {  // (the same while blit runs: worked out once, not each frame)
                u64 k = 1469598103934665603ull;
                for (const FontFile& f : font_fallbacks())
                    for (const char c : f.path + '\n' + std::to_string(f.index)) k = (k ^ static_cast<u8>(c)) * 1099511628211ull;
                return k;
            }();
            mix(&fallbacks, sizeof fallbacks);
        }
        // Its text handed to the worker when it, the width or the height change, a picture comes, a notebook's outputs change, or the diagrams' look changes.
        if (src && (src->ed.revision() != v.asked_rev || width != v.asked_width || rows != v.asked_tall || pictures_gen_ != v.asked_pictures || cfg_.markdown_web_pictures != v.asked_web ||
                    nb_gen != v.asked_nb || look != v.asked_look)) {
            if (!md_worker_) md_worker_ = std::make_shared<MdWorker>();
            v.base = !src->ed.path.empty() ? parent_of(full_path(src->ed.path)) : std::string();
            MdWorker::Job j;
            j.view = pane->doc->id;
            j.rev = src->ed.revision(), j.pictures = pictures_gen_, j.width = width, j.tall = rows;
            j.text = src->ed.buf.text(0, src->ed.buf.size());
            j.base = v.base;
            j.web = cfg_.markdown_web_pictures;
            j.density = density_, j.cw = cw, j.ch = ch;
            for (const std::string& path : v.pics) {
                const PictureInfo* p = picture(path);
                j.known[path] = {p->state == PictureInfo::kReady, p->full_w, p->full_h, p->state == PictureInfo::kFailed};
            }
            j.colour = cfg_.syntax;
            j.syntax.parser_dirs = cfg_.parser_dirs;
            j.syntax.query_dirs = cfg_.query_dirs;
            j.grammars = cfg_.grammars;
            if (v.notebook && !nb_model(*src, &nb_gen, &j.notebook, &j.lang)) j.notebook = std::make_shared<const std::vector<NotebookCell>>();
            for (u32 i = 0; i < kMdInkCount; ++i) std::copy(v.palette[i], v.palette[i] + 3, j.palette.ink[i]);
            std::copy(theme_.bg, theme_.bg + 3, j.palette.bg);
            j.font_path = font.path(), j.font_index = font.index(), j.font_px = font.px();
            j.fallbacks = font_fallbacks();
            if (v.glyphs_font != j.font_path || v.glyphs == MdView::kNoGlyphs) {  // (looked up when the font changes)
                auto all = [&](std::initializer_list<u32> cps) { return std::ranges::all_of(cps, [&](u32 cp) { return font.can_draw(cp); }); };
                v.glyphs = static_cast<u8>((all({0x2070, 0xB9, 0x2074, 0x2079}) ? kMdSuperDigits : 0) | (all({0x2080, 0x2081, 0x2089}) ? kMdSubDigits : 0) |
                                           (all({0x207A, 0x207F, 0x1D43, 0x1D57, 0x1D40}) ? kMdSuperLetters : 0) | (all({0x208A, 0x2090, 0x1D62, 0x209C}) ? kMdSubLetters : 0));
                v.glyphs_font = j.font_path;
            }
            j.glyphs = v.glyphs;
            v.asked_rev = j.rev, v.asked_width = width, v.asked_tall = rows, v.asked_pictures = j.pictures, v.asked_web = j.web, v.asked_nb = nb_gen, v.asked_look = look;
            // (the first time: at once, so that it opens drawn; its code is
            // coloured on the worker. A notebook's, only there: its pictures
            // are written out as it is drawn.)
            const bool first = v.renders == 0 && v.rows.empty() && !v.notebook;
            if (first) {
                MdWorker::Done d = MdWorker::draw(j);
                std::lock_guard l(md_worker_->mu);
                md_worker_->done.push_back(std::move(d));
            }
            const bool fences = j.text.find("```") != std::string::npos || j.text.find("~~~") != std::string::npos;
            if (!first || v.notebook || (fences && (j.colour || j.text.find("mermaid") != std::string::npos))) {
                std::lock_guard l(md_worker_->mu);
                md_worker_->todo[j.view] = std::move(j);
                ++md_worker_->posted;
                md_worker_->cv.notify_all();
            }
            if (v.renders == 0) md_sync();
        }
        const bool fresh = v.fresh;
        v.fresh = false;
        // Scrolled to where the file's cursor is, while the file is being worked on (not the preview).
        if (src && src_pane && src_pane->doc == src && pane.get() != pane_ && v.notebook) {
            // A notebook's: to the cell the cursor is in, from its top when
            // it fits (and as the cursor moves in it, only when its row is
            // not on show; with rows drawn again, the cell's top kept at
            // the top if it was put there).
            const u64 line = src->ed.buf.line_of(src->ed.cursor());
            if ((line != v.followed || fresh) && !v.rows.empty()) {
                v.followed = line;
                const auto in = std::upper_bound(v.heads.begin(), v.heads.end(), line);
                const u64 head = in == v.heads.begin() ? 0 : *(in - 1);
                u32 first = 0, row = ~0u;
                for (u32 k = 0; k < v.rows.size(); ++k) {
                    if (v.rows[k].line < head) first = k + 1;
                    if (row == ~0u && v.rows[k].line >= line) row = k;
                }
                first = std::min<u32>(first, static_cast<u32>(v.rows.size()) - 1);
                if (row == ~0u || row < first) row = first;
                if (head != v.followed_head || row < v.top || row >= v.top + rows || v.at_cell) {
                    v.at_cell = row - first < rows * 2 / 3;
                    v.top = v.at_cell ? first : row - rows / 3;
                }
                v.followed_head = head;
            }
        } else if (src && src_pane && src_pane->doc == src && pane.get() != pane_) {
            const u64 line = src->ed.buf.line_of(src->ed.cursor());
            if (line != v.followed || fresh) {
                const bool moved = line != v.followed;
                v.followed = line;
                u32 row = 0;
                for (u32 k = 0; k < v.rows.size(); ++k)
                    if (v.rows[k].line <= line) row = k;
                if (moved || row < v.top || row >= v.top + rows) v.top = row > rows / 3 ? row - rows / 3 : 0;
            }
        }
        const u32 max_top = v.rows.size() > rows / 2 ? static_cast<u32>(v.rows.size()) - rows / 2 : 0;
        v.top = std::min(v.top, max_top);

        u64 key = 1469598103934665603ull;
        auto mix = [&](u64 k) { key = (key ^ k) * 1099511628211ull; };
        mix(cols), mix(rows), mix(font.generation()), mix(v.top), mix(v.renders), mix(src ? 1 : 0);
        for (int c = 0; c < 3; ++c) mix(static_cast<u64>(v.palette[kMdText][c] * 1000));
        if (key != v.key) {
            v.key = key;
            v.box.resize(cols, all_rows);
            v.hits.clear();
            if (!src) v.box.put(font, top_row, kMargin, "The file this showed has been closed.", kMdMuted, 0);
            for (u32 k = 0; src && k < rows && v.top + k < v.rows.size(); ++k) {
                const MdRow& row = v.rows[v.top + k];
                const u32 r = k + top_row;  // (its row in the box)
                u32* line = v.box.cells.data() + static_cast<size_t>(r) * cols;
                if (row.fill)
                    for (u32 c = kMargin + row.fill_from; c < std::min(cols, kMargin + row.fill_to); ++c) line[c] |= kCellSelected | static_cast<u32>(row.fill) << kCellDecoShift;
                u32 c = kMargin;
                for (const MdRun& run : row.runs) {
                    // (A link is underlined, struck-out text has a line through it.)
                    u32 style = (run.style & kMdBold ? Font::kBold : 0) | (run.style & kMdItalic ? Font::kItalic : 0) |
                                (run.style & kMdStrike ? Font::kStrike : 0) | (run.link.empty() ? 0 : Font::kUnderline);
                    if (!font.ensure_style(style)) style &= Font::kFaces - 1;
                    const u32 end = v.box.put(font, r, c, run.text, run.ink, style);
                    if (run.tone)
                        for (u32 cell = c; cell < end; ++cell) line[cell] = (line[cell] & ~kCellDecoMask) | kCellSelected | static_cast<u32>(run.tone) << kCellDecoShift;
                    if (!run.link.empty()) v.hits.push_back({r, c, end, run.link});
                    c = end;
                }
                for (const MdPic& pic : row.pics)
                    if (!pic.link.empty()) v.hits.push_back({r, kMargin + pic.col, kMargin + pic.col + pic.cols, pic.link});
            }
        }
        Popup& q = fd.popups[fd.popup_count++];
        q = Popup{};
        q.x = x, q.y = y, q.w = w, q.h = h;
        q.pad_x = v.pad_x, q.pad_y = v.pad_y;
        q.cols = v.box.cols, q.rows = v.box.rows;
        q.cells = v.box.cells.data();
        q.border_px = 0;
        q.bg = theme_.bg;
        q.palette = &v.palette[0][0];
        q.tones = &v.tones[0][0];
        // The pictures on show, over the rows kept for them (each from its
        // first row, or the first row shown if that is above the view).
        for (u32 k = 0; src && k < rows && v.top + k < v.rows.size(); ++k)
            for (const MdPic& pic : v.rows[v.top + k].pics) {
                if ((pic.part != 0 && k != 0) || fd.picture_count >= std::size(fd.pictures)) continue;
                const u32 r = k + top_row;
                const PictureInfo* p = nullptr;
                std::string path;
                if (const auto made = v.diagrams.find(pic.src); made != v.diagrams.end()) {  // (a diagram's)
                    path = pic.src;
                    p = picture_made(path, made->second->rgba.data(), made->second->w, made->second->h);
                } else {
                    path = picture_source(v.base, pic.src, cfg_.markdown_web_pictures);
                    if (path.empty()) continue;
                    p = picture(path);
                }
                if (p->state != PictureInfo::kReady) continue;
                const u32 image = picture_gpu(path, pic.w, pic.h);
                PictureDraw& d = fd.pictures[fd.picture_count++];
                d = PictureDraw{};
                d.image = image;
                const int left = x + v.pad_x + static_cast<int>(kMargin + pic.col) * cw;
                const int top = y + v.pad_y + (static_cast<int>(r) - static_cast<int>(pic.part)) * ch;
                d.rect[0] = static_cast<float>(left), d.rect[1] = static_cast<float>(top);
                d.rect[2] = static_cast<float>(left + static_cast<int>(pic.w)), d.rect[3] = static_cast<float>(top + static_cast<int>(pic.h));
                d.clip[0] = x, d.clip[1] = y + v.pad_y + static_cast<int>(top_row) * ch, d.clip[2] = x + w, d.clip[3] = y + v.pad_y + static_cast<int>(all_rows) * ch;
                d.after = fd.popup_count;
                d.sharp = !p->vector && static_cast<double>(pic.w) / p->w >= 3;
                d.clear = p->clear;
            }
    }
}

bool App::md_hit(int px, int py, Pane** pane, u32* row, u32* col) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        if (px < v.x || py < v.y || px >= p->x1 || py >= p->y1 || !v.cols) continue;
        const int c = (px - v.x - v.pad_x) / font_->cell_w(), r = (py - v.y - v.pad_y) / font_->cell_h();
        *pane = p.get();
        *row = static_cast<u32>(std::max(0, r));
        *col = static_cast<u32>(std::max(0, c));
        return true;
    }
    return false;
}

bool App::md_key(const SDL_KeyboardEvent& k) {
    if (!pane_->doc->md || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT))) return false;
    MdView& v = *pane_->doc->md;
    const u32 page = v.shown > 3 ? v.shown - 2 : 1;
    const u32 last = static_cast<u32>(v.rows.size());
    switch (k.key) {
        case SDLK_UP:
        case SDLK_K: v.top = v.top ? v.top - 1 : 0; break;
        case SDLK_DOWN:
        case SDLK_J: v.top = std::min(v.top + 1, last); break;
        case SDLK_PAGEUP:
        case SDLK_B: v.top = v.top > page ? v.top - page : 0; break;
        case SDLK_PAGEDOWN:
        case SDLK_SPACE: v.top = std::min(v.top + page, last); break;
        case SDLK_HOME: v.top = 0; break;
        case SDLK_G:
            v.top = k.mod & SDL_KMOD_SHIFT ? last : 0;
            break;
        case SDLK_END: v.top = last; break;
        case SDLK_ESCAPE: {  // back to its file
            Pane* p = nullptr;
            size_t at = 0;
            if (!find_doc(v.source, &p, &at)) return false;
            focus_pane(p);
            tab_show(at);
            break;
        }
        default: return false;
    }
    term_drop_text_ = true;  // (a letter's text is not typed after it)
    v.at_cell = false;
    dirty_ = true;
    return true;
}

bool App::md_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    u32 row = 0, col = 0;
    if (!md_hit(static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_), &pane, &row, &col)) return false;
    MdView& v = *pane->doc->md;
    const bool precise = w.y != std::floor(w.y);
    v.wheel -= w.y * (precise ? 1.0 : 3.0);
    const int step = static_cast<int>(v.wheel);
    v.wheel -= step;
    v.top = static_cast<u32>(std::clamp(static_cast<i64>(v.top) + step, i64{0}, static_cast<i64>(v.rows.size())));
    v.at_cell = false;  // (scrolled by hand: where it is left)
    dirty_ = true;
    return true;
}

// A click on the preview gives it the keyboard; on a link, follows it: a
// web address in the browser, a file beside the source in a tab.
bool App::md_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    u32 row = 0, col = 0;
    if (!md_hit(static_cast<int>(b.x * density_), static_cast<int>(b.y * density_), &pane, &row, &col)) return false;
    MdView& v = *pane->doc->md;
    dirty_ = true;
    const bool on_link = std::ranges::any_of(v.hits, [&](const MdView::Hit& h) { return h.row == row && col >= h.c0 && col < h.c1; });
    if (v.notebook && b.button == SDL_BUTTON_LEFT && !on_link) {
        // A notebook's cell: the script's cursor to the line it is of (its
        // first, for its outputs and what is between cells), the keyboard
        // to the script, the view staying where it is.
        const u32 all_rows = static_cast<u32>((pane->y1 - v.y) / font_->cell_h()), top_row = all_rows > v.shown ? all_rows - v.shown : 0;
        Pane* p = nullptr;
        size_t at = 0;
        Doc* src = find_doc(v.source, &p, &at);
        if (!src || row < top_row || v.top + row - top_row >= v.rows.size()) return true;
        u64 line = v.rows[v.top + row - top_row].line;
        const auto in = std::upper_bound(v.heads.begin(), v.heads.end(), line);
        const u64 head = in == v.heads.begin() ? 0 : *(in - 1);
        for (const CellLines& c : notebook_cell_lines(src->ed.buf.text(0, src->ed.buf.size())))
            if (c.head == head && (line < c.first || line >= c.end)) line = c.first;
        focus_pane(p);
        tab_show(at);
        Editor& ed = src->ed;
        line = std::min<u64>(line, ed.buf.line_count() ? ed.buf.line_count() - 1 : 0);
        if (cfg_.modal) vim_attach();
        ed.move_to(ed.buf.line_start(line), false);
        scroll_to_cursor();
        v.followed = line, v.followed_head = head, v.at_cell = false;  // (it stays as it is: the click was on what is on show)
        return true;
    }
    focus_pane(pane);
    if (b.button != SDL_BUTTON_LEFT) return true;
    for (const MdView::Hit& h : v.hits) {
        if (h.row != row || col < h.c0 || col >= h.c1) continue;
        const std::string& link = h.link;
        if (link.starts_with("http://") || link.starts_with("https://") || link.starts_with("mailto:")) {
            SDL_OpenURL(link.c_str());
        } else if (link.starts_with('#')) {
            // A place in it, its row to the top: the row of that name (a
            // footnote's text), the first row with a mark of a footnote
            // (back from its text), or a heading.
            const std::string want = link.substr(1);
            u32 to = ~0u;
            for (u32 k = 0; k < v.rows.size() && to == ~0u; ++k)
                if (v.rows[k].id == want) to = k;
            if (to == ~0u && want.starts_with("fnref-")) {
                const std::string mark = "#fn-" + want.substr(6);
                for (u32 k = 0; k < v.rows.size() && to == ~0u; ++k)
                    if (std::ranges::any_of(v.rows[k].runs, [&](const MdRun& r) { return r.link == mark; })) to = k;
            }
            for (u32 k = 0; k < v.rows.size() && to == ~0u; ++k) {
                std::string slug;
                for (const char ch : md_row_text(v.rows[k])) {
                    if (std::isalnum(static_cast<unsigned char>(ch))) slug += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    else if (ch == ' ' || ch == '-') slug += '-';
                }
                if (slug == want) to = k;
            }
            if (to != ~0u) v.top = to;
        } else if (Doc* src = find_doc(v.source); src && !src->ed.path.empty()) {
            std::string path = link.substr(0, link.find('#'));
            if (!path.starts_with('/')) path = std::string(parent_of(full_path(src->ed.path))) + "/" + path;
            open_path(path.c_str());
        }
        break;
    }
    return true;
}

// The preview in front (the first there is), its rows as text (for the
// tests); with `how` 1, each run of a row in the colour it is drawn in
// ("[#rrggbb]text"); with 2, in its style, tone and link too
// ("[#rrggbb bi t1 >link]text": bold, italic, struck, underlined), a row
// with an id after that ("#id ").
std::string App::md_dump(int how) {
    md_wait();
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        std::string out;
        for (const MdRow& r : v.rows) {
            if (!how) {
                out += md_row_text(r) + "\n";
                continue;
            }
            if (how == 2 && !r.id.empty()) out += "#" + r.id + " ";
            for (const MdRun& run : r.runs) {
                char hex[16];
                const float* c = v.palette[run.ink < 32 ? run.ink : 0];
                auto byte = [](float f) { return static_cast<int>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255)); };
                std::snprintf(hex, sizeof hex, "[#%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]));
                out += hex;
                if (how == 2) {
                    if (run.style) out += ' ';
                    for (int s = 0; s < 4; ++s)
                        if (run.style >> s & 1) out += "bisu"[s];
                    if (run.tone) out += " t" + std::to_string(run.tone);
                    if (!run.link.empty()) out += " >" + run.link;
                }
                out += "]" + run.text;
            }
            out += "\n";
        }
        return out;
    }
    return "no preview\n";
}

// For the tests: where the preview in front shows a link that goes to
// `link` (the middle of its first cell, in the window's points; false: none
// on show), and the first row it shows, as text.
bool App::md_link_point(std::string_view link, float* x, float* y) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        for (const MdView::Hit& h : v.hits) {
            if (h.link != link) continue;
            *x = static_cast<float>((v.x + v.pad_x + static_cast<int>(h.c0) * font_->cell_w() + font_->cell_w() / 2) / density_);
            *y = static_cast<float>((v.y + v.pad_y + static_cast<int>(h.row) * font_->cell_h() + font_->cell_h() / 2) / density_);
            return true;
        }
        return false;
    }
    return false;
}

std::string App::md_top_dump() {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        return "top " + std::to_string(v.top) + ": " + (v.top < v.rows.size() ? md_row_text(v.rows[v.top]) : std::string()) + "\n";
    }
    return "no preview\n";
}

bool App::md_notebook(const Doc& d) { return d.md && d.md->notebook; }

// The columns a notebook's view (the first) draws its outputs in, by the
// width it last had (0: it has none, or has not been drawn yet).
u32 App::nb_view_room(const Doc& src) {
    u64 gen = 0;
    std::shared_ptr<const std::vector<NotebookCell>> cells;
    if (!nb_model(src, &gen, &cells)) return 0;
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (d->md && d->md->notebook && d->md->source == src.id && d->md->asked_width) return nb_room(d->md->asked_width, *cells);
    return 0;
}

// For the tests: where the notebook's view shows its row `row` (from 0),
// in the window's points (its middle, a few columns in). False: not on show.
bool App::nb_view_point(u32 row, float* x, float* y) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md || !p->doc->md->notebook) continue;
        const MdView& v = *p->doc->md;
        const u32 all_rows = static_cast<u32>((p->y1 - v.y) / font_->cell_h()), top_row = all_rows > v.shown ? all_rows - v.shown : 0;
        if (row < v.top || row >= v.top + v.shown) return false;
        *x = static_cast<float>((v.x + v.pad_x + static_cast<int>(kMargin + 12) * font_->cell_w()) / density_);
        *y = static_cast<float>((v.y + v.pad_y + static_cast<int>(row - v.top + top_row) * font_->cell_h() + font_->cell_h() / 2) / density_);
        return true;
    }
    return false;
}

// For the tests: the notebook's view (the first there is, once drawn): the
// first row on show and the script's line it is of, then each row, as the
// line (from 1) it is of and its text (a picture by its file's name).
std::string App::nb_view_dump() {
    md_wait();
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md || !p->doc->md->notebook) continue;
        const MdView& v = *p->doc->md;
        std::string out = "top " + std::to_string(v.top) + " @" + std::to_string(v.top < v.rows.size() ? v.rows[v.top].line + 1 : 0) + "\n";
        for (const MdRow& r : v.rows) {
            MdRow named = r;
            for (MdPic& pic : named.pics) pic.src = pic.src.substr(pic.src.find_last_of('/') + 1);
            out += std::to_string(r.line + 1) + ": " + md_row_text(named) + "\n";
        }
        return out;
    }
    return "no notebook view\n";
}

// The date the preview's diagrams are drawn for, from now on (for the
// tests: a Gantt chart's today marker on a day of their own).
void App::md_set_today(i64 day) {
    if (!md_worker_) md_worker_ = std::make_shared<MdWorker>();
    md_worker_->day = day;
}

// The preview in front's Mermaid diagrams, laid out as its worker lays
// them out (for the tests): each one's size in points (or why it is not a
// picture), then where its nodes, subgraphs and labels are, and points
// along each edge, its ends first and last.
std::string App::md_diagram_dump() {
    md_wait();
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        const Doc* src = find_doc(v.source);
        if (!src) return "no file\n";
        DiagramFont f;
        f.open(font_->path(), font_->index(), font_fallbacks());
        DiagramStyle st;
        st.font = &f;
        const float density = std::max(1.0f, static_cast<float>(density_));
        st.size = font_->px() / density;
        st.room = static_cast<float>(v.asked_width) * static_cast<float>(font_->cell_w()) / density;
        for (u32 i = 0; i < kMdInkCount; ++i) std::copy(v.palette[i], v.palette[i] + 3, st.palette.ink[i]);
        std::copy(theme_.bg, theme_.bg + 3, st.palette.bg);
        st.today = md_worker_ ? md_worker_->today() : diagram_today();
        const std::string text = src->ed.buf.text(0, src->ed.buf.size());
        std::string out = "laid out " + std::to_string(md_worker_ ? md_worker_->laid.load() : 0) + "\n", block;
        // The pictures on show: their sizes and a hash of their pixels (the
        // same on every system, for the same font file).
        std::vector<std::string> pics;
        for (const auto& [name, pic] : v.diagrams) {
            u64 h = 1469598103934665603ull;
            for (const u8 b : pic->rgba) h = (h ^ b) * 1099511628211ull;
            char line[80];
            std::snprintf(line, sizeof line, "picture %dx%d %016llx\n", pic->w, pic->h, static_cast<unsigned long long>(h));
            pics.emplace_back(line);
        }
        std::sort(pics.begin(), pics.end());
        for (const std::string& l : pics) out += l;
        out += "kept " + std::to_string(pictures_unmade([](const std::string&) { return true; })) + "\n";  // (made pictures held, none let go)
        bool in = false;
        int n = 0;
        for (size_t at = 0; at < text.size();) {
            const size_t nl = std::min(text.find('\n', at), text.size());
            const std::string_view raw = std::string_view(text).substr(at, nl - at), line = diagram_trim(raw);
            at = nl + 1;
            if (!in) {
                in = line.starts_with("```mermaid") || line.starts_with("~~~mermaid");
                block.clear();
                continue;
            }
            if (!line.starts_with("```") && !line.starts_with("~~~")) {
                block += std::string(raw) + "\n";  // (as it is: a mindmap nests by indentation)
                continue;
            }
            in = false;
            Drawing d;
            DiagramGeometry geo;
            std::string err;
            const u8 made = diagram_make(block, st, &d, &err, &geo);
            char head[96];
            std::snprintf(head, sizeof head, "diagram %d ", ++n);
            out += head;
            if (made == kDiagramDrawn) {
                std::snprintf(head, sizeof head, "%.0fx%.0f\n", static_cast<double>(d.w), static_cast<double>(d.h));
                out += head + geo.dump();
            } else {
                out += made == kDiagramError ? "error: " + err + "\n" : std::string("cells\n");
            }
        }
        return out;
    }
    return "no preview\n";
}

}  // namespace blit
