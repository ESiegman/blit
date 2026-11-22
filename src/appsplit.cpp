// Splits: the window divided into panes, side by side or one above the
// other, each with its own tabs. How they are arranged is a small tree
// (SplitNode); this file places the panes, moves the dividers, and carries
// tabs, whole groups of tabs and files from the tree from one pane to
// another, or to a new pane at the side of one.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "tabbar.hpp"
#include "textbox.hpp"

namespace blit {

namespace {

constexpr float kEdgeZone = 0.28f;  // this share of a pane, in from each side, is "split here"
constexpr u32 kBlankCell = 0;

SplitNode* find_leaf(SplitNode* n, const Pane* p, SplitNode** parent = nullptr) {
    if (!n) return nullptr;
    if (n->pane) return n->pane == p ? n : nullptr;
    for (SplitNode* half : {n->a.get(), n->b.get()}) {
        if (half->pane == p) {
            if (parent) *parent = n;
            return half;
        }
        if (SplitNode* hit = find_leaf(half, p, parent)) return hit;
    }
    return nullptr;
}

Pane* first_pane(SplitNode* n) {
    while (n && !n->pane) n = n->a.get();
    return n ? n->pane : nullptr;
}

// Puts a pane beside the one a leaf shows: the leaf becomes two halves.
void attach(SplitNode* leaf, Pane* fresh, bool side_by_side, bool first) {
    auto old_half = std::make_unique<SplitNode>();
    old_half->pane = leaf->pane;
    auto new_half = std::make_unique<SplitNode>();
    new_half->pane = fresh;
    leaf->pane = nullptr;
    leaf->side_by_side = side_by_side;
    leaf->share = 0.5f;
    leaf->a = std::move(first ? new_half : old_half);
    leaf->b = std::move(first ? old_half : new_half);
}

// Takes a pane out of the arrangement: what was its other half has the
// room of both. (Not for the last pane.)
void detach(SplitNode* root, const Pane* p) {
    SplitNode* parent = nullptr;
    SplitNode* leaf = find_leaf(root, p, &parent);
    if (!leaf || !parent) return;
    std::unique_ptr<SplitNode> rest = std::move(parent->a.get() == leaf ? parent->b : parent->a);
    std::unique_ptr<SplitNode> a = std::move(rest->a), b = std::move(rest->b);
    parent->pane = rest->pane;
    parent->side_by_side = rest->side_by_side;
    parent->share = rest->share;
    parent->a = std::move(a);  // (lets go of the leaf)
    parent->b = std::move(b);
}

// The node whose divider is under a point, if any.
SplitNode* divider_at(SplitNode* n, int px, int py, int grab) {
    if (!n || n->pane) return nullptr;
    const bool inside = px >= n->x0 && px < n->x1 && py >= n->y0 && py < n->y1;
    if (inside && (n->side_by_side ? std::abs(px - n->b->x0) <= grab : std::abs(py - n->b->y0) <= grab)) return n;
    if (SplitNode* hit = divider_at(n->a.get(), px, py, grab)) return hit;
    return divider_at(n->b.get(), px, py, grab);
}

std::string_view file_name(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}  // namespace

struct App::Drag {
    DragKind kind = kDragNone;
    bool moving = false;  // the mouse has gone far enough for this to be a drag and not a click
    bool away = false;    // it has left where it was picked up (its own tab bar, the side panel)
    float press_x = 0, press_y = 0, x = 0, y = 0;  // window coordinates
    u64 doc = 0;               // a tab: the document
    u64 pane = 0;              // a tab or a group: the pane it came from
    std::string path;          // a file from the tree
    SplitNode* node = nullptr; // a divider: whose
    // Where it would land if let go now: in a pane (among its tabs), or
    // as a new split along one edge of it. 0: nowhere.
    u64 over = 0;
    Edge edge = kEdgeNone;
    bool on_bar = false;
    size_t bar_index = 0;
    // What is carried along under the mouse.
    TextBox ghost;
    std::string label;
    u32 ghost_font = ~0u;
    // The mouse pointer over a divider.
    SDL_Cursor* across = nullptr;
    SDL_Cursor* updown = nullptr;
    int cursor = 0;  // 0 the usual one, 1 across, 2 up and down
};

// ---------------------------------------------------------------- arrangement

void App::splits_create() { drag_ = std::make_shared<Drag>(); }

void App::for_each_pane(std::function_ref<void()> fn) {
    Pane* const focused = pane_;
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc) continue;
        pane_ = p.get();
        fn();
    }
    pane_ = focused;
}

// Gives every pane its rectangle: the window right of the side panel,
// divided the way the tree says.
void App::place_panes(int w, int h) {
    if (!split_ || !font_) return;
    place_node(*split_, std::min(panel_px(), w), 0, w, std::max(1, h - vim_px() - status_px()));  // (the bottom lines are the command line's and the status bar's)
    for_each_pane([this] {  // (never past the pane's own edges, however little room it has)
        pane_->text_y = std::min(pane_->y0 + tabs_px() + crumbs_px(*pane_), pane_->y1);
        pane_->text_x = std::min(pane_->x0 + gutter_px(), pane_->x1);
    });
}

void App::place_node(SplitNode& n, int x0, int y0, int x1, int y1) {
    n.x0 = x0;
    n.y0 = y0;
    n.x1 = x1;
    n.y1 = y1;
    if (n.pane) {
        n.pane->x0 = x0;
        n.pane->y0 = y0;
        n.pane->x1 = x1;
        n.pane->y1 = y1;
        return;
    }
    // Neither half gets less than a few cells, while there is room for that.
    if (n.side_by_side) {
        const int size = x1 - x0, least = std::min(12 * font_->cell_w(), size / 2);
        const int mid = x0 + std::clamp(static_cast<int>(std::lround(size * n.share)), least, size - least);
        place_node(*n.a, x0, y0, mid, y1);
        place_node(*n.b, mid, y0, x1, y1);
    } else {
        const int size = y1 - y0, least = std::min(tabs_px() + 3 * font_->cell_h(), size / 2);
        const int mid = y0 + std::clamp(static_cast<int>(std::lround(size * n.share)), least, size - least);
        place_node(*n.a, x0, y0, x1, mid);
        place_node(*n.b, x0, mid, x1, y1);
    }
}

Pane* App::pane_at(int px, int py) const {
    for (auto p = panes_.rbegin(); p != panes_.rend(); ++p)  // (from the last: a peek's box, over the split it floats on)
        if (px >= (*p)->x0 && px < (*p)->x1 && py >= (*p)->y0 && py < (*p)->y1) return p->get();
    return nullptr;
}

// Gives a pane the keyboard. The language server goes with it, to the file
// in front there.
void App::focus_pane(Pane* p) {
    if (!p || p == pane_) return;
    stop_lsp(true);
    // The same file in both: Vim leaves the view it was in (Insert mode
    // ends there, at that view's cursor) and takes up the other's.
    if (cfg_.modal && pane_ && pane_->doc && p->doc == pane_->doc && vim_doc_ == pane_->doc->id) {
        place_view(pane_);
        vim_.suspend(pane_->doc->ed);
        pane_->doc->ed.watch = nullptr;
        vim_doc_ = 0;
    }
    pane_ = p;
    place_view(pane_);
    dragging_ = dragging_thumb_ = false;
    lsp_pending_ = true;
    grammars_pending_ = true;
    dirty_ = true;
    if (pane_->doc) vim_attach();
}

void App::forget_doc(const Doc* d) {
    remote_forget(d->id);  // (a save of it to another machine under way: it takes its own copy of the text)
    if (lsp_doc_ == d) stop_lsp();
    lsp_forget(*d);  // (its language server lets go of its text)
    if (script_) script_closed(*d);  // (BufDelete, at the next tick)
    minimap_forget(*d);  // (its minimap's pictures let go of)
    wrap_forget(*d);     // (and the soft wrap maps made for it)
    if (d->nb) nb_forget(*d);  // (a notebook's kernel ends)
    build_forget(d->id);  // (a run of a task's: no longer the Build tab's)
}

Pane* App::split_pane(Pane* target, Edge edge) {
    if (!target || edge == kEdgeNone || panes_.size() >= kMaxViews) return nullptr;
    SplitNode* leaf = find_leaf(split_.get(), target);
    if (!leaf) return nullptr;
    if (drag_->kind == kDragDivider) drag_->kind = kDragNone;  // the tree is about to change under it
    Pane* fresh = add_pane();
    attach(leaf, fresh, edge == kEdgeLeft || edge == kEdgeRight, edge == kEdgeLeft || edge == kEdgeTop);
    place_panes(px_w_, px_h_);
    dirty_ = true;
    return fresh;
}

// Cmd+Shift+W: the split with the keyboard goes. Its tabs join the split
// beside it (the one that takes its room), in their order after that
// split's own, the one that was in front staying in front; a new file
// with nothing in it goes with the split. The terminals' split is hidden
// instead (its programs keep running). The last split cannot go: its tabs
// close, each with unsaved changes asked about as Cmd+W asks.
void App::split_close() {
    Pane* const p = pane_;
    if (p->floating) return peek_close();
    if (p == dock_pane()) return dock_hide();
    auto blank = [](const Doc& d) { return d.ed.path.empty() && !d.ed.modified() && d.ed.buf.size() == 0 && !d.term && !d.prof && !d.md && !d.image && !d.pdf && d.title.empty(); };
    const bool last = std::ranges::none_of(panes_, [&](const std::unique_ptr<Pane>& q) { return q.get() != p && q.get() != dock_pane(); });
    if (last) {  // (beside the terminals' split, if that is on show, there is no other)
        for (size_t i = p->docs.size(); i-- > 0;) {
            if (blank(*p->docs[i])) continue;
            if (!tab_close(i)) return;  // (the user said no: the rest stay)
        }
        return;
    }
    SplitNode* parent = nullptr;
    SplitNode* leaf = find_leaf(split_.get(), p, &parent);
    if (!leaf || !parent) return;
    Pane* heir = first_pane(parent->a.get() == leaf ? parent->b.get() : parent->a.get());
    if (heir && heir == dock_pane())  // (not into the terminals)
        for (const std::unique_ptr<Pane>& q : panes_)
            if (q.get() != p && q.get() != dock_pane()) heir = q.get();
    if (!heir || heir == p) return;
    const u64 front = p->doc ? p->doc->id : 0, heir_id = heir->id, heir_front = heir->doc ? heir->doc->id : 0;
    std::vector<u64> blanks;
    for (const std::shared_ptr<Doc>& d : p->docs)
        if (blank(*d)) blanks.push_back(d->id);
    for (size_t n = p->docs.size(); n > 0; --n) move_doc(p, 0, heir, heir->docs.size());  // (the last move lets go of p)
    for (const std::unique_ptr<Pane>& q : panes_) {
        if (q->id != heir_id) continue;
        focus_pane(q.get());
        for (const u64 id : blanks) {
            size_t i = 0;
            Pane* where = nullptr;
            if (pane_->docs.size() > 1 && find_doc(id, &where, &i) && where == pane_) tab_close(i, true);
        }
        // (Its front tab in front; if that was a new file that went, the one that was in front here.)
        const bool kept = std::ranges::any_of(pane_->docs, [&](const std::shared_ptr<Doc>& d) { return d->id == front; });
        for (size_t i = 0; i < pane_->docs.size(); ++i)
            if (pane_->docs[i]->id == (kept ? front : heir_front)) tab_show(i);
        break;
    }
    panel_focus(false);
    pick_theme();
    dirty_ = true;
}

void App::remove_pane(Pane* p) {
    if (!p || panes_.size() < 2) return;
    if (drag_->kind == kDragDivider) drag_->kind = kDragNone;
    for (const std::shared_ptr<Doc>& d : p->docs) {
        if (views_of(d.get()) == 1) forget_doc(d.get());  // (one another split shows stays, with its place there)
        else if (d->placed == p->id) d->placed = 0;
    }
    // Its neighbour takes the room, and the keyboard if this pane had it.
    SplitNode* parent = nullptr;
    SplitNode* leaf = find_leaf(split_.get(), p, &parent);
    Pane* heir = leaf && parent ? first_pane(parent->a.get() == leaf ? parent->b.get() : parent->a.get()) : nullptr;
    detach(split_.get(), p);
    const bool focused = p == pane_;
    std::erase_if(panes_, [p](const std::unique_ptr<Pane>& x) { return x.get() == p; });
    if (!heir || std::none_of(panes_.begin(), panes_.end(), [heir](const auto& x) { return x.get() == heir; })) heir = panes_.front().get();
    if (focused) {
        pane_ = heir;
        place_view(pane_);
        dragging_ = dragging_thumb_ = false;
        lsp_pending_ = true;
        grammars_pending_ = true;
    }
    place_panes(px_w_, px_h_);
    pick_theme();
    dirty_ = true;
}

// Takes a pane out of the arrangement as remove_pane does, but keeps it
// whole, its tabs and what runs in them, for put_pane to bring back.
// `room`: the share of the space it had with its other half.
std::unique_ptr<Pane> App::take_pane(Pane* p, float* room) {
    if (!p || panes_.size() < 2) return nullptr;
    SplitNode* parent = nullptr;
    SplitNode* leaf = find_leaf(split_.get(), p, &parent);
    if (!leaf || !parent) return nullptr;
    if (drag_->kind == kDragDivider) drag_->kind = kDragNone;
    const bool first = parent->a.get() == leaf;
    if (room) *room = first ? parent->share : 1 - parent->share;
    Pane* heir = first_pane(first ? parent->b.get() : parent->a.get());
    detach(split_.get(), p);
    std::unique_ptr<Pane> out;
    for (std::unique_ptr<Pane>& x : panes_)
        if (x.get() == p) out = std::move(x);
    std::erase_if(panes_, [](const std::unique_ptr<Pane>& x) { return !x; });
    if (!heir || std::none_of(panes_.begin(), panes_.end(), [heir](const auto& x) { return x.get() == heir; })) heir = panes_.front().get();
    if (p == pane_) {
        pane_ = heir;
        place_view(pane_);
        dragging_ = dragging_thumb_ = false;
        lsp_pending_ = true;
        grammars_pending_ = true;
        if (pane_->doc) vim_attach();
    }
    place_panes(px_w_, px_h_);
    pick_theme();
    dirty_ = true;
    return out;
}

// Puts a pane take_pane kept back in, on that side of `target`, with
// `room` of the space they share. False: there is no room for another
// split (it stays out).
bool App::put_pane(std::unique_ptr<Pane>& p, Pane* target, Edge edge, float room) {
    if (!p || !target || edge == kEdgeNone || panes_.size() >= kMaxViews) return false;
    SplitNode* leaf = find_leaf(split_.get(), target);
    if (!leaf) return false;
    if (drag_->kind == kDragDivider) drag_->kind = kDragNone;
    Pane* raw = p.get();
    panes_.push_back(std::move(p));
    attach(leaf, raw, edge == kEdgeLeft || edge == kEdgeRight, edge == kEdgeLeft || edge == kEdgeTop);
    raw->cache_rev = ~0ull;
    split_room(raw, room);
    pick_theme();
    return true;
}

void App::split_room(Pane* p, float room) {
    SplitNode* parent = nullptr;
    SplitNode* leaf = find_leaf(split_.get(), p, &parent);
    if (!leaf || !parent) return;
    parent->share = std::clamp(parent->a.get() == leaf ? room : 1 - room, 0.1f, 0.9f);
    place_panes(px_w_, px_h_);
    for (const std::unique_ptr<Pane>& q : panes_) q->cache_rev = ~0ull;
    dirty_ = true;
}

// Moves a tab from one pane to another (or to another place in its own),
// where it comes to the front. A pane left with no tabs goes.
void App::move_doc(Pane* from, size_t index, Pane* to, size_t at) {
    if (!from || !to || index >= from->docs.size()) return;
    stop_lsp(true);  // whichever file it was following may be about to move or step back
    lsp_pending_ = true;
    if (from != to) {
        const Doc* moving = from->docs[index].get();
        for (size_t k = 0; k < to->docs.size(); ++k)
            if (to->docs[k].get() == moving) {  // it shows there already: that tab comes to the front, this one goes
                Pane* const keep = pane_;
                pane_ = from;
                tab_close(index, true);
                const bool still = std::ranges::any_of(panes_, [&](const std::unique_ptr<Pane>& q) { return q.get() == to; });
                if (still) {
                    pane_ = to;
                    for (size_t j = 0; j < to->docs.size(); ++j)
                        if (to->docs[j].get() == moving) tab_show(j);
                }
                const bool kept = std::ranges::any_of(panes_, [&](const std::unique_ptr<Pane>& q) { return q.get() == keep; });
                pane_ = kept ? keep : (still ? to : panes_.front().get());
                place_view(pane_);
                pick_theme();
                dirty_ = true;
                return;
            }
        // (Its place goes with it.)
        if (const auto it = from->places.find(moving->id); it != from->places.end()) {
            to->places[moving->id] = it->second;
            from->places.erase(it);
        }
        if (moving->placed == from->id) const_cast<Doc*>(moving)->placed = to->id;
    }
    std::shared_ptr<Doc> d = std::move(from->docs[index]);
    const bool was_front = from->doc == d.get();
    if (was_front) {  // its view goes with it
        d->scroll_x = from->target_x;
        d->scroll_y = from->target_y;
    }
    from->docs.erase(from->docs.begin() + static_cast<std::ptrdiff_t>(index));
    if (from == to && at > index) --at;
    auto bring_front = [](Pane* p, size_t i) {
        p->doc = p->docs[i].get();
        p->tab = i;
        p->scroll_x = p->target_x = p->doc->scroll_x;
        p->scroll_y = p->target_y = p->doc->scroll_y;
        p->cache_rev = ~0ull;
        p->bar->reveal = true;
    };
    if (from->docs.empty()) {
        from->doc = nullptr;
    } else if (was_front) {
        bring_front(from, std::min(index, from->docs.size() - 1));
    } else if (index < from->tab) {
        --from->tab;
    }
    if (to->doc) {
        to->doc->scroll_x = to->target_x;
        to->doc->scroll_y = to->target_y;
    }
    at = std::min(at, to->docs.size());
    to->docs.insert(to->docs.begin() + static_cast<std::ptrdiff_t>(at), std::move(d));
    bring_front(to, at);
    if (from->docs.empty() && from != to) remove_pane(from);
    pick_theme();
    dirty_ = true;
}

// The pane next to the focused one on that side gets the keyboard.
void App::focus_towards(Edge edge) {
    const Pane& p = *pane_;
    const int cx = (p.x0 + p.x1) / 2, cy = (p.y0 + p.y1) / 2;
    Pane* next = edge == kEdgeLeft    ? pane_at(p.x0 - 4, cy)
                 : edge == kEdgeRight ? pane_at(p.x1 + 4, cy)
                 : edge == kEdgeTop   ? pane_at(cx, p.y0 - 4)
                                      : pane_at(cx, p.y1 + 4);
    if (next) {
        focus_pane(next);
        panel_focus(false);
    }
}

// ---------------------------------------------------------------- input

bool App::splits_key(const SDL_KeyboardEvent& k) {
    const bool cmd = k.mod & kPrimaryMod, shift = k.mod & SDL_KMOD_SHIFT, alt = k.mod & SDL_KMOD_ALT;
    if (k.key == SDLK_ESCAPE && drag_->kind != kDragNone && drag_->moving) {  // changed my mind: put it back
        drag_->kind = kDragNone;
        drag_->moving = drag_->away = false;
        drag_->over = 0;
        for (const std::unique_ptr<Pane>& p : panes_) p->bar->drop = -1, p->bar->pressed = p->bar->dragging = false;
        dirty_ = true;
        return true;
    }
    if (cmd && k.key == SDLK_BACKSLASH) {  // a new split to the right (with Shift: below), and Go to file for what goes in it
        if (Pane* fresh = split_pane(pane_, shift ? kEdgeBottom : kEdgeRight)) {
            focus_pane(fresh);
            add_doc(0);  // (what Esc leaves it with: a new file)
            tab_show(0);
            panel_focus(false);
            find_for_split(fresh);
        }
        return true;
    }
    if (cmd && shift && !alt && k.key == SDLK_W) {
        split_close();
        return true;
    }
    if (cmd && alt) {
        switch (k.key) {
            case SDLK_LEFT: focus_towards(kEdgeLeft); return true;
            case SDLK_RIGHT: focus_towards(kEdgeRight); return true;
            case SDLK_UP: focus_towards(kEdgeTop); return true;
            case SDLK_DOWN: focus_towards(kEdgeBottom); return true;
            default: break;
        }
    }
    return false;
}

void App::drag_arm(DragKind kind, u64 doc, const std::string& path, float x, float y) {
    Drag& d = *drag_;
    d.kind = kind;
    d.moving = d.away = d.on_bar = false;
    d.doc = doc;
    d.pane = pane_->id;
    d.path = path;
    d.press_x = d.x = x;
    d.press_y = d.y = y;
    d.over = 0;
    d.edge = kEdgeNone;
    d.ghost_font = ~0u;  // the label is drawn afresh
    d.label.clear();
    if (kind == kDragTab) {
        if (const Doc* held = find_doc(doc)) d.label = held->ed.path.empty() ? "untitled" : std::string(file_name(held->ed.path));
    } else if (kind == kDragGroup) {
        d.label = std::to_string(pane_->docs.size()) + (pane_->docs.size() == 1 ? " tab" : " tabs");
    } else if (kind == kDragFile) {
        d.label = std::string(file_name(path));
    }
}

// A press on a divider takes hold of it.
bool App::splits_mouse_down(const SDL_MouseButtonEvent& b) {
    if (b.button != SDL_BUTTON_LEFT) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    SplitNode* n = divider_at(split_.get(), px, py, std::max(3, static_cast<int>(3 * scale_)));
    if (!n) return false;
    Drag& d = *drag_;
    d.kind = kDragDivider;
    d.moving = d.away = false;
    d.over = 0;
    d.node = n;
    return true;
}

bool App::splits_mouse_motion(const SDL_MouseMotionEvent& m) {
    Drag& d = *drag_;
    if (d.kind != kDragNone && !(m.state & SDL_BUTTON_LMASK)) drag_cancel();  // (the button went up where this window did not see it)
    const int px = static_cast<int>(m.x * density_), py = static_cast<int>(m.y * density_);
    if (d.kind == kDragNone) {
        // Over a divider the pointer says which way it moves.
        if (win_ && !dragging_ && !dragging_thumb_) {
            const SplitNode* n = divider_at(split_.get(), px, py, std::max(3, static_cast<int>(3 * scale_)));
            const int want = !n ? 0 : n->side_by_side ? 1 : 2;
            if (want != d.cursor) {
                if (!d.across) d.across = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
                if (!d.updown) d.updown = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
                SDL_SetCursor(want == 1 ? d.across : want == 2 ? d.updown : SDL_GetDefaultCursor());
                d.cursor = want;
            }
        }
        return false;
    }
    d.x = m.x;
    d.y = m.y;
    if (d.kind == kDragDivider) {
        SplitNode& n = *d.node;
        const float size = static_cast<float>(n.side_by_side ? n.x1 - n.x0 : n.y1 - n.y0);
        const float at = static_cast<float>(n.side_by_side ? px - n.x0 : py - n.y0);
        if (size > 0) n.share = std::clamp(at / size, 0.05f, 0.95f);
        for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
        dirty_ = true;
        return true;
    }
    if (!d.moving) {
        if (std::hypot(m.x - d.press_x, m.y - d.press_y) < 5) return false;
        d.moving = true;
    }
    // Still where it was picked up? A tab inside its own bar is being put
    // in order there (see tabs_mouse_motion); a file over the side panel is
    // going nowhere yet.
    auto nowhere = [&] {
        d.away = false;
        d.over = 0;
        for (const std::unique_ptr<Pane>& p : panes_) p->bar->drop = -1;
        dirty_ = true;
    };
    Pane* src = nullptr;
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->id == d.pane) src = p.get();
    if (d.kind == kDragFile) {
        if (px < panel_px()) {
            nowhere();
            return true;
        }
    } else {
        const int slack = font_->cell_h() / 2;
        if (!src) {  // its pane has gone meanwhile
            d.kind = kDragNone;
            nowhere();
            return true;
        }
        if (px >= src->x0 && px < src->x1 && py >= src->y0 - slack && py < src->text_y + slack) {
            nowhere();
            return d.kind == kDragGroup;
        }
    }
    d.away = true;

    // Where would it land?
    d.over = 0;
    d.edge = kEdgeNone;
    d.on_bar = false;
    if (Pane* over = pane_at(px, py)) {
        d.over = over->id;
        if (py < over->text_y) {  // among that pane's tabs: before the one whose middle is right of the mouse
            const int cw = font_->cell_w();
            const double col = static_cast<double>(px - over->x0 - look().edge) / cw + over->bar->scroll;
            d.on_bar = true;
            d.bar_index = 0;
            for (const TabBar::Span& s : over->bar->spans)
                if (col >= (s.x0 + s.x1) * 0.5) ++d.bar_index;
        } else {
            const float rx = static_cast<float>(px - over->x0) / static_cast<float>(std::max(1, over->x1 - over->x0));
            const float ry = static_cast<float>(py - over->text_y) / static_cast<float>(std::max(1, over->y1 - over->text_y));
            const float near[4] = {rx, 1 - rx, ry, 1 - ry};  // left, right, top, bottom
            int best = 0;
            for (int i = 1; i < 4; ++i)
                if (near[i] < near[best]) best = i;
            if (near[best] < kEdgeZone) d.edge = static_cast<Edge>(best);
        }
        // A new split needs room for one more pane, unless what is carried
        // is a pane already (a group, or a pane's only tab).
        const bool whole_pane = d.kind == kDragGroup || (d.kind == kDragTab && src->docs.size() == 1);
        if (d.edge != kEdgeNone && !whole_pane && panes_.size() >= kMaxViews) d.edge = kEdgeNone;
        // Letting go where nothing would change is letting go nowhere.
        if (d.kind != kDragFile && over == src && (d.edge == kEdgeNone || whole_pane)) d.over = 0;
    }
    for (const std::unique_ptr<Pane>& p : panes_) p->bar->drop = d.over == p->id && d.on_bar ? static_cast<int>(d.bar_index) : -1;
    dirty_ = true;
    return true;
}

bool App::splits_mouse_up(const SDL_MouseButtonEvent& b) {
    Drag& d = *drag_;
    if (b.button != SDL_BUTTON_LEFT || d.kind == kDragNone) return false;
    const bool was_drag = d.kind == kDragDivider || d.moving;
    if (d.kind == kDragFile && !d.moving) {
        // A plain click on a file in the tree: open it.
        const std::string path = d.path;
        d.kind = kDragNone;
        panel_activate(path);
        return true;
    }
    if (d.kind == kDragFile && d.moving && panel_drop(d.path, b.x, b.y)) {  // (let go over the tree: moved into a folder)
        d.kind = kDragNone;
        d.moving = d.away = false;
        d.over = 0;
        return true;
    }
    if (d.kind != kDragDivider && d.moving && d.away && d.over) drag_drop();
    d.kind = kDragNone;
    d.moving = d.away = false;
    d.over = 0;
    for (const std::unique_ptr<Pane>& p : panes_) p->bar->drop = -1;
    dirty_ = true;
    return was_drag;
}

// Lets go of every drag with nothing dropped: the mouse-up was lost, or the
// window lost the keyboard in the middle of one.
void App::drag_cancel() {
    Drag& d = *drag_;
    if (d.kind != kDragNone) {
        d.kind = kDragNone;
        d.moving = d.away = false;
        d.over = 0;
        for (const std::unique_ptr<Pane>& p : panes_) p->bar->drop = -1;
    }
    panel_mouse_up();  // (a resize of the side panel too)
    dragging_ = dragging_thumb_ = false;
    dirty_ = true;
}

// Lets go of what is being carried, where the last mouse movement said.
void App::drag_drop() {
    const Drag d = *drag_;  // (what follows may clear the real one)
    Pane* target = nullptr;
    Pane* src = nullptr;
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (p->id == d.over) target = p.get();
        if (p->id == d.pane) src = p.get();
    }
    if (!target) return;
    const bool side_by_side = d.edge == kEdgeLeft || d.edge == kEdgeRight, first = d.edge == kEdgeLeft || d.edge == kEdgeTop;
    // Takes a whole pane to an edge of another: out of the tree, back in there.
    auto move_pane = [&](Pane* p) {
        if (p == target) return;
        stop_lsp(true);
        lsp_pending_ = true;
        detach(split_.get(), p);
        attach(find_leaf(split_.get(), target), p, side_by_side, first);
        place_panes(px_w_, px_h_);
    };
    switch (d.kind) {
        case kDragTab: {
            size_t index = 0;
            Pane* where = nullptr;
            if (!src || !find_doc(d.doc, &where, &index) || where != src) return;
            if (d.edge != kEdgeNone && src->docs.size() == 1) {
                move_pane(src);  // a pane's only tab is the pane
                focus_pane(src);
            } else if (d.edge != kEdgeNone) {
                Pane* fresh = split_pane(target, d.edge);
                if (!fresh) return;
                move_doc(src, index, fresh, 0);
                focus_pane(fresh);
            } else if (target != src) {
                move_doc(src, index, target, d.on_bar ? d.bar_index : target->docs.size());
                focus_pane(target);
            }
            break;
        }
        case kDragGroup: {
            if (!src || src == target) return;
            if (d.edge != kEdgeNone) {
                move_pane(src);
                focus_pane(src);
            } else {
                // Into the other pane's tabs, in their order, the one that was in front staying in front.
                const u64 front = src->doc ? src->doc->id : 0;
                size_t at = d.on_bar ? std::min(d.bar_index, target->docs.size()) : target->docs.size();
                const u64 target_id = target->id;
                for (size_t n = src->docs.size(); n > 0; --n) move_doc(src, 0, target, at++);  // (the last move lets go of src)
                for (const std::unique_ptr<Pane>& p : panes_)
                    if (p->id == target_id) {
                        focus_pane(p.get());
                        for (size_t i = 0; i < p->docs.size(); ++i)
                            if (p->docs[i]->id == front) tab_show(i);
                    }
            }
            break;
        }
        case kDragFile: {
            if (fs_kind(d.path) == 2) break;  // (a folder of the tree: nothing to open in a split)
            Pane* dest = d.edge != kEdgeNone ? split_pane(target, d.edge) : target;
            if (!dest) dest = target;
            Pane* where = nullptr;
            size_t index = 0;
            const std::string full = full_path(d.path);
            const auto in_dest = std::ranges::find_if(dest->docs, [&](const std::shared_ptr<Doc>& x) { return !x->ed.path.empty() && full_path(x->ed.path) == full; });
            if (in_dest != dest->docs.end()) {  // shown there already: to its tab
                focus_pane(dest);
                tab_show(static_cast<size_t>(in_dest - dest->docs.begin()));
            } else if (find_open(full, &where, &index) && shareable(*where->docs[index])) {  // another split's: shown here too
                focus_pane(dest);
                if (dest->docs.empty()) {
                    add_doc(0);
                    tab_show(0);
                }
                show_also(where, index);
            } else if (find_open(full, &where, &index)) {  // (a picture, a PDF: its tab moves here)
                move_doc(where, index, dest, dest->docs.size());
                focus_pane(dest);
            } else {
                focus_pane(dest);
                if (dest->docs.empty()) {  // a new split: a new file, which the one opened takes the place of
                    add_doc(0);
                    tab_show(0);
                }
                open_path(d.path.c_str());
            }
            panel_focus(false);
            break;
        }
        default: break;
    }
    pick_theme();
    dirty_ = true;
}

// ---------------------------------------------------------------- drawing

// Adds to a frame the lines between panes (a hairline in the colour of a
// box's outline) and, while something is being carried, a wash over where
// it would land and its name under the mouse.
void App::splits_layout(int w, int h, FrameDesc& fd) {
    (void)w;
    (void)h;
    Drag& d = *drag_;
    const int line = look().hair;
    auto divider = [&](auto&& self, const SplitNode* n) -> void {
        if (!n || n->pane) return;
        if (fd.popup_count < std::size(fd.popups)) {
            Popup& p = fd.popups[fd.popup_count++];
            p.x = n->side_by_side ? n->b->x0 : n->x0;
            p.y = n->side_by_side ? n->y0 : n->b->y0;
            p.w = n->side_by_side ? line : n->x1 - n->x0;
            p.h = n->side_by_side ? n->y1 - n->y0 : line;
            p.cols = p.rows = 1;
            p.pad_x = p.w;  // no cell shows: the box is all margin, in the border's colour
            p.cells = &kBlankCell;
            p.border_px = 0;
            p.bg = theme_.popup_border;
        }
        self(self, n->a.get());
        self(self, n->b.get());
    };
    divider(divider, split_.get());

    if (d.kind == kDragNone || d.kind == kDragDivider || !d.moving || !d.away) return;
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (p->id != d.over || d.on_bar || p->view >= fd.view_count) continue;
        float* t = fd.views[p->view].tint;
        // (Over the text, not the line numbers: the wash is drawn with the text, and its corners are round.)
        const float x0 = static_cast<float>(p->text_x), x1 = static_cast<float>(p->x1), y0 = static_cast<float>(p->text_y), y1 = static_cast<float>(p->y1);
        const float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        t[0] = d.edge == kEdgeRight ? mx : x0;
        t[1] = d.edge == kEdgeBottom ? my : y0;
        t[2] = d.edge == kEdgeLeft ? mx : x1;
        t[3] = d.edge == kEdgeTop ? my : y1;
    }
}

// The name of what is being carried, by the mouse: a small box that floats
// over everything else, so it is added after everything else.
void App::splits_carry_layout(int w, int h, FrameDesc& fd) {
    (void)w;
    (void)h;
    Drag& d = *drag_;
    if (d.kind == kDragNone || d.kind == kDragDivider || !d.moving || !d.away) return;
    if (!d.label.empty() && fd.popup_count < std::size(fd.popups)) {
        if (d.ghost_font != font_->generation()) {
            d.ghost_font = font_->generation();
            d.ghost.resize(width_of(d.label), 1);
            put(d.ghost, *font_, 0, 0, d.label, ink::kStrong);
        }
        const int cw = font_->cell_w(), ch = font_->cell_h();
        const Look l = look();
        Popup& p = fd.popups[fd.popup_count++];
        p.pad_x = l.edge;
        p.pad_y = l.pad_y;
        p.cols = d.ghost.cols;
        p.rows = 1;
        p.w = static_cast<int>(p.cols) * cw + 2 * p.pad_x;
        p.h = ch + 2 * p.pad_y;
        p.x = std::clamp(static_cast<int>(d.x * density_) + cw, 0, std::max(0, px_w_ - p.w));
        p.y = std::clamp(static_cast<int>(d.y * density_) + ch / 2, 0, std::max(0, px_h_ - p.h));
        p.cells = d.ghost.cells.data();
        p.border_px = static_cast<float>(l.hair);
        fx_float(p, kFxGhost);
        if (l.box > 0) p.opacity *= 0.94f;  // (what it is over shows through a little)
    }
}

}  // namespace blit
