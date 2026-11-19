// The minimap: down the right of a split wide enough for it, the file's
// lines at a couple of pixels each, each character a pixel in its colour
// (its syntax colour where the colours are in, else the text's), over the
// ends of long lines. Drawn in tiles of lines, each a picture on the GPU,
// made again when its lines or their colours change and it is on show.
// The view's place on it is a lighter box; a click or a drag there
// scrolls the view to it.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "app.hpp"

namespace blit {

namespace {

constexpr u64 kTileLines = 1024;  // (one or two on show: a draw each)
constexpr u32 kColumns = 100;  // characters of a line drawn

}  // namespace

struct Minimap {
    struct Tile {
        u32 image = 0;
        u64 made = ~0ull;      // the state it was made for (see key)
        std::vector<u8> rgba;  // (what it was made of: one made again the same is not sent to the GPU again)
    };
    std::map<u64, Tile> tiles;  // by the number of its first line / kTileLines
    u64 rev = ~0ull, hl = ~0ull, theme = 0;
    u64 low_line = 0;  // lines from here on changed since the tiles past it were made
    u64 gen = 1;       // what a tile made now is made for; tiles before low_line keep theirs
    std::map<u64, u64> gen_of;  // (the generation each tile index must have to be current)
    // A change seen and not yet acted on: the tiles from `from` on (all, for
    // other colours) made again once typing pauses, at `due`.
    u64 due_ns = 0, from = ~0ull;
};

namespace {

// The sizes on screen: pixels a line and a character, and the column's width.
void sizes(double scale, int* lp, int* cp, int* width) {
    *lp = std::max(1, static_cast<int>(std::lround(scale)));
    *cp = std::max(1, static_cast<int>(std::lround(scale / 2)));
    *width = *cp * static_cast<int>(kColumns) + static_cast<int>(std::lround(6 * scale));
}

}  // namespace

bool App::minimap_on(const Pane& p) const {
    if (!cfg_.minimap || focus_mode_ || !p.doc || p.doc->term || p.doc->prof || p.doc->md || p.doc->image || p.doc->pdf || p.doc->settings || p.floating || !font_) return false;
    int lp = 0, cp = 0, w = 0;
    sizes(scale_, &lp, &cp, &w);
    return p.x1 - p.text_x >= 80 * font_->cell_w() + w;  // (only where the text keeps its room)
}

// How much of a split's width the column takes (0: it has none).
int App::minimap_px(const Pane& p) const {
    if (!minimap_on(p)) return 0;
    int lp = 0, cp = 0, w = 0;
    sizes(scale_, &lp, &cp, &w);
    return w;
}

// The column's place in a split, and where the file's lines are on it.
bool App::minimap_place(const Pane& p, int* x0, int* y0, int* x1, int* y1, double* top_line, int* lp) const {
    if (!minimap_on(p)) return false;
    int cp = 0, w = 0;
    sizes(scale_, lp, &cp, &w);
    *x1 = p.x1;  // (over the scroll bar's strip: its box is where the view is, and dragging it scrolls)
    *x0 = *x1 - w;
    *y0 = p.text_y;
    *y1 = p.y1;
    const double lines = static_cast<double>(p.doc->ed.buf.line_count()), shown = static_cast<double>(*y1 - *y0) / *lp;
    // All of it, if it fits; else as far down it as the view is down the file.
    const double ch = font_->cell_h(), view_lines = static_cast<double>(p.y1 - p.text_y) / ch;
    const double most = std::max(1.0, lines - view_lines), frac = std::clamp(p.scroll_y / ch / most, 0.0, 1.0);
    *top_line = lines <= shown ? 0.0 : frac * (lines - shown);
    return true;
}

// A tile's picture: its lines, each character a pixel (or more) in its colour.
void App::minimap_tile(Doc& d, u64 index, std::vector<u8>& rgba) {
    int lp = 0, cp = 0, w = 0;
    sizes(scale_, &lp, &cp, &w);
    const int pw = w, ph = lp * static_cast<int>(kTileLines);  // (the column's width: its margins too)
    const int inset = static_cast<int>(std::lround(3 * scale_));
    // On the text's background (each tile opaque: nothing under it is drawn).
    const u8 bg[4] = {static_cast<u8>(theme_.bg[0] * 255), static_cast<u8>(theme_.bg[1] * 255), static_cast<u8>(theme_.bg[2] * 255), 255};
    rgba.resize(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4);
    for (size_t k = 0; k < rgba.size(); k += 4) std::memcpy(&rgba[k], bg, 4);
    const Buffer& b = d.ed.buf;
    const Highlights& hl = d.syntax.active() ? d.syntax.highlights() : Highlights();
    const float* fg = theme_.palette[0];
    const u64 first = index * kTileLines, last = std::min<u64>(b.line_count(), first + kTileLines);
    std::string text;
    for (u64 l = first; l < last; ++l) {
        const u64 s = b.line_start(l), e = d.ed.line_end(l);
        text = b.text(s, std::min<u64>(e - s, 4 * kColumns));
        u32 col = 0;
        size_t m = hl.empty() ? 0 : hl.upper(s);
        u8 st = m && !hl.empty() ? hl.style[m - 1] : 0;
        for (size_t i = 0; i < text.size() && col < kColumns; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (!hl.empty())
                while (m < hl.size() && hl.at(m) <= s + i) st = hl.style[m++];
            if (c == '\t') {
                col = (col / d.ed.tab_width + 1) * d.ed.tab_width;
                continue;
            }
            if ((c & 0xC0) == 0x80) continue;  // (a character's later bytes)
            if (c == ' ' || c == '\r') {
                ++col;
                continue;
            }
            const u32 ink = st ? (d.style_bits[st] >> kCellInkShift) & 31 : 0;
            const float* rgb = ink ? theme_.palette[ink] : fg;
            for (int y = 0; y < lp; ++y)
                for (int x = 0; x < cp; ++x) {
                    u8* px = &rgba[((static_cast<size_t>(l - first) * lp + y) * pw + inset + col * cp + x) * 4];
                    const float a = 0.8f;  // (a little softer than the text)
                    for (int ch = 0; ch < 3; ++ch) px[ch] = static_cast<u8>((rgb[ch] * a + theme_.bg[ch] * (1 - a)) * 255);
                }
            ++col;
        }
    }
}

// Each frame: the column of each split that has one, its tiles on show
// made if they are not current, and the view's box over them.
void App::minimap_layout(FrameDesc& fd) {
    minimap_due_ns_ = 0;
    for (const std::unique_ptr<Pane>& pp : panes_) {
        Pane& p = *pp;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0, lp = 0;
        double top = 0;
        if (!minimap_place(p, &x0, &y0, &x1, &y1, &top, &lp)) continue;
        Doc& d = *p.doc;
        if (!d.minimap) d.minimap = std::make_shared<Minimap>();
        Minimap& m = *d.minimap;
        const u64 theme = static_cast<u64>(theme_.bg[0] * 1000) * 1000003 + static_cast<u64>(scale_ * 100) + (dark() ? 7 : 0);
        const u64 now = now_ns();
        if (m.theme != theme) {  // (another theme or size: every tile, now)
            m.theme = theme;
            m.from = 0;
            m.due_ns = now;
        }
        if (m.hl != d.hl_gen) {  // (other colours: every tile, once typing pauses)
            m.hl = d.hl_gen;
            m.from = 0;
            m.due_ns = now + 150'000'000ull;
        }
        if (m.rev != d.ed.revision()) {  // (the text changed: the tiles from where it did)
            const u64 low = d.ed.edit_log().changed_since(m.rev, d.ed.revision());
            m.rev = d.ed.revision();
            m.from = std::min(m.from, d.ed.buf.line_of(std::min(low, d.ed.buf.size())) / kTileLines);
            m.due_ns = now + 150'000'000ull;
        }
        if (m.from != ~0ull && now >= m.due_ns) {
            ++m.gen;
            for (auto it = m.gen_of.lower_bound(m.from); it != m.gen_of.end();) it = m.gen_of.erase(it);
            m.from = ~0ull;
            m.due_ns = 0;
        }
        if (m.due_ns && (!minimap_due_ns_ || m.due_ns < minimap_due_ns_)) minimap_due_ns_ = m.due_ns;
        // The background (the text's, over what is under it), then the tiles on show.
        if (fd.picture_count + 8 >= kMaxPictures) continue;
        const u8 bg[4] = {static_cast<u8>(theme_.bg[0] * 255), static_cast<u8>(theme_.bg[1] * 255), static_cast<u8>(theme_.bg[2] * 255), 255};
        const u32 bg_key = (static_cast<u32>(bg[0]) << 16) | (static_cast<u32>(bg[1]) << 8) | bg[2];
        if (!minimap_bg_ || minimap_bg_key_ != bg_key) {
            if (minimap_bg_) gpu_.image_drop(minimap_bg_);
            minimap_bg_ = gpu_.image_add(bg, 1, 1);
            minimap_bg_key_ = bg_key;
        }
        const u64 lines = d.ed.buf.line_count();
        const u64 t0 = static_cast<u64>(top) / kTileLines, t1 = std::min<u64>((lines + kTileLines - 1) / kTileLines, (static_cast<u64>(top) + static_cast<u64>((y1 - y0) / lp) + 1) / kTileLines + 1);
        int cp = 0, w = 0;
        sizes(scale_, &lp, &cp, &w);
        for (u64 t = t0; t < t1 && fd.picture_count + 2 < kMaxPictures; ++t) {
            Minimap::Tile& tile = m.tiles[t];
            const auto want = m.gen_of.find(t);
            if (!tile.image || want == m.gen_of.end() || tile.made != want->second) {
                minimap_tile(d, t, minimap_rgba_);
                if (!tile.image || minimap_rgba_ != tile.rgba) {
                    if (tile.image) gpu_.image_drop(tile.image);
                    int lp2 = 0, cp2 = 0, w2 = 0;
                    sizes(scale_, &lp2, &cp2, &w2);
                    tile.image = gpu_.image_add(minimap_rgba_.data(), w2, lp2 * static_cast<int>(kTileLines));
                    tile.rgba.swap(minimap_rgba_);
                }
                tile.made = m.gen;
                m.gen_of[t] = m.gen;
            }
            if (!tile.image) continue;
            PictureDraw& pd = fd.pictures[fd.picture_count++];
            pd = {};
            pd.image = tile.image;
            const double ty = y0 + (static_cast<double>(t * kTileLines) - top) * lp;
            pd.rect[0] = static_cast<float>(x0);
            pd.rect[1] = static_cast<float>(ty);
            pd.rect[2] = static_cast<float>(x1);
            pd.rect[3] = static_cast<float>(ty + static_cast<double>(lp) * kTileLines);
            pd.clip[0] = x0, pd.clip[1] = y0, pd.clip[2] = x1, pd.clip[3] = y1;
            pd.sharp = true;
        }
        // (Below the last tile, if the column goes on past it: the background.)
        const double tiles_end = y0 + (static_cast<double>(t1 * kTileLines) - top) * lp;
        if (minimap_bg_ && tiles_end < y1 && fd.picture_count < kMaxPictures) {
            PictureDraw& pd = fd.pictures[fd.picture_count++];
            pd = {};
            pd.image = minimap_bg_;
            pd.rect[0] = static_cast<float>(x0), pd.rect[1] = static_cast<float>(std::max<double>(y0, tiles_end)), pd.rect[2] = static_cast<float>(x1), pd.rect[3] = static_cast<float>(y1);
            pd.clip[0] = x0, pd.clip[1] = y0, pd.clip[2] = x1, pd.clip[3] = y1;
        }
        // (Tiles far from the view let go of.)
        for (auto it = m.tiles.begin(); it != m.tiles.end();) {
            if (it->first + 8 < t0 || it->first > t1 + 8) {
                if (it->second.image) gpu_.image_drop(it->second.image);
                it = m.tiles.erase(it);
            } else {
                ++it;
            }
        }
        // The view's place: a lighter box across the column.
        if (!minimap_box_) {
            const u8 white[4] = {255, 255, 255, 255};
            minimap_box_ = gpu_.image_add(white, 1, 1);
        }
        if (minimap_box_ && fd.picture_count < kMaxPictures) {
            const double ch = font_->cell_h(), first = p.scroll_y / ch, view = static_cast<double>(p.y1 - p.text_y) / ch;
            PictureDraw& pd = fd.pictures[fd.picture_count++];
            pd = {};
            pd.image = minimap_box_;
            pd.rect[0] = static_cast<float>(x0);
            pd.rect[1] = static_cast<float>(y0 + (first - top) * lp);
            pd.rect[2] = static_cast<float>(x1);
            pd.rect[3] = static_cast<float>(y0 + (first + view - top) * lp);
            pd.clip[0] = x0, pd.clip[1] = y0, pd.clip[2] = x1, pd.clip[3] = y1;
            pd.opacity = minimap_drag_ == p.id ? 0.16f : 0.08f;
            pd.radius = static_cast<float>(2 * scale_);
            pd.clear = true;
        }
    }
}

// A click on a split's column: the view goes to that place (its middle
// there), and follows the mouse while the button is held.
bool App::minimap_mouse(float x, float y, bool down) {
    const int px = static_cast<int>(x * density_), py = static_cast<int>(y * density_);
    Pane* p = nullptr;
    if (down) {
        p = pane_at(px, py);
    } else {
        for (const std::unique_ptr<Pane>& q : panes_)
            if (q->id == minimap_drag_) p = q.get();
    }
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, lp = 0;
    double top = 0;
    if (!p || !minimap_place(*p, &x0, &y0, &x1, &y1, &top, &lp)) return false;
    if (down && (px < x0 || px >= x1 || py < y0 || py >= y1)) return false;
    if (down) {
        focus_pane(p);
        minimap_drag_ = p->id;
    }
    const double ch = font_->cell_h(), view = static_cast<double>(p->y1 - p->text_y) / ch;
    const double line = top + static_cast<double>(std::clamp(py, y0, y1) - y0) / lp;
    Pane* const keep = pane_;
    pane_ = p;
    p->target_y = std::clamp((line - view / 2) * ch, 0.0, std::max(0.0, max_scroll_y()));
    p->scroll_y = p->target_y;
    pane_ = keep;
    dirty_ = true;
    return true;
}

// How long the event loop may wait before a minimap is due to be made
// again (-1: none is).
int App::minimap_wait_ms() const {
    if (!minimap_due_ns_) return -1;
    const u64 now = now_ns();
    return minimap_due_ns_ <= now ? 0 : static_cast<int>((minimap_due_ns_ - now) / 1'000'000 + 1);
}

// A document that goes: its tiles let go of.
void App::minimap_forget(const Doc& d) {
    if (!d.minimap) return;
    for (auto& [i, t] : d.minimap->tiles)
        if (t.image) gpu_.image_drop(t.image);
    d.minimap->tiles.clear();
}

// For the tests: each split's column (its place, its tiles), or none.
std::string App::minimap_dump() {
    std::string out;
    for (const std::unique_ptr<Pane>& p : panes_) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0, lp = 0;
        double top = 0;
        if (!minimap_place(*p, &x0, &y0, &x1, &y1, &top, &lp)) {
            out += (out.empty() ? "" : " | ") + std::string("none");
            continue;
        }
        size_t tiles = p->doc->minimap ? p->doc->minimap->tiles.size() : 0;
        out += (out.empty() ? "" : " | ") + std::string("top ") + std::to_string(static_cast<u64>(top)) + " tiles " + std::to_string(tiles);
    }
    return out;
}

}  // namespace blit
