// Modal editing in the application: keys and typed text on their way to Vim
// (vim.cpp), what Vim asks for in return (save, close, another split), the
// line at the bottom of the window that shows the command being typed and
// Vim's messages (and the mode, when there is no status bar to show it),
// and the search matches lit up in the text.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.hpp"
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

// Ctrl with a letter: Vim's, or the application's? On a Mac the
// application's keys are Cmd's, and every Ctrl key Vim knows is Vim's.
// Where Ctrl is the application's key too (kCtrlShared) they are still
// Vim's while the text has the keyboard (vim_owns_key()), but for Ctrl+N
// and Ctrl+P outside Insert mode, which there are only j and k by other
// names: those stay a new file and Go to file.
bool vim_takes_ctrl(SDL_Keycode key, bool insert) {
    if (key == SDLK_LEFTBRACKET) return true;
    if (key == SDLK_RIGHTBRACKET) return !insert;
    if (key < SDLK_A || key > SDLK_Z) return false;
    const char c = static_cast<char>('a' + (key - SDLK_A));
    if (kCtrlShared && !insert && (c == 'n' || c == 'p')) return false;
    return std::strchr(insert ? "wuhrojmnpctdavqey" : "rdufbeyowaxvhjmnpciltk", c) != nullptr;
}

// What makes a comment of a line in a language (Neovim's 'commentstring'
// for it), for gc. Nothing for a language without comments, or one not known.
void comment_marks(const std::string& lang, std::string* left, std::string* right) {
    static const struct {
        const char* left;
        const char* right;
        const char* langs;  // names between spaces
    } table[] = {
        {"// ", "", " c cpp cuda rust go zig java kotlin swift c_sharp javascript typescript tsx json5 scss php glsl hlsl systemverilog "},
        {"# ", "", " python ruby bash toml yaml cmake make graphql "},
        {"-- ", "", " lua sql "},
        {"<!-- ", " -->", " html xml markdown vue svelte astro dtd "},
        {"/* ", " */", " css "},
        {"\" ", "", " vim "},
        {"; ", "", " query "},
    };
    left->clear();
    right->clear();
    if (lang.empty()) return;
    const std::string name = " " + lang + " ";
    for (const auto& t : table)
        if (std::strstr(t.langs, name.c_str())) {
            *left = t.left;
            *right = t.right;
            return;
        }
}

}  // namespace

struct App::VimUi {
    // The line at the bottom, and what it was drawn from.
    TextBox line;
    std::string status, right;
    int cursor = -2;
    bool selected = false;  // (the command line's text is selected)
    u32 font = ~0u;
    // The file the comment markers were looked up for.
    u64 comment_doc = ~0ull;
    std::string comment_path;
    // Search matches on screen and Visual Block mode's rectangle, with
    // whatever else underlines the text.
    std::vector<GridDeco> marks, block, all;
    u64 marks_key = 0;
    // A pattern that took the backtracker past its budget while lighting
    // its matches: not looked for again on every frame.
    std::shared_ptr<const VimRegex> marks_gave_up;
};

void App::vim_create() {
    vimui_ = std::make_shared<VimUi>();
    vim_.clipboard_get = [] {
        char* t = SDL_GetClipboardText();
        std::string s = t ? t : "";
        SDL_free(t);
        return s;
    };
    vim_.clipboard_set = [](const std::string& s) { SDL_SetClipboardText(s.c_str()); };
}

// Vim works on the document in front. When that has become another one,
// the old one is left tidily (Insert mode ends there) and the new one is
// taken up in Normal mode. Also tells Vim what it needs to know of the view.
void App::vim_attach() {
    if (!cfg_.modal || !font_) return;
    Doc& d = *pane_->doc;
    if (vim_doc_ != d.id || vim_pane_ != pane_->id) {
        if (Doc* old = find_doc(vim_doc_); old && old != &d) {
            vim_.suspend(old->ed);
            old->ed.watch = nullptr;
        } else if (old == &d) {  // (the same file, in another split: left, and taken up there)
            vim_.suspend(d.ed);
        } else {
            vim_.forget();
        }
        vim_doc_ = d.id;
        vim_pane_ = pane_->id;
        vim_.buffer_id = d.id;  // (before resume(): the places kept for this document come back)
        vim_.resume(d.ed);
        d.ed.watch = &vim_;     // marks and the like move with the text
        pane_->cache_rev = ~0ull;
    }
    vim_.buffer_id = d.id;
    vim_.read_only = d.read_only;
    const u64 ch = static_cast<u64>(font_->cell_h());
    vim_.view_top = static_cast<u64>(std::max(0.0, pane_->target_y)) / ch;
    vim_.view_rows = static_cast<u32>(std::max(1, visible_rows()));
    const u32 step = indent_step();
    vim_.shift = d.indent_tabs ? std::string("\t") : std::string(step, ' ');
    VimUi& u = *vimui_;
    if (u.comment_doc != d.id || u.comment_path != d.ed.path) {
        u.comment_doc = d.id;
        u.comment_path = d.ed.path;
        comment_marks(file_language(), &vim_.comment_left, &vim_.comment_right);
    }
}

bool App::vim_selection(u64* lo, u64* hi) const {
    // (Visual Block mode's rectangle is no run of text: vim_marks() draws it.)
    if (!cfg_.modal || !vim_.visual() || vim_.mode() == Vim::kVisualBlock || pane_->doc->id != vim_doc_ || pane_->id != vim_pane_) return false;
    vim_.visual_range(pane_->doc->ed, lo, hi);
    return true;
}

// Where Ctrl is the editor's key and Vim's alike: whether this one is
// Vim's before any of the editor's shortcuts look at it. It is when modes
// are on, the text has the keyboard (not a box, the side panel or a tab of
// another kind), and Vim has a use for it in the mode it is in. A command
// run from the command search is the editor's whatever its keys, and so
// are Ctrl+N and Ctrl+P while a completion list is open (they go through
// it, as Vim's would through its own).
bool App::vim_owns_key(const SDL_KeyboardEvent& k) {
    if (!kCtrlShared || !cfg_.modal || command_sending_) return false;
    if (!(k.mod & SDL_KMOD_CTRL) || (k.mod & (SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI))) return false;
    if (find_open_ || panel_has_keyboard()) return false;
    const Doc& d = *pane_->doc;
    if (d.term || d.prof || d.md || d.image || d.pdf) return false;
    if ((k.key == SDLK_N || k.key == SDLK_P) && lsp_completing()) return false;
    vim_attach();
    return vim_takes_ctrl(k.key, vim_.mode() == Vim::kInsert || vim_.mode() == Vim::kReplace);
}

// A key press. True if it was Vim's (or is nobody's: outside Insert mode a
// key Vim has no use for does nothing).
bool App::vim_key(const SDL_KeyboardEvent& k) {
    if (!cfg_.modal || (k.mod & SDL_KMOD_GUI)) return false;
    vim_attach();
    const bool ctrl = k.mod & SDL_KMOD_CTRL, alt = k.mod & SDL_KMOD_ALT, shift = k.mod & SDL_KMOD_SHIFT;
    const bool insert = vim_.mode() == Vim::kInsert || vim_.mode() == Vim::kReplace;
    VimKey v;
    bool moving = false;
    switch (k.key) {
        case SDLK_ESCAPE: v.kind = VimKey::kEsc; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: v.kind = VimKey::kEnter; break;
        case SDLK_BACKSPACE: v.kind = VimKey::kBackspace, moving = true; break;
        case SDLK_TAB:
            if (ctrl) return false;
            v.kind = VimKey::kTab;
            break;
        case SDLK_LEFT: v.kind = VimKey::kLeft, moving = true; break;
        case SDLK_RIGHT: v.kind = VimKey::kRight, moving = true; break;
        case SDLK_UP: v.kind = VimKey::kUp, moving = true; break;
        case SDLK_DOWN: v.kind = VimKey::kDown, moving = true; break;
        case SDLK_HOME: v.kind = VimKey::kHome, moving = true; break;
        case SDLK_END: v.kind = VimKey::kEnd, moving = true; break;
        case SDLK_PAGEUP: v.kind = VimKey::kPageUp; break;
        case SDLK_PAGEDOWN: v.kind = VimKey::kPageDown; break;
        case SDLK_DELETE: v.kind = VimKey::kDelete, moving = true; break;
        default:
            if (ctrl && !alt && vim_takes_ctrl(k.key, insert)) {
                v.ch = k.key == SDLK_LEFTBRACKET ? '[' : k.key == SDLK_RIGHTBRACKET ? ']' : static_cast<u32>('a' + (k.key - SDLK_A));
                v.ctrl = true;
                break;
            }
            // A character: it arrives as typed text. Other keys are not Vim's.
            return !insert && !ctrl && !alt && k.key < 0x40000000;
    }
    // In Insert mode, Shift and Option with the arrows select and move by
    // words, as they do without Vim.
    if (insert && moving && (shift || alt)) return false;
    // Ctrl+Home and Ctrl+End, the document's ends where Ctrl is the editor's
    // key: Vim's gg and G outside Insert mode (as Vim has them), the
    // editor's in it.
    if (kCtrlShared && ctrl && (k.key == SDLK_HOME || k.key == SDLK_END)) {
        if (insert) return false;
        if (!shift && !alt) {
            vim_.keys(pane_->doc->ed, k.key == SDLK_HOME ? "gg" : "G");
            vim_after(Vim::kHandled);
        }
        return true;
    }
    if (!insert) lsp_close(kPopHover);
    const u32 r = vim_.feed(pane_->doc->ed, v);
    vim_after(r);
    if (insert && r) lsp_after_key(k.key);
    return r != 0 || !insert;
}

// Typed text: in Insert mode it goes into the document, elsewhere each
// character is a command.
void App::vim_text(const char* text) {
    vim_attach();
    if (vim_.mode() != Vim::kInsert && vim_.mode() != Vim::kReplace) lsp_close(kPopHover);
    const u8* p = reinterpret_cast<const u8*>(text);
    const size_t n = std::strlen(text);
    u32 all = 0;
    std::string typed;
    for (size_t i = 0; i < n;) {
        const Decoded d = decode_utf8(p + i, n - i);
        i += d.len;
        VimKey v;
        v.ch = d.cp;
        const u32 r = vim_.feed(pane_->doc->ed, v);
        all |= r;
        if (r & Vim::kTyped) typed += vim_.typed();
        if (script_modes_) script_mode_seen();  // (each change of mode, for the scripts' handlers)
    }
    vim_after(all);
    if (!typed.empty()) lsp_typed(typed);
}

// After Vim has had keys: carry out what it asked for, follow the cursor.
void App::vim_after(u32 result) {
    (void)result;
    if (vim_.mode() != Vim::kInsert && vim_.mode() != Vim::kReplace) lsp_close(kPopCompletion | kPopSignature);
    pane_->cache_rev = ~0ull;  // (the selection may have changed with the cursor where it was)
    bool scrolled = false;
    for (const VimRequest& q : vim_.take()) {
        switch (q.kind) {
            case VimRequest::kSave:
                if (!q.arg.empty()) {  // :w NAME, from the project folder when it is not a whole path
                    std::string path = q.arg;
                    if (path[0] == '~' && SDL_getenv("HOME")) path = std::string(SDL_getenv("HOME")) + path.substr(1);
                    if (path[0] != '/' && !is_remote(path)) {
                        const std::string* root = panel_root();
                        path = (root ? *root : default_workdir()) + "/" + path;
                    }
                    save_named(path, pane_->doc->id);
                    break;
                }
                save();
                if (q.then_close && (!pane_->doc->ed.modified() || remote_saving(pane_->doc->id))) tab_close(pane_->tab);  // (another machine's: once written)
                break;
            case VimRequest::kSaveAll:
                save_all();
                if (q.then_close) request_quit();
                break;
            case VimRequest::kClose: tab_close(pane_->tab, q.force, true); break;  // (:q in the last window quits, as in Vim)
            case VimRequest::kQuitAll:
                if (q.force) running_ = false;
                else request_quit();
                break;
            case VimRequest::kOpen: {
                // A name that is not a whole path is taken from the working directory.
                std::string path = q.arg;
                if (!path.empty() && path[0] == '~' && SDL_getenv("HOME")) path = std::string(SDL_getenv("HOME")) + path.substr(1);
                if (!path.empty() && path[0] != '/') {
                    const std::string* root = panel_root();
                    path = (root ? *root : default_workdir()) + "/" + path;
                }
                open_path(path.c_str());
                break;
            }
            case VimRequest::kTab:
                if (q.arg == "first") tab_show(0);
                else if (q.arg == "last") tab_show(pane_->docs.size() - 1);
                else if (q.n == 0) tab_new();
                else tab_step(static_cast<int>(q.n % static_cast<i64>(pane_->docs.size())));
                break;
            case VimRequest::kDiagnostic: {
                // To the diagnostic after the cursor (or before it), going
                // round at the ends of the file as Neovim's ]d and [d do.
                std::vector<u64> at;
                if (lsp_doc_ == pane_->doc)
                    for (const Diagnostic& d : lsp_.diagnostics()) at.push_back(d.start);
                if (at.empty()) {
                    vim_.say("No diagnostics in this file");
                    break;
                }
                std::sort(at.begin(), at.end());
                at.erase(std::unique(at.begin(), at.end()), at.end());
                const u64 cur = pane_->doc->ed.cursor();
                const i64 count = static_cast<i64>(at.size());
                i64 i;
                if (q.arg == "first") i = 0;
                else if (q.arg == "last") i = count - 1;
                else if (q.n > 0) i = (std::upper_bound(at.begin(), at.end(), cur) - at.begin()) + (q.n - 1);
                else i = (std::lower_bound(at.begin(), at.end(), cur) - at.begin()) + q.n;
                vim_.jump(pane_->doc->ed, at[static_cast<size_t>(((i % count) + count) % count)]);
                break;
            }
            case VimRequest::kSplit:
                if (Pane* fresh = split_pane(pane_, q.n == 0 ? kEdgeRight : kEdgeBottom)) {
                    focus_pane(fresh);
                    add_doc(0);
                    tab_show(0);
                    if (!q.arg.empty()) open_path(q.arg.c_str());
                }
                break;
            case VimRequest::kFocus:
                if (q.n < 4) {
                    focus_towards(static_cast<Edge>(q.n == 0 ? kEdgeLeft : q.n == 1 ? kEdgeRight : q.n == 2 ? kEdgeTop : kEdgeBottom));
                } else {  // the next split, round and round
                    for (size_t i = 0; i < panes_.size(); ++i)
                        if (panes_[i].get() == pane_) {
                            focus_pane(panes_[(i + 1) % panes_.size()].get());
                            break;
                        }
                }
                break;
            case VimRequest::kScroll: {
                const double ch = font_->cell_h();
                pane_->target_y = std::clamp(static_cast<double>(q.n) * ch, 0.0, std::max(max_scroll_y(), 0.0));
                if (!animating_) last_step_ns_ = now_ns() - 8'000'000;
                animating_ = true;
                scrolled = true;
                break;
            }
            case VimRequest::kFind: find_open(q.n == 0 ? kFindNames : kFindText, q.arg); break;
            case VimRequest::kEx: script_ex(q); break;  // :lua, :set, :map, a script's command (applua.cpp)
            case VimRequest::kDefinition:  // (asked for, as F12 asks: the jump comes with the answer)
                if (lsp_.ready()) lsp_.definition(pane_->doc->ed.cursor());
                else goto_definition();  // (which says why there is nobody to ask)
                break;
            case VimRequest::kHover: show_hover(pane_->doc->ed.cursor(), true); break;
            case VimRequest::kReferences: lsp_references(); break;
            case VimRequest::kFormat: lsp_format_lines(q.line0, q.line1); break;
            case VimRequest::kBack: jump_walk(q.n ? q.n : -1); break;  // (<C-o> and <C-i>: the jumps across files, app.cpp)
            default: break;
        }
    }
    vim_attach();  // (what was asked for may have brought another document to the front)
    if (!scrolled) scroll_to_cursor();
    if (script_modes_) script_mode_seen();
    dirty_ = true;
}

// After the mouse has put the cursor somewhere or dragged out a selection.
void App::vim_mouse() {
    if (!cfg_.modal) return;
    vim_attach();
    vim_.adopt_selection(pane_->doc->ed);
    pane_->cache_rev = ~0ull;
    dirty_ = true;
}

// ---------------------------------------------------------------- drawing

int App::vim_px() const {
    if (!cfg_.modal || !font_) return 0;
    return font_->cell_h() + font_->cell_h() / 4;
}

// The line at the bottom of the window, just above the status bar and
// coloured as it is, so that the two read as one: the command line or a
// message on the left (and the mode, when there is no status bar to show
// it), the keys of a command under way and the cursor's place on the right.
void App::vim_layout(int w, int h, FrameDesc& fd) {
    const int tall = vim_px();
    if (!tall || fd.popup_count >= std::size(fd.popups)) return;
    VimUi& u = *vimui_;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    const int x = std::min(panel_px(), w), width = std::max(1, w - x), pad_x = look().edge;
    const u32 cols = static_cast<u32>(std::max(1, (width - 2 * pad_x) / cw));
    const bool command = vim_.mode() == Vim::kCommand;
    std::string status = vim_.status();
    // The status bar names the mode: "-- INSERT --" is not said again here
    // (what else there is, "recording @q", stays).
    if (status_px() && !command && vim_.message().empty() && status.starts_with("-- ")) {
        const size_t end = status.find(" --", 3);
        if (end != std::string::npos) status.erase(0, status.find_first_not_of(' ', end + 3));
    }
    std::string right = vim_.showcmd();
    // (With the status bar, the cursor's place and a script's text are there.)
    if (!status_px()) {
        right.resize(std::max<size_t>(right.size(), 10), ' ');
        if (!script_status_.empty()) right = script_status_ + "   " + right;  // (blit.ui.status)
        right += std::to_string(pane_->cur_line + 1) + "," + std::to_string(pane_->cur_col + 1);
    }
    const int cursor = vim_.status_cursor();
    const bool selected = vim_.command_selected();
    if (status != u.status || right != u.right || cursor != u.cursor || selected != u.selected || u.line.cols != cols || u.font != font_->generation()) {
        u.status = status;
        u.right = right;
        u.cursor = cursor;
        u.selected = selected;
        u.font = font_->generation();
        u.line.resize(cols, 1);
        Font& font = *font_;
        const std::string_view said(status);
        if (command && !said.empty()) {
            // The command line: what kind it is (":", "/" or "?") in the accent, what is typed plain.
            put(u.line, font, 0, put(u.line, font, 0, 0, said.substr(0, 1), ink::kAccent), said.substr(1), ink::kText);
        } else {
            const bool error = status.size() > 2 && status[0] == 'E' && std::isdigit(static_cast<unsigned char>(status[1]));
            const bool mode = status.starts_with("-- ");
            put(u.line, font, 0, 0, said, error ? ink::kError : mode ? ink::kStrong : ink::kText);
        }
        const u32 need = width_of(right);
        if (cols > need + width_of(status) + 2) put(u.line, font, 0, cols - need, right, ink::kMuted);
        if (selected) {  // what Cmd+F put there, lit as a selection is: typing takes its place
            for (u32 k = 1; k < static_cast<u32>(std::max(cursor, 1)) && k < cols; ++k) u.line.cells[k] |= kCellSelected;
        } else if (cursor >= 0 && static_cast<u32>(cursor) < cols) {
            u.line.cells[static_cast<size_t>(cursor)] |= kCellSelected;
        }
    }
    Popup& p = fd.popups[fd.popup_count++];
    p.x = x;
    p.y = h - status_px() - tall;
    p.w = width;
    p.h = tall;
    p.pad_x = pad_x;
    p.pad_y = (tall - ch) / 2;
    p.cols = u.line.cols;
    p.rows = 1;
    p.cells = u.line.cells.data();
    p.border_px = 0;
    // With something to show (the command line, a message, a recording) it
    // has the status bar's colour, one block with it; empty, the text's, so
    // that no empty band sits over the status bar.
    if (status.empty() && right.empty()) p.bg = theme_.bg;
}

// Lights up the matches of the last search in the split that has the
// keyboard (Vim's hlsearch, or what the find box is looking for in the
// file), worked out again only when the text, the view or the pattern has
// changed; and shows Visual Block mode's rectangle.
void App::vim_marks(FrameDesc& fd) {
    if (focus_view_ >= fd.view_count) return;
    VimUi& u = *vimui_;
    GridSpec& grid = fd.views[focus_view_].grid;
    const Editor& ed = pane_->doc->ed;
    u.block.clear();
    u64 line0, line1;
    u32 col0, col1;
    if (cfg_.modal && pane_->doc->id == vim_doc_ && pane_->id == vim_pane_ && vim_.block(ed, &line0, &line1, &col0, &col1)) {
        // (On each line the grid shows as much of it as the line has text for.)
        for (u64 line = std::max(line0, grid.top_line); line <= line1 && line < grid.top_line + grid.rows; ++line) u.block.push_back({line, col0, col1, kDecoSelect});
        if (!u.block.empty()) fd.views[focus_view_].sel_radius = look().sel;  // its corners are rounded like any selection's
    }
    static const Vim::Pattern kNoPattern{};
    const Vim::Pattern* boxed = find_highlight();
    const Vim::Pattern& pat = boxed ? *boxed : cfg_.modal ? vim_.highlight() : kNoPattern;
    if (pat.empty() || pat.bad()) {
        if (u.block.empty()) return;
        u.all.assign(grid.decos.begin(), grid.decos.end());
        u.all.insert(u.all.end(), u.block.begin(), u.block.end());
        grid.decos = u.all;
        return;
    }
    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
    mix(pane_->doc->id);
    mix(ed.revision());
    mix(grid.top_line);
    mix(grid.rows);
    mix(ed.tab_width);
    mix(std::hash<std::string>{}(pat.text));
    mix(reinterpret_cast<u64>(pat.re.get()));
    mix((pat.ignore_case ? 1u : 0u) | (pat.word ? 2u : 0u));
    if (key != u.marks_key && pat.re && pat.re == u.marks_gave_up) {
        u.marks_key = key;
        u.marks.clear();
    } else if (key != u.marks_key) {
        u.marks_key = key;
        u.marks.clear();
        const u32 runs = pat.re ? pat.re->overruns() : 0;
        auto gave_up = [&] {
            if (!pat.re || pat.re->overruns() == runs) return false;
            u.marks_gave_up = pat.re;
            return true;
        };
        constexpr u64 kLineBytes = 8192;  // a longer line is looked at this far
        constexpr size_t kMost = 4000;
        const u64 lines = ed.buf.line_count(), bottom = std::min<u64>(grid.top_line + grid.rows, lines);
        // A match, a piece on each line of the view it is on. (One of
        // nothing, "^" or "\zs" alone, is not lit.)
        auto mark = [&](u64 a, u64 b) {
            if (b <= a) return;
            for (u64 line = std::max(ed.buf.line_of(a), grid.top_line); line < bottom && u.marks.size() < kMost; ++line) {
                const u64 start = ed.buf.line_start(line);
                if (start >= b) break;
                const u64 end = ed.line_end(line);
                const u32 c0 = a > start ? ed.col_between(start, std::min(a, end)) : 0;
                const u32 c1 = b > end ? ed.col_between(start, end) + 1 : ed.col_between(start, b);
                u.marks.push_back({line, c0, std::max(c1, c0 + 1), kDecoMatch});
            }
        };
        if (!pat.re) {
            for (u64 line = grid.top_line; line < bottom && u.marks.size() < kMost; ++line) {
                const u64 start = ed.buf.line_start(line);
                const std::string text = ed.buf.text(start, std::min(ed.line_end(line) - start, kLineBytes));
                for (size_t at = Vim::match_in(text, pat, 0); at != std::string::npos; at = Vim::match_in(text, pat, at + pat.text.size())) {
                    const u32 c0 = ed.col_between(start, start + at), c1 = ed.col_between(start, start + at + pat.text.size());
                    u.marks.push_back({line, c0, std::max(c1, c0 + 1), kDecoMatch});
                }
            }
        } else if (!pat.re->multiline()) {  // line by line, a very long one as far as kLineBytes
            for (u64 line = grid.top_line; line < bottom && u.marks.size() < kMost && !gave_up(); ++line) {
                const u64 start = ed.buf.line_start(line), len = ed.line_end(line) - start;
                Vim::each_whole(ed.buf, pat, start, start + std::min(len, kLineBytes) + 1, false, [&](const RegexMatch& m) {
                    mark(m.start, m.end);
                    return u.marks.size() < kMost;
                });
            }
        } else if (grid.top_line < bottom) {  // (a match may run from one line into the next)
            const u64 lo = ed.buf.line_start(grid.top_line), hi = ed.line_end(bottom - 1) + 1;
            Vim::each_whole(ed.buf, pat, lo, hi, false, [&](const RegexMatch& m) {
                mark(m.start, m.end);
                return u.marks.size() < kMost;
            });
            gave_up();
        }
    }
    if (u.marks.empty() && u.block.empty()) return;
    // With whatever the language server has underlined.
    u.all.assign(grid.decos.begin(), grid.decos.end());
    u.all.insert(u.all.end(), u.marks.begin(), u.marks.end());
    u.all.insert(u.all.end(), u.block.begin(), u.block.end());
    grid.decos = u.all;
}

// Where things stand, on one line (for the tests): the mode, the bottom
// line, the cursor's line and column, and the text of the cursor's line.
std::string App::vim_dump() {
    vim_attach();
    static const char* const modes[] = {"NORMAL", "INSERT", "REPLACE", "VISUAL", "V-LINE", "COMMAND", "V-BLOCK"};
    const Editor& ed = pane_->doc->ed;
    const u64 line = ed.buf.line_of(ed.cursor()), start = ed.buf.line_start(line);
    std::string status = vim_.status();
    if (vim_.command_selected()) status = status.substr(0, 1) + "[" + status.substr(1) + "]";  // (what Cmd+F put there, selected)
    return std::string(modes[vim_.mode()]) + " | " + status + " | " + std::to_string(line + 1) + ":" + std::to_string(ed.cursor() - start + 1) + " | " +
           ed.buf.text(start, std::min<u64>(ed.line_end(line) - start, 200));
}

}  // namespace blit
