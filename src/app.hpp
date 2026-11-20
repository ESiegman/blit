#pragma once
// The application: window, input, scrolling, and the frame loop.
#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "build.hpp"
#include "buildscan.hpp"
#include "config.hpp"
#include "editor.hpp"
#include "folds.hpp"
#include "font.hpp"
#include "git.hpp"
#include "grammars.hpp"
#include "instance.hpp"
#include "keymap.hpp"
#include "lsp.hpp"
#include "project.hpp"
#include "render.hpp"
#include "syntax.hpp"
#include "vim.hpp"
#include "watch.hpp"
#include "wrap.hpp"

namespace blit {

struct Options {
    const char* file = nullptr;
    std::vector<OpenAsk> more;     // the other files named after it (instance.hpp)
    const char* font = nullptr;    // font family or file; overrides the config
    // (The benchmark and test options below can only be set in a Debug
    // build: main.cpp reads their flags there and nowhere else.)
    const char* bench = nullptr;   // benchmark scenario, see appbench.cpp
    const char* shot = nullptr;    // render one frame offscreen to this BMP and exit
    const char* script = nullptr;  // input events to replay before --shot, see run_script()
    float pt = 0;                  // font size in points; 0 = from the config
    float scale = 0;               // 0 = ask the display
    int off_w = 0, off_h = 0;      // offscreen target size in pixels (no window)
    u64 goto_line = 0;             // 1-based line to show at start
    u32 goto_col = 0;              // columns scrolled horizontally at start
    int frames = 0;                // benchmark length override
    bool select_all = false;
    bool info = false;             // print the resolved settings and font, then exit
    bool list_grammars = false;    // print the grammars blit can install, then exit
    std::vector<std::string> install;  // grammars to install, then exit
    int frames_in_flight = 0;      // 0 = default
    const char* present = nullptr; // vsync | mailbox | immediate
    bool debug = false;            // GPU validation
    u64 t_main = 0;                // steady clock at entry to main()
    double launch_wall = 0;        // wall clock (s) just before exec, from the bench launcher
    float fps_cap = 0;             // frames per second to hold to without a window (tests of the cap)
};

// The platform's shortcut modifier (Cmd on macOS, Ctrl elsewhere) and the
// modifier that moves by words (Option on macOS, Ctrl elsewhere).
#if defined(__APPLE__)
inline constexpr SDL_Keymod kPrimaryMod = SDL_KMOD_GUI;
inline constexpr SDL_Keymod kWordMod = SDL_KMOD_ALT;
#else
inline constexpr SDL_Keymod kPrimaryMod = SDL_KMOD_CTRL;
inline constexpr SDL_Keymod kWordMod = SDL_KMOD_CTRL;
#endif
// Ctrl is both the editor's key and Vim's (everywhere but on a Mac). With
// modes on, Vim keeps its Ctrl keys while the text has the keyboard, and
// the editor's commands it takes them from are on Ctrl+Shift or Alt as well
// (README.md, "Keys without Cmd"; vim_owns_key() in appvim.cpp).
inline constexpr bool kCtrlShared = kPrimaryMod == SDL_KMOD_CTRL;

// A path's folder, and a path made whole (from the current directory).
std::string parent_of(const std::string& path);
std::string full_path(const std::string& path);
// A language's name as people write it: "C++" for "cpp" (appstatus.cpp).
std::string language_name(const std::string& id);

// One open file: its text, its colours, and where its view was left.
struct Profile;      // (profile.hpp)
struct ProfileFunc;
struct ProfView;     // a profile's tab (appprofile.cpp)
struct TermView;     // a terminal's tab (appterm.cpp)
struct MdView;       // a Markdown preview's tab (appmarkdown.cpp)
struct AsmTab;       // a file's assembly's tab (appasm.cpp)
struct NotebookTab;  // a Jupyter notebook's tab (appnotebook.cpp)
struct NotebookCell;  // (notebook.hpp)
struct MergeView;    // a merge tab (appgit.cpp)
struct RebaseView;   // an interactive rebase's tab (appgit.cpp)
struct SettingsView;  // the settings screen's tab (appsettings.cpp)
struct ImageView;    // an image file's tab (appimage.cpp)
struct PdfView;      // a PDF's tab (apppdf.cpp)
struct RemoteEntry;  // a folder's entry on another machine (remote.hpp)

// A picture read from its file and put on the GPU (appimage.cpp).
struct PictureInfo {
    enum : u8 { kReading, kReady, kFailed };
    u8 state = kReading;
    bool clear = false;   // some of it is see-through
    bool again = false;   // being read again (its file changed); what is here stays until then
    u32 gpu = 0;          // the renderer's id for it
    int w = 0, h = 0;     // as read (smaller than it is, if it is very large)
    int full_w = 0, full_h = 0, frames = 1;
    std::string kind, error;
    u64 bytes = 0;        // the file's size
    i64 stamp = 0;        // its change time when it was read (a file here)
    u64 used_ns = 0;      // when it was last drawn or asked about
    // An animation (a GIF, an animated PNG or WebP): each frame's picture on
    // the GPU (0: not yet; it goes up the first time it is shown), how long
    // each shows, the frames not up yet (let go once all are), which one is
    // shown, until when (0: it is not playing), and how many times it has
    // played through, of how many (0: for ever).
    std::vector<u32> frame_gpu, frame_ms;
    std::shared_ptr<u8> frames_waiting;
    int frames_w = 0, frames_h = 0;
    u32 frame = 0, played = 0, loops = 0;
    u64 frame_due_ns = 0;
    bool finished = false;
    // Drawn from shapes (an SVG): drawn again at the size it is shown, from
    // its file's bytes, one size at a time; the largest size it was shown
    // at in the frame it was last drawn in, and in the frame before that.
    bool vector = false, redrawing = false;
    bool made = false;  // made in memory (a diagram's), not read from a file
    std::shared_ptr<const std::string> source;
    u64 shown_frame = 0;
    int shown_w = 0, shown_h = 0, before_w = 0, before_h = 0;
};

class App;

// A dev container, as its devcontainer.json says it is made (appcontainer.cpp).
struct ContainerPlan {
    std::string root, name, image, dockerfile, context, workspace, mount;
    std::vector<std::string> run_args, env, post;
};

// What a new init.lua starts as: every setting, commented out (init.example.lua).
std::string_view init_template();
struct Doc;

// Moves the places the other splits keep in a document two or more of them
// show (Pane::places) along with its edits.
struct OtherViews final : EditWatch {
    explicit OtherViews(Doc* d) : doc(d) {}
    App* app = nullptr;
    Doc* doc = nullptr;
    void edited(u64 start, u64 old_end, u64 new_end) override;
};

struct Doc {
    Editor ed;
    Syntax syntax;  // after ed: it must let go of the buffer first
    Folds folds;    // what is folded away (listening to ed while there is any)
    u64 id = 0;     // stays the same however the tabs are reordered
    double scroll_x = 0, scroll_y = 0;  // the view, kept while another tab is in front
    // Shown in more than one split, the editor holds one split's cursor at a
    // time: this one's (a pane's id; 0: none in particular), the others'
    // being kept in their Pane::places.
    u64 placed = 0;
    OtherViews others{this};
    // The widest line, in columns, and the revision of the text it was
    // measured at (see App::widest_line()).
    u64 widest_rev = ~0ull;
    u32 widest_tab = 0;
    u64 widest_cols = 0;
    // Columns per level of indentation, guessed from the text; 0: not yet.
    u64 indent_rev = ~0ull, indent_lines = 0;
    u32 indent_step = 0;
    bool indent_tabs = true;
    bool indent_chosen = false;  // chosen from the status bar: no longer guessed (indent_step 0: the tab width)
    std::string lang;            // its language as chosen from the status bar ("-": plain text); empty: by its name
    // A big file's widest line is found on a thread of its own, which
    // leaves the answer here. Declared last: the thread is stopped and
    // waited for before the text it reads goes.
    u64 measuring_rev = 0, measured_cols = 0;
    u32 measuring_tab = 0;
    std::atomic<bool> measured{false};
    std::jthread measurer;
    // Cell bits for each of its syntax style ids (see App::pick_theme()).
    u32 style_bits[256] = {};
    size_t styles_known = 0;

    // Git's view of it (appgit.cpp): the lines that differ from the
    // committed text and who last changed each line, each with the
    // revision of the text it was worked out for and the one last asked
    // about, and whether blame shows (it does only when it is turned on:
    // Cmd+Option+B, or the Git tab's Blame).
    Git* git = nullptr;  // set once something has been asked about this document
    std::vector<GitHunk> hunks;
    u64 hunks_rev = ~0ull, hunks_asked = ~0ull, hunks_gen = 0;
    std::shared_ptr<GitBlame> blame;
    u64 blame_rev = ~0ull, blame_asked = ~0ull, blame_gen = 0;
    bool blame_on = false;
    std::shared_ptr<struct GitView> gitview;  // a diff tab's: what it shows, how it was asked for, its hunks
    std::shared_ptr<RebaseView> rebase;  // an interactive rebase's tab: the commits, and what is done with each (appgit.cpp)
    std::shared_ptr<MergeView> merge;  // a merge tab's: a conflicted file's three versions, and what is taken of each (appgit.cpp)
    u64 seen_rev = ~0ull, changed_ns = 0;  // when its text was last seen to have changed
    u64 hist_rev = ~0ull, hist_ns = 0;     // the text last kept in its local history, and when (apphistory.cpp; 0: not yet this session)
    bool hist_disk = false;                // the text it had on disk kept, at its first save this session (apphistory.cpp)
    i64 disk_stamp = 0;                    // the file's change time when it was last read or written
    std::optional<std::string> renamed_from;  // (Save as: the name it had, back again if the save fails)
    bool disk_moved = false;               // its file changed on disk under changes of its own: the next save asks first
    bool save_over = false;                // (and the answer was to save over it)
    // A tab that shows something made and not a file (a diff, a commit):
    // what it is called, that it is not to be typed in, and its colours.
    std::string title;
    bool read_only = false;
    bool loading = false;  // a file on another machine, on its way (read only until it is here: see appremote.cpp)
    bool must_exist = false;  // (and asked for as one that is there, by gf or a click: if it is not, its tab goes)
    u64 goto_line = ~0ull;  // and where the cursor goes when it is (a line from 0, and a byte in it)
    u32 goto_col = 0;
    bool goto_utf16 = false;  // (goto_col is a language server's that counts UTF-16 units, not bytes)
    std::shared_ptr<ProfView> prof;  // a profile's tab: drawn over the text area (appprofile.cpp)
    std::shared_ptr<TermView> term;  // a terminal's tab: drawn over it too (appterm.cpp)
    std::shared_ptr<MdView> md;      // a Markdown preview's tab: drawn over it too (appmarkdown.cpp)
    std::shared_ptr<AsmTab> asmv;    // a file's assembly's tab (appasm.cpp)
    std::shared_ptr<NotebookTab> nb;  // a notebook's: what it was, its outputs (appnotebook.cpp)
    std::shared_ptr<SettingsView> settings;  // the settings screen's: its settings, a line each (appsettings.cpp)
    std::shared_ptr<struct Minimap> minimap;  // its minimap's tiles (appminimap.cpp)
    u64 hl_gen = 0;  // counts up as its syntax colours change
    signed char wrap_choice = -1;  // its own soft wrap (Toggle soft wrap): 1 on, 0 off, -1 as the settings say
    std::shared_ptr<ImageView> image;  // an image file's tab: drawn over it too (appimage.cpp)
    std::shared_ptr<PdfView> pdf;      // a PDF's tab: drawn over it too (apppdf.cpp)
    bool term_closed = false;        // (its program has ended, and that has been seen to)
    std::unique_ptr<Highlights> marks;

    // (Git's worker may be reading the text: it lets go first.)
    ~Doc() {
        if (git) git->forget(id);
    }
};

struct TabBar;  // a pane's tab bar and line numbers, as cells (apptabs.cpp)
struct Crumbs;  // a pane's breadcrumbs under its tabs, as cells (appshape.cpp)
struct Sticky;  // the scope lines pinned at the top of a pane's view (appshape.cpp)
struct Ruler;   // the marks along a pane's scroll bar (appshape.cpp)
struct ScriptKey;  // a key held back for the mappings (applua.cpp)

// One split of the window: a group of tabs, and the view of the one in
// front. A window that is not split has one.
struct Pane {
    u64 id = 0;
    // A peek's box (apppeek.cpp): not one of the window's splits but over
    // one (host), placed under the line it was asked from (peek_line, the
    // host's).
    bool floating = false;
    u64 host = 0;
    u64 peek_line = 0;
    // Its files, in the order of their tabs, and the one in front. Never
    // empty while the pane is on show.
    std::vector<std::shared_ptr<Doc>> docs;  // (shared: one file can show in several splits, each with its own place in it)
    Doc* doc = nullptr;
    size_t tab = 0;
    // Its place in each document another split shows too, kept while that
    // split has the document's editor (see App::place_view()): the cursor
    // and the selection, and, while the document is not in front here,
    // the scroll.
    struct Place {
        Editor::Caret caret;
        double scroll_x = 0, scroll_y = 0;
        bool scrolled = false;  // (the scroll is kept here, not in the document)
    };
    std::unordered_map<u64, Place> places;
    // Where it is in the window, in pixels, and where in that the text
    // starts: right of the line numbers, under the tab bar.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int text_x = 0, text_y = 0;
    // Scroll position in pixels; scroll eases toward target.
    double scroll_x = 0, scroll_y = 0;
    double target_x = 0, target_y = 0;
    // Cursor and selection in (line, column), cached per edit/move.
    u64 cache_rev = ~0ull, cache_cursor = ~0ull, cache_anchor = ~0ull, cache_folds = ~0ull;
    u64 cur_line = 0;
    u64 cur_row = 0;  // the row of the view it is on: its line less those folded away above it
    std::vector<u64> row_lines;  // with lines folded, the line each row of the view shows (GridSpec::row_lines)
    std::vector<u32> row_cols, row_ends;  // with lines wrapped, where each row starts in its line and where its part ends (GridSpec)
    std::unique_ptr<Wrap> wrap;           // this view's soft wrap, while its file wraps (appwrap.cpp)
    std::vector<GridNote> fold_notes;  // (fold_notes(): the notes after its folded lines, with the others)
    std::vector<u32> fold_note_cells;
    u32 cur_col = 0;
    u32 cur_x = 0;  // the column along its row, where it is drawn: cur_col less where a wrapped line's row starts
    u64 sel_line0 = 0, sel_line1 = 0;
    u32 sel_col0 = 0, sel_col1 = 0;
    bool has_sel = false;       // there is a selection to draw: the editor's own, or Visual mode's
    bool cache_visual = false;  // and it was Visual mode's when the cache was made
    // What the renderer remembers about this view from frame to frame.
    AnchorCache anchors;
    GuideCache guides;
    u32 view = 0;  // which of the last frame's text views was this pane's
    std::shared_ptr<TabBar> bar;
    std::shared_ptr<Crumbs> crumbs;  // (appshape.cpp)
    std::shared_ptr<Sticky> sticky;  // (appshape.cpp)
    std::shared_ptr<Ruler> ruler;    // (appshape.cpp)
    // Its scroll thumb: how strongly it shows (it fades away when the view
    // has been still a while), when it starts to fade, and where the view
    // was when it last showed fully.
    float thumb_alpha = 1;
    bool has_thumb = false;  // there is something to scroll (set when it is laid out)
    u64 thumb_until_ns = 0;
    double thumb_x = -1, thumb_y = -1;
};

// How the window is split: a leaf shows a pane, anything else is two
// halves, side by side or one above the other, each split further or not.
struct SplitNode {
    Pane* pane = nullptr;
    bool side_by_side = true;
    float share = 0.5f;  // of the first half
    std::unique_ptr<SplitNode> a, b;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // where it was last placed
};

class App {
public:
    App();
    ~App();
    int run(const Options& opt);

private:
    bool init(std::string* err);
    void loop();
    void handle(const SDL_Event& e);
    void on_key(const SDL_KeyboardEvent& k);
    void on_wheel(const SDL_MouseWheelEvent& w);
    void on_mouse_down(const SDL_MouseButtonEvent& b);
    void on_mouse_motion(const SDL_MouseMotionEvent& m);
    void drag_thumb(float y);

    Renderer::Result frame(FrameStats* st = nullptr);
    void pace();  // holds the next frame back to the display's rate, where the window system does not
    void choose_images();  // how many images of the window may be under way, for what is drawn now
    void layout(int w, int h, FrameDesc& fd);
    bool step_scroll(double dt);
    void scroll_to_cursor(bool jump = false);
    void clamp_scroll();
    double max_scroll_y() const;
    u64 offset_at_point(float x, float y) const;
    void point_line_col(float x, float y, u64* line, u32* col) const;
    void caret_marks(FrameDesc& fd);
    struct SnippetRun;
    std::shared_ptr<SnippetRun> snippet_;  // a snippet being filled in (appsnippet.cpp)
    std::vector<GridDeco> snippet_decos_;
    bool snippet_live();
    void snippet_end();
    void snippet_expand(std::string_view body, u64 from, u64 to, const std::string& selected);
    void snippet_go(size_t k);
    bool snippet_key(const SDL_KeyboardEvent& k);
    void snippet_pick();
    void snippet_marks(FrameDesc& fd);
    std::string snippet_dump();
    u32 vim_feed(const VimKey& v);
    void edit_key(const SDL_KeyboardEvent& k, i64 page);
    void caret_add_match(bool every);
    void caret_add_line(int dir);
    void carets_to_visual();
    void carets_from_visual();
    void caret_click(float x, float y, bool drag);
    std::string carets_text() const;
    void carets_paste(const std::string& text);
    std::string caret_dump() const;
    std::vector<GridDeco> caret_decos_;
    std::vector<Editor::Caret> column_keep_;  // (Alt+drag: the carets there were before it)
    u64 column_from_line_ = 0;
    u32 column_from_col_ = 0;
    bool column_drag_ = false;
    void refresh_cursor_cache();
    void update_title();
    bool load_font(std::string* err);
    void rescale_scroll(int old_cw, int old_ch);  // every view keeps its place when the cells change size
    bool adopt_font(std::unique_ptr<Font> f, std::string* err);
    FontSpec font_spec(float scale) const;
    void pick_theme();
    void start_syntax(u64 first_line, bool at_launch = false);
    void stop_syntax();
    void sync_syntax();
    void settle_syntax();
    std::string file_language() const;
    void want_grammars();
    void sync_grammars();
    void grammar_install();  // the grammar of the file in front's language, fetched and built (a command)
    bool grammar_missing();  // that language has none installed: its colours are missing
    void open_config();
    // The text's shape from its syntax tree (appshape.cpp).
    void shape_marks(FrameDesc& fd);
    std::string shape_dump();  // (the tests: the brackets on show, by depth, how many inks they take, and the pair at the cursor)
    std::vector<GridDeco> shape_decos_;
    // Expand the selection (to the tree's node around it, then its parent)
    // and shrink it back: the nodes asked for at the first press, kept while
    // the text and the selection stay as these left them.
    void select_grow(bool grow);
    // Lines moved up (-1) or down (+1), and copied (appedit.cpp).
    void lines_move(int dir);
    void lines_copy(int dir);
    // Run configurations (.blit/run.json, appbuild.cpp): the one chosen,
    // its program taken by Debug and Profile, and the file made or opened.
    const BuildTask* run_config() const;
    void run_config_take();
    void run_config_edit();
    void debug_take(const std::string& program, const std::string& args, const std::string& cwd, const std::vector<std::string>& env);
    void prof_take(const std::string& program, const std::string& args, const std::string& cwd, const std::vector<std::string>& env);
    std::string debug_target() const;  // (the tests: what Debug and Profile would run: program, arguments, folder, environment)
    std::string prof_target() const;
    // Brackets and quotes closed as they are typed (appedit.cpp): a key or a
    // character taken (true), and the closers put in, kept in step with what
    // is typed between them.
    bool pair_text(const char* text);
    bool pair_key(const SDL_KeyboardEvent& k);
    void pairs_follow(u64 cursor_before, u64 size_before, u64 rev_before);
    std::vector<u64> pair_closers_;
    u64 pair_doc_ = 0, pair_rev_ = ~0ull;
    // The breadcrumbs under a split's tabs: how tall (none for a tab that is
    // not a file's text), drawn, clicked.
    int crumbs_px(const Pane& p) const;
    void crumbs_layout(FrameDesc& fd, bool focused);
    bool crumbs_mouse_down(const SDL_MouseButtonEvent& b);
    std::string crumbs_dump();  // (the tests: what the split in front's say)
    // Sticky scroll: the lines that open the scopes the top of the view is
    // in, pinned over it (and clicked, gone to).
    void sticky_layout(FrameDesc& fd, const TextView& v);
    bool sticky_mouse_down(const SDL_MouseButtonEvent& b);
    std::string sticky_dump();  // (the tests: which lines are pinned in the split in front)
    struct Grow {
        u64 doc = 0, revision = ~0ull;
        std::pair<u64, u64> start{}, now{};  // what was selected before the first press, and after the last
        std::vector<std::pair<u64, u64>> chain;
        size_t at = 0;  // how many of the chain are taken
    } grow_;
    // init.lua at the line that sets `key` (its value selected): the line
    // there, else the template's commented one made live, else a new one.
    void open_setting(const std::string& key);
    void apply_present_mode();
    void update_refresh();
    void zoom(float dpt);
    bool open_path(const char* p, bool must_exist = false);
    void open_named(std::string name);
    bool save();  // false: not written (nor on its way to another machine)
    void disk_changed_under(Doc& d);  // its file changed on disk under its own changes: a notice, and the next save asks
    void save_ask(u64 doc);           // whether to save over what is on disk now, asked in the box
    void disk_compare();              // "Compare with disk": the file in front against what is on disk (apphistory.cpp)
    void disk_compare_show(u64 doc, std::string_view disk);
    void request_quit();
    int view_h() const { return std::max(1, pane_->y1 - pane_->text_y); }  // height of the focused pane's text area
    int view_w() const { return std::max(1, pane_->x1 - pane_->text_x); }
    int visible_rows() const { return view_h() / font_->cell_h(); }
    mutable std::string workdir_for_, workdir_found_;  // (default_workdir()'s last answer, for which folder)
    mutable u64 workdir_ns_ = 0;
    // Rows of the view and the lines they show, with what is folded in the
    // file in front (appshape.cpp): with nothing folded, the same numbers.
    const Folds* folds_on() const;
    u64 row_of_line(u64 line) const;
    u64 line_of_row(u64 row) const;
    u64 view_line_count() const;  // rows the whole text takes
    // Folding (appshape.cpp): the fold at the cursor's line closed, opened,
    // switched; all of them; and a line the cursor has gone into shown.
    void fold_close();
    void fold_open();
    void fold_toggle();
    void fold_all(bool close);
    void fold_reveal();
    void fold_notes(FrameDesc& fd);
    // The marks along the scroll bar (appshape.cpp): problems, matches,
    // changes, breakpoints and the cursor, where they are in the file.
    void ruler_layout(const TextView& v);
    void ruler_notes(FrameDesc& fd);
    std::string ruler_dump();  // (the tests: the marks of the split in front, row:kind)
    // Find in this file's matches, while its box is open on that document (appfind.cpp).
    const std::vector<u64>* find_file_matches(u64 doc);
    bool fold_mouse_down(const SDL_MouseButtonEvent& b);  // a click on a folded line's mark or note opens it
    std::string fold_dump();  // (the tests: what is folded in the file in front, and the rows on show)
    void measure_widest();  // finds out how wide the widest line is, if that is not known
    void take_measured();
    double max_scroll_x() const;
    u32 indent_step();

    // The language server's part of the editor (applsp.cpp).
    struct Assist;
    enum : u32 { kPopCompletion = 1, kPopHover = 2, kPopSignature = 4, kPopAll = 7 };
    void start_lsp();
    void stop_lsp(bool keep_server = false);  // keep_server: the next file may be one for it too
    void sync_lsp();
    void lsp_close(u32 what);
    void request_completion(char trigger);
    bool filter_completions();
    void accept_completion();
    void show_hover(u64 off, bool at_cursor);
    int lsp_wait_ms() const;
    void lsp_tick();
    void lsp_mouse(float x, float y);
    void goto_definition();
    void go_back();
    void lsp_typed(std::string_view text);
    bool lsp_key(const SDL_KeyboardEvent& k);
    bool lsp_completing() const;  // a completion list is open
    void lsp_after_key(SDL_Keycode key);
    bool lsp_click(float x, float y, bool cmd);
    void lsp_decorate(const GridSpec& g);
    void lsp_layout(int w, int h, FrameDesc& fd);
    void lsp_forgive();  // servers that failed may be tried again
    u32 lsp_popups_open() const;  // how many popups the last layout drew
    // Every tab with text a server could have is open on one (didOpen and
    // didClose follow the tabs); and a document about to go is let go of.
    void lsp_reconcile();
    void lsp_forget(const Doc& d);
    // Shift+F12 (Vim's gr): the references to the name at the cursor, in a
    // list; F4 / Shift+F4 step through them while it is the last list made.
    void lsp_references();
    bool lsp_step(int by);
    // Finding one's way (applsp.cpp): implementations, type definitions and
    // declarations (one gone to, several listed as references are); the
    // header for a source file or the source for a header; the file's
    // outline; the project's symbols, asked for as they are typed.
    void lsp_places(Lsp::PlaceKind kind);
    void lsp_show_places(std::vector<LspPlace> list, const std::string& word);
    void lsp_switch_header();
    std::string other_half(const std::string& path) const;
    void other_half_open(const std::string& path);  // (another machine's looked for on an errand there)
    void lsp_outline();
    // The file's outline in the box, `pick` (a symbol's offset; ~0: the one
    // the cursor is in) chosen to start with.
    void outline_show(std::vector<LspSymbol> list, u64 pick);
    static std::vector<LspSymbol> tree_symbols(const Doc& d, const SyntaxStructure& s);
    void lsp_find_symbols();
    void lsp_go_symbol(size_t i);  // (one of the project's symbols last found)
    // Search everywhere's symbols: the server asked for those with `typed`
    // in them, and what it last found, as a name and what it is and where.
    void lsp_everywhere_ask(const std::string& typed);
    std::vector<std::pair<std::string, std::string>> lsp_symbol_rows() const;
    bool lsp_uses_shown() const;
    std::string lsp_uses_dump();  // (the tests: the uses marked in the split in front, as line:column ranges)
    void lsp_rename();   // F2
    void lsp_rename_checked(const std::vector<LspPlace>& found, bool defined);  // (where the name is declared or defined, while the server knows the open files only)
    // Option+Shift+F: the selection's lines, or the whole file (Vim's gq: lines l0 to l1).
    void lsp_format();
    void lsp_format_lines(u64 l0, u64 l1);
    void lsp_format_on_save();  // before the file in front is written, with lsp.format_on_save on
    void lsp_actions();  // Option+Enter: the fixes and refactorings at the cursor, in a menu
    bool lsp_apply(const LspWorkspaceEdit& e, std::string* said);  // a workspace edit made, as Replace in files does
    Doc* lsp_edit_doc(const std::string& path);  // the tab a file a workspace edit changes is open in, if any
    u32 lsp_gutter(const Doc& d, u64 line);  // a mark beside a problem the server has a fix for
    void lsp_extras(FrameDesc& fd);  // inlay hints and the server's colours, for each view
    static std::string hint_text(const InlayHint& h);  // (an inlay hint as it is drawn)
    std::string lsp_dump();  // (tests) the servers, and each tab's place on one
    std::string hover_dump();  // (tests) the hover's rows, and where it is down the window
    std::string lsp_extras_dump(bool colours = false);  // (tests) the inlay hints and the servers' colours on show (with what each colour is)
    void lsp_go_problem(size_t i);  // (apppanel.cpp) the language servers' problem i, counted across files

    // The open files and their tab bar (apptabs.cpp).
    void tabs_create();
    Doc* add_doc(size_t at);
    Doc* find_doc(u64 id, Pane** pane = nullptr, size_t* index = nullptr);
    bool find_open(const std::string& full, Pane** pane, size_t* index) const;  // the tab showing a file, if any (the focused split's first)
    // Two views of one file (apptabs.cpp): how many splits show a document;
    // every document open, each once; the editor made to hold a split's own
    // place in the document in front there; that place, wherever it is
    // kept; a file another split shows, shown in this one too.
    int views_of(const Doc* d) const;
    std::vector<Doc*> open_docs() const;
    void place_view(Pane* p);
    Editor::Caret caret_in(const Pane& p, const Doc& d) const;
    void show_also(Pane* from, size_t index);
    bool shareable(const Doc& d) const;
    void views_edited(Doc& d, u64 start, u64 old_end, u64 new_end);
    friend struct OtherViews;
    void tab_show(size_t i);
    void tab_new();
    bool tab_close(size_t i, bool force = false, bool quit_if_last = false);  // false: the user said no (force: without asking)
    void tab_move(size_t from, size_t to);
    void tab_step(int by);
    void for_each_doc(std::function_ref<void()> fn);  // runs fn with each document in front in turn
    void renamed_on_disk(const std::string& from, const std::string& to);
    int tabs_px() const;
    bool tabs_key(const SDL_KeyboardEvent& k);
    bool tabs_mouse_down(const SDL_MouseButtonEvent& b);
    bool tabs_mouse_motion(const SDL_MouseMotionEvent& m);
    void tabs_mouse_up();
    bool tabs_wheel(const SDL_MouseWheelEvent& w);
    void tabs_layout(int w, int h, FrameDesc& fd, bool focused);
    std::vector<std::string> tab_labels(const Pane& p) const;
    std::string tabs_dump();  // (the tests: what the split in front's tabs say, " | " between them)

    // Finding files, text and paths (appfind.cpp).
    struct Find;
    enum : int { kFindNames, kFindText, kFindOpen, kFindSave, kFindFolder, kFindPick, kFindInput, kFindAsk, kFindCommand, kFindFile, kFindAll };
    void find_create();
    bool find_is_open() const;
    void find_stale();  // the files may have changed: the finder reads the folder again when it opens
    void find_open(int mode, const std::string& typed = {});
    void find_close(bool chosen);
    void find_query();
    void find_sync();
    bool find_current(int wait_ms);  // the rows answer what is typed (waiting a moment for the worker if they do not yet)
    void find_accept();
    void find_tab();
    void find_move(i64 by);
    bool find_key(const SDL_KeyboardEvent& k);
    bool find_typed(const char* text);
    int find_row_at(int px, int py) const;
    bool find_mouse_down(const SDL_MouseButtonEvent& b);
    bool find_mouse_motion(const SDL_MouseMotionEvent& m);
    bool find_wheel(const SDL_MouseWheelEvent& w);
    void find_layout(int w, int h, FrameDesc& fd);
    std::string find_dump();
    std::string find_heading() const;  // (the tests: what the box says it lists, "References (4)")
    std::string find_inks_dump();  // (tests) the inks of the rows' secondary text
    void find_settle(bool listing = false);
    void find_save_for(u64 doc);  // the document the name being asked for is for
    // The box as a picker for a script (input: a line to type, no list).
    void find_pick(std::vector<std::string> items, const std::string& prompt, bool input, const std::string& typed);
    void find_drop_script();  // a script's picker that is open shuts
    // A question ssh asks while connecting (remote.hpp), in the box: hidden as
    // it is typed if it is secret. The box open before it comes back after.
    void find_ask(u64 id, const std::string& host, const std::string& prompt, bool secret);
    u64 find_ask_id() const;
    void find_drop_ask();
    void find_choose(int mode, const std::string& prompt, const std::string& typed, std::function<void(const std::string&)> chose);
    void find_choose_item(const std::string& prompt, std::vector<std::string> items, std::function<void(int)> chose, const std::string& typed = {});
    void find_ask_footer(const std::string& question);
    void remote_asks();
    void save_named(const std::string& path, u64 doc);
    void remember_place();  // where the cursor is now is somewhere to come back to
    // Go to file for a split just made: what is chosen opens in it (appfind.cpp).
    void find_for_split(Pane* p);
    // A list of blit's own whose rows are tried out as the choice moves
    // (`moved` hears each, from 0), starting on `at`: Enter is `chose`, Esc
    // `cancelled` (appfind.cpp; the theme picker's).
    void find_on_side(std::function<bool(int, bool)> fn);
    // Reference counts after the definitions (applsp.cpp).
    void lens_marks(FrameDesc& fd);
    void lens_saved();
    bool lens_click(const SDL_MouseButtonEvent& b);
    std::string lens_dump();
    bool lens_waiting() const;
    std::vector<GridNote> lens_view_notes_;
    // Spelling (appspell.cpp).
    void spell_marks(FrameDesc& fd);
    void spell_take();
    int spell_wait_ms() const;
    bool spell_fix();
    std::string spell_dump();
    std::shared_ptr<struct SpellWork> spell_work_;
    std::shared_ptr<struct SpellUi> spell_;
    // Pull requests, through gh (apppr.cpp).
    std::string git_root() const;
    void pr_run(std::vector<std::string> args, std::function<void(int, const std::string&, const std::string&)> done);
    void pr_list();
    void pr_menu(i64 n);
    void pr_checkout(i64 n);
    void pr_changes(i64 n);
    void pr_comments(i64 n, bool show);
    void pr_comment_menu(size_t i);
    void pr_review(i64 n, const std::string& kind);
    void pr_marks(FrameDesc& fd);
    std::string pr_dump();
    std::shared_ptr<struct PrUi> pr_;
    // Dev containers (appcontainer.cpp).
    bool container_plan(const std::string& root, ContainerPlan* plan, std::string* err);
    void container_open(bool anew);
    // Updates (appupdate.cpp).
    static std::string version();
    void update_check(bool asked);
    void update_fetch(const std::string& version, const std::string& where, const std::string& url, const std::string& sum);
    void update_install(const std::string& version, const std::string& where, const std::string& file);
    bool update_busy_ = false;
    // The settings screen (appsettings.cpp).
    void settings_open();
    void settings_lay(Doc& d);
    void settings_relay();
    bool settings_key(const SDL_KeyboardEvent& k);
    void settings_change(const std::string& key);
    void settings_write(const std::string& key, const ConfigValue* v);
    std::string settings_key_here() const;
    std::string settings_dump();
    static std::string init_with(std::string text, const std::string& key, const std::string& line);
    // The disk watched (apppanel.cpp): the tree reads again what changed.
    void watch_follow();
    void watch_take();
    void watch_tick();
    int watch_wait_ms() const;
    bool panel_drop(const std::string& path, float x, float y);
    std::string tree_dump();
    std::unique_ptr<DirWatch> watch_;
    std::string watch_real_;
    u64 watch_due_ns_ = 0, watch_events_ = 0;
    // Distraction-free and presentation modes (appfocus.cpp).
    enum : u8 { kFocusNone, kFocusPlain, kFocusShow };
    void focus_toggle(u8 mode);
    std::string focus_dump();
    u8 focus_mode_ = kFocusNone;
    float focus_pt_ = 0;
    bool focus_full_ = false;
    // Soft wrap (appwrap.cpp).
    bool wrap_wanted(const Pane& p, const Doc* d = nullptr) const;  // (d: one of its files not in front)
    u32 wrap_width(const Pane& p) const;
    void wrap_sync(Pane& p);
    double view_line(const Pane& p, const Doc& d, double rows) const;  // (a view's top: rows of the split's map as a line)
    double view_rows(const Pane& p, const Doc& d, double line) const;  // (and back)
    const Wrap* wrap_on() const;
    void wrap_toggle();
    void wrap_forget(const Doc& d);
    std::string wrap_dump();
    // The minimap (appminimap.cpp).
    bool minimap_on(const Pane& p) const;
    int minimap_px(const Pane& p) const;
    bool minimap_place(const Pane& p, int* x0, int* y0, int* x1, int* y1, double* top_line, int* lp) const;
    void minimap_tile(Doc& d, u64 index, std::vector<u8>& rgba);
    int minimap_wait_ms() const;
    void minimap_layout(FrameDesc& fd);
    bool minimap_mouse(float x, float y, bool down);
    void minimap_forget(const Doc& d);
    std::string minimap_dump();
    u32 minimap_box_ = 0, minimap_bg_ = 0, minimap_bg_key_ = ~0u;
    u64 minimap_drag_ = 0, minimap_due_ns_ = 0;
    std::vector<u8> minimap_rgba_;
    // Local history (apphistory.cpp).
    std::string history_folder(const Doc& d) const;
    bool history_wanted(const Doc& d) const;
    void history_keep_doc(Doc& d, bool saved, std::string text);
    void history_before_save(Doc& d);
    void history_saved(Doc& d);
    void history_tick();
    void history_show();
    void history_try(int i);
    void history_close_tab();
    void history_put_back(int i);
    std::string history_dump();
    std::shared_ptr<struct HistoryWork> history_work_;
    std::shared_ptr<struct HistoryView> history_view_;
    // Peek (apppeek.cpp).
    void peek_open();
    void peek_show(LspLocation loc);  // (a copy: the client's own goes as the keyboard moves)
    void peek_place(Pane& p, Pane& host);
    void peek_close();
    void peek_layout();
    bool peek_key(const SDL_KeyboardEvent& k);
    std::string peek_dump();
    Pane* peek_pane() const;
    u64 peek_ = 0;
    bool peek_pending_ = false;
    // Call and type hierarchies (applsp.cpp).
    void hier_show(u8 kind);
    void hier_take();
    void hier_open(size_t i);
    bool hier_side(int index, bool right);
    void hier_go(int index);
    std::vector<std::string> hier_texts();
    std::string hier_dump();
    void find_set_rows(std::vector<std::string> items, size_t at);
    void find_preview_item(const std::string& prompt, std::vector<std::string> items, size_t at, std::function<void(int)> moved,
                           std::function<void(int)> chose, std::function<void()> cancelled);
    // A list of blit's own whose items come from elsewhere as what is typed
    // changes (the project's symbols): `typed` is told each new text, and
    // find_set_items() brings the rows for it.
    void find_live_items(const std::string& prompt, std::function<void(const std::string&)> typed, std::function<void(int)> chose);
    void find_set_items(std::vector<std::string> items);
    void find_on_tab(std::function<void(const std::string&)> fn);
    void find_take_typed(std::function<void(const std::string&)> fn);  // (Enter with no row that matches: what is typed, to fn)
    void find_set_typed(const std::string& typed);
    std::string find_typed() const;
    // Search everywhere (kFindAll): files, the project's symbols, commands
    // and settings, each kind under its heading; rows made again when the
    // server's symbols come.
    void find_everywhere();
    bool find_everywhere_open() const;
    void find_everywhere_rebuild();
    // The places jumps started from, across files, for Ctrl+- (back) and
    // Ctrl+Shift+- (forward), and Vim's Ctrl-O and Ctrl-I (app.cpp).
    struct Jumps;
    void jumps_create();
    void jump_record();                       // the cursor's place, in the file in front
    bool jump_note(const Doc& d, u64 off);    // a place in a document (false: not one to come back to)
    void jump_walk(i64 by);                   // back (negative) or forward through them
    bool jump_go(size_t i);
    bool jump_key(const SDL_KeyboardEvent& k);
    // Where something that may jump started (the side panel's rows, F8):
    // noted once it has taken the cursor to another file or line.
    struct JumpFrom {
        u64 doc = 0, off = 0;
    };
    JumpFrom jump_from() const;
    void jump_if_moved(const JumpFrom& from);
    std::string jump_dump() const;
    std::shared_ptr<Jumps> jumps_;
    // (The overhaul's entry points; each is implemented by the part named.)
    void find_step(int by);                      // appfind.cpp: the next (1) or previous (-1) hit of the last search in files
    std::vector<std::string> recent_files(size_t most) const;  // appstart.cpp: files opened lately, the latest first, that are still there
    void save_as();                              // app.cpp: the file in front, under a name asked for
    void save_all();                             // app.cpp: every file with changes that has a name
    void save_docs(const std::vector<u64>& ids);  // app.cpp: those documents (each with a name), the tabs in front staying in front
    // app.cpp: after a change across files (a rename, Replace in files), the
    // open files it changed saved where they had no changes of their own;
    // the others named, in words to add to what is said (empty: none).
    std::string save_changed(const std::vector<u64>& clean, const std::vector<u64>& dirty);
    void find_in_file(bool replace = false);     // appcommands.cpp: a search in the file in front (Vim's /, or a box without modes; with "Replace with", the box), from the word or selection
    // The box that finds in the file in front (kFindFile), with "Replace
    // with" under it if asked; Search in files with that field (Cmd+Shift+H);
    // the next (1) or previous (-1) match of what the box looked for last;
    // and what it is looking for, to be lit up in the file (null: nothing).
    void find_file_open(bool replace, const std::string& typed);  // appfind.cpp
    void find_replace_in_files();                // appfind.cpp
    void find_file_step(int by);                 // appfind.cpp
    const Vim::Pattern* find_highlight() const;  // appfind.cpp
    // The box's toggles: regular expressions (Option+R) and exact case (Option+C).
    void find_toggle(bool regex);                // appfind.cpp
    std::shared_ptr<Find> find_;

    // Looks and motion (appfx.cpp).
    struct Fx;
    struct Look {
        // Corner radii, in pixels: of a floating box, of a lit row or tab, of
        // what is selected in the text, of a search match, of the cursor.
        // And how far a floating box's shadow reaches (0: no shadows).
        float box = 0, pill = 0, sel = 0, match = 0, cursor = 0, shadow = 0;
        // Spacing, in pixels (set in points, so the same on any display; see
        // look.hpp): from a box's edge, or the window's, to its text; from a
        // box's edge to the rounded bar behind a lit row (so that box and bar
        // are rounded about the same centre); above the first row of a box
        // and below its last; from a floating box to what it belongs to; an
        // outline or a rule.
        int edge = 0, inset = 0, pad_y = 0, gap = 0, hair = 1;
    };
    enum : int { kFxCompletion, kFxDoc, kFxHover, kFxSignature, kFxMenu, kFxGhost, kFxFinder, kFxScript, kFxToast, kFxKinds };
    void fx_create();
    Look look() const;
    void fx_kick();
    void fx_float(Popup& p, int kind);
    void fx_begin_layout();
    void fx_end_layout();
    void fx_cursor(const double to[4], double out[4]);
    float fx_thumb(Pane& p, i64 sx, i64 sy);
    bool step_fx(double dt);
    void fx_settle();
    int fx_wait_ms() const;
    void fx_tick();
    void hover_update(int px, int py);
    bool panel_hover(int px, int py);
    std::shared_ptr<Fx> fx_;
    bool fx_live_ = true;     // things move; false: they are drawn where they end up (scripted runs)
    bool fx_moving_ = false;  // something is on its way: frames follow one another

    // Modal editing (appvim.cpp).
    struct VimUi;
    void vim_create();
    void vim_attach();
    bool vim_selection(u64* lo, u64* hi) const;
    bool vim_key(const SDL_KeyboardEvent& k);
    bool vim_owns_key(const SDL_KeyboardEvent& k);  // a Ctrl key that is Vim's before it is the editor's (kCtrlShared)
    void vim_text(const char* text);
    void vim_after(u32 result);
    void vim_mouse();
    int vim_px() const;
    void vim_layout(int w, int h, FrameDesc& fd);

    // The start screen (appstart.cpp), and the files and folders opened lately.
    struct StartUi;
    void start_init();
    bool start_shown();  // (and once it is not, never again)
    void start_do(size_t item);
    bool start_key(const SDL_KeyboardEvent& k);
    bool start_text(const char* text);
    int start_item_at(int px, int py) const;
    bool start_mouse_down(const SDL_MouseButtonEvent& b);
    bool start_mouse_motion(const SDL_MouseMotionEvent& m);
    void start_layout(FrameDesc& fd);
    std::string start_dump();
    // The start screen's New project tab (appstart.cpp, project.cpp).
    void project_page(bool on);
    void project_anywhere();
    void project_change(int by);  // the chosen row's value, a step along (a choice, a switch)
    void project_edit(const std::string& typed = {});
    void project_set(const std::string& key, const std::string& value);
    bool project_create();  // (made on a thread: project_created takes it)
    void project_created();
    void project_wait();    // (for the tests: until what the form waits for has come)
    std::string project_dump();
    std::string project_facts_dump();
    void project_cells(u32 cols, u32 rows);
    void project_facts();  // what the machine the project goes on has (vcpkg, ROCm, GPUs, uv, pkg-config), looked for on a thread
    void project_facts_wait();  // (for the tests)
    void project_facts_apply();
    void start_sync();  // a thread of the start screen has found something, or made the project (user event 19)
    void project_libraries(const std::string& typed = {});
    void recent_add(const std::string& path, bool dir);

    // Sessions (appsession.cpp): what was open in each project folder (the
    // tabs and splits, each file's cursor and view, the side panel, the
    // Build tab's task), kept beside `recent` and brought back when the
    // folder is opened again. Files are read and written on threads.
    struct SessionUi;
    struct Session;
    void session_create();     // init: the Recent folders' tab counts, read on a thread meanwhile
    bool project_open(const std::string& dir);  // a folder opened to work in: the last one's session kept, this one's back (true: it came back)
    void worktree_open(const std::string& dir, const std::string& from);  // another working tree, so: with no session, from's tabs at their places in it
    void session_save();       // the project folder's, as things are now (on quit, and when the folder changes)
    int session_tabs(const std::string& dir, bool wait = false);  // how many tabs its session brings back; -1: none
    bool session_arrived(Pane& p, Doc& d);      // a file from another machine is in its tab (true: one a session opened)
    bool session_dropped(u64 doc, const std::string& why);  // one that could not be read (why: "" not there): a session's goes quietly
    std::string session_dump();
    std::string session_text();
    void session_apply(Session& st);
    void panel_session(bool* open, std::string* tab, int* cols) const;  // (apppanel.cpp)
    void panel_session_set(bool open, const std::string& tab, int cols);
    std::string build_chosen() const;  // (appbuild.cpp) the Run button's task
    void build_choose(const std::string& key);
    std::shared_ptr<SessionUi> session_;
    bool restoring_ = false;  // files opened by a session do not go into `recent`
    u64 session_gen_ = 0;     // counts up whenever a Recent folder's tab count may have changed

    // Files on other machines (appremote.cpp): read on a thread of their
    // own, while their tab is already there.
    struct RemoteUi;
    void remote_start();
    void remote_load(const Doc& d, bool reload);
    void remote_sync();
    bool remote_busy() const;  // (scripted runs: something is still on its way)
    void find_remote_ready();
    void remote_opened(Pane& p, Doc& d, bool fresh);
    void remote_reconnect();
    // Saving one: the text as it is, written there on a thread of its own
    // while typing goes on; closing its tab, or quitting, waits for that in a
    // box that says so (true: it waits; Esc calls the close or quit off).
    void remote_save(Doc& d, bool over = false);  // (over: written even if it has changed there since it was read)
    void remote_compare(const Doc& d);           // read there for "Compare with disk"
    bool remote_saving(u64 doc) const;       // a save of it is under way
    bool remote_saving_all(const Doc& d) const;  // and nothing but that has changed it
    void remote_saved();
    void remote_forget(u64 doc);  // its text is about to go: a save of it takes its own copy first
    bool remote_hold_close(size_t i);
    bool remote_hold_quit(bool asked);
    void remote_hold_box(u8 what, u64 doc);
    // A folder there, for the box that opens files: true with its entries,
    // false while they are on their way (find_remote_ready() is called when they come).
    bool remote_listing(const std::string& dir, std::vector<RemoteEntry>* out);
    bool remote_listing_busy() const;
    // An errand to another machine for the main thread (a file looked for
    // there, a report read back), run on a thread of its own in the order
    // asked: `work` returns what the main thread then does (remote_sync).
    using RemoteErrand = std::function<std::function<void()>(std::stop_token)>;
    void remote_errand(RemoteErrand work);
    std::string remote_unreached() const;  // the machine of what was opened while it could not be reached ("": none)
    void remote_unreached_drop(const std::string& opened);  // something else opened: that one is not opened when it can be
    bool panel_busy() const;  // (apppanel.cpp) the file tree is reading folders, or changing another machine's files

    // Profiling (appprofile.cpp): the side panel's Profile tab, a tab for
    // each profile (a flame graph and the functions), inline performance labels.
    struct ProfUi;
    void prof_start();
    void prof_record();
    void prof_stop();
    void prof_sync();
    void prof_open(std::shared_ptr<Profile> p);
    std::string prof_kept_folder() const;
    void prof_list_kept();
    void prof_keep(const std::shared_ptr<Profile>& p);
    void prof_open_kept(const std::string& path);
    void prof_thread(struct ProfView& v, int i);
    void prof_take_profile(std::shared_ptr<struct Profile> p);
    void prof_compare(std::shared_ptr<struct Profile> before, std::shared_ptr<struct Profile> after);
    void prof_compare_pick();
    void prof_narrow(struct ProfView& v);
    void prof_mouse_up();
    void prof_set_stretch(double t0, double t1);
    void prof_view_init(struct ProfView& v);
    void prof_flip(struct ProfView& v);
    void prof_relations(struct ProfView& v);
    bool prof_pick(const std::string& section, u32 n);
    void prof_do(u32 row);
    void prof_panel_draw(struct TextBox& box, bool focused);
    void prof_panel_click(int row, int col, const SDL_MouseButtonEvent& b);
    bool prof_panel_key(const SDL_KeyboardEvent& k);
    void prof_layout(FrameDesc& fd);
    void prof_marks(FrameDesc& fd);
    bool prof_hit(int px, int py, Pane** pane, u32* row, u32* col);
    bool prof_mouse_down(const SDL_MouseButtonEvent& b);
    bool prof_mouse_motion(const SDL_MouseMotionEvent& m);
    bool prof_wheel(const SDL_MouseWheelEvent& w);
    bool prof_key(const SDL_KeyboardEvent& k);
    void prof_go_to(const Profile& p, u32 func);
    u64 prof_definition(const Doc& d, const ProfileFunc& f) const;
    void prof_set(const std::string& what, const std::string& value);
    bool prof_busy() const;
    std::string prof_dump();
    void panel_show_profile();

    // The Markdown preview (appmarkdown.cpp, markdown.cpp, diagram.cpp): a tab
    // beside a Markdown file that shows it drawn, Mermaid diagrams and all.
    void md_open();
    // A file's assembly beside it (appasm.cpp).
    void asm_open();
    void asm_ask(Doc& d);
    void asm_saved(const std::string& path);
    void asm_marks(FrameDesc& fd);
    std::string asm_dump();
    void asm_take();
    std::shared_ptr<struct AsmWork> asm_work_;
    std::vector<GridDeco> asm_decos_[kMaxViews];
    std::array<u64, 4> asm_seen_{};
    void md_layout(FrameDesc& fd);
    bool md_hit(int px, int py, Pane** pane, u32* row, u32* col);
    bool md_key(const SDL_KeyboardEvent& k);
    bool md_wheel(const SDL_MouseWheelEvent& w);
    bool md_mouse_down(const SDL_MouseButtonEvent& b);
    std::string md_dump(int how = 0);  // (for the tests: its rows as text; 1: with each run's colour; 2: and its style, tone and link)
    bool md_link_point(std::string_view link, float* x, float* y);
    std::string md_top_dump();
    static bool md_notebook(const Doc& d);  // (a notebook's view)
    u32 nb_view_room(const Doc& src);
    std::string md_diagram_dump();  // (for the tests: where the preview's diagrams' parts are)
    void md_set_today(i64 day);     // (for the tests: the date diagrams are drawn for)
    struct MdWorker;  // draws previews on a thread of its own, so that typing never waits for one
    void md_sync();   // (event 20: a preview has been drawn)
    void md_wait();   // (for the tests: until every preview asked for is drawn)
    void md_unmade();  // (the diagrams' pictures no preview names, let go of)

    // Pictures (appimage.cpp): image files read on a thread of their own and
    // put on the GPU, for an image file's tab and the Markdown preview.
    struct Pictures;
    static bool is_picture_path(std::string_view path);
    static bool is_svg_path(std::string_view path);
    void svg_preview();   // (appimage.cpp) an SVG's picture beside its text
    void preview_open();  // Cmd+Shift+V: that, or the Markdown preview
    const PictureInfo* picture(const std::string& path);
    // A picture made in memory (a diagram's, by the Markdown preview), by a
    // name of its own: put on the GPU the first time it is asked for (and
    // again if it has been let go).
    const PictureInfo* picture_made(const std::string& name, const u8* rgba, int w, int h);
    // Those made pictures that `named` says no preview names any more, let
    // go of now (on the GPU too); how many made ones are left.
    size_t pictures_unmade(const std::function<bool(const std::string&)>& named);
    // What to draw of it now, shown w by h pixels: an animation's frame (its
    // next asked for when it is due), a drawing from shapes made again at
    // that size if it was made at another.
    u32 picture_gpu(const std::string& path, double w, double h);
    void picture_due(u64 at_ns);  // the timer that wakes the loop for an animation's next frame
    void picture_sync();
    void picture_recheck();
    void picture_tidy();
    void picture_wait();
    bool pictures_busy() const;
    bool image_open(const std::string& full);
    void image_layout(FrameDesc& fd);
    bool image_hit(int px, int py, Pane** pane);
    bool image_key(const SDL_KeyboardEvent& k);
    bool image_wheel(const SDL_MouseWheelEvent& w);
    bool image_pinch(float by);
    bool image_mouse_down(const SDL_MouseButtonEvent& b);
    bool image_mouse_motion(const SDL_MouseMotionEvent& m);
    void image_mouse_up();
    std::string image_dump();
    std::string picture_frames(int ms);  // (for the tests: frames drawn in `ms` milliseconds of the main loop)

    // PDFs (apppdf.cpp): in a tab of their own, pages drawn on a thread of their own.
    struct Pdfs;
    static bool is_pdf_path(std::string_view path);
    bool pdf_open(const std::string& full);
    void pdf_sync();
    void pdf_recheck();
    void pdf_layout(FrameDesc& fd);
    bool pdf_hit(int px, int py, Pane** pane);
    bool pdf_key(const SDL_KeyboardEvent& k);
    bool pdf_wheel(const SDL_MouseWheelEvent& w);
    bool pdf_pinch(float by);
    bool pdf_mouse_down(const SDL_MouseButtonEvent& b);
    bool pdf_mouse_motion(const SDL_MouseMotionEvent& m);
    void pdf_mouse_up();
    bool pdfs_busy() const;
    void pdf_wait();
    std::string pdf_dump();
    std::string pdf_dump_more(std::string_view what);
    std::string pdf_page_rect(int page);
    bool pdf_text(const char* text);  // a password typed, or what to find
    void pdf_find(Doc& doc);
    void pdf_copy(Doc& doc);

    // Terminals (appterm.cpp): a tab running a shell, here or on another machine.
    // A terminal tab in dir (the working folder when none): a login shell, or
    // `command` run by sh, whose tab stays when it ends and says how (and
    // keeps `title`). Its document's id; 0 if it could not start.
    u64 term_open(const std::string& dir = {}, const std::string& command = {}, const std::string& title = {}, std::string_view hint = {}, bool console = false);
    // The Python console, in the terminals' split (appterm.cpp).
    // python_for: the Python a folder's code runs with, as words for a
    // shell (the console's: IPython where the folder's environment has it;
    // else, and for a notebook's kernel, Python).
    static std::string python_for(const std::string& root, bool console = true);
    TermView* py_console(bool keyboard);
    void py_send(std::string code);
    void py_flush(TermView& v);
    void py_send_lines();
    // Jupyter notebooks (appnotebook.cpp).
    bool nb_load(Doc& d);
    bool nb_save(Doc& d, std::string* err);
    std::string nb_json(Doc& d);
    bool nb_new(Doc& d);
    bool nb_run_cell();
    bool nb_key(const SDL_KeyboardEvent& k);
    void nb_marks(FrameDesc& fd);
    std::string nb_dump();
    std::string nb_cells_dump();
    bool nb_model(const Doc& d, u64* gen, std::shared_ptr<const std::vector<NotebookCell>>* cells = nullptr, std::string* lang = nullptr) const;
    bool nb_set_outputs(Doc& d, size_t cell, std::string_view outputs, i64 count);
    // Its kernel (kernel.hpp): cells run in it, their outputs put in the model.
    Doc* nb_of(Doc& d);
    void nb_queue(Doc& d, const std::vector<size_t>& cells);
    bool nb_start(Doc& d);
    void nb_next(Doc& d);
    void nb_run_all();
    void nb_interrupt();
    void nb_restart();
    void nb_sync();
    int nb_wait_ms() const;  // (a running cell's outputs, put in the model a few times a second: until the next)
    void nb_tick();
    void nb_forget(const Doc& d);
    bool nb_kernel_wait(bool started);
    std::string nb_kernel_dump();
    std::string nb_kernel_status(Doc& d, bool* lost);
    std::string nb_host_down(const Doc& d);
    void nb_connections();  // (a connection has come up or gone: a kernel that went with it, said)
    // Its view (appmarkdown.cpp): the notebook drawn beside its script.
    void nb_view_toggle();
    void nb_view_close(u64 source);
    std::string nb_view_dump();
    bool nb_view_point(u32 row, float* x, float* y);
    std::vector<GridNote> nb_notes_[kMaxViews];
    u64 nb_spent_ns_ = 0;  // (for the tests: the main thread's time taking in what kernels said)
    // The terminals' split along the bottom, the one the build's output
    // uses too: Ctrl+` shows it with the keyboard on its terminal, and
    // with the keyboard there hides it and gives the keyboard back to the
    // text (its programs keep running); Ctrl+Shift+` opens another there.
    struct TermDock;
    TermDock& term_dock();
    void term_toggle();
    void term_new();
    Pane* dock_show(Pane* above);  // on show, brought back, or made below `above` (nullptr: no room)
    Pane* dock_pane() const;       // on show: the split; else nullptr
    void dock_hide();
    bool term_may_close(size_t i);  // the tab's program still running: asked first (false: the user said no)
    size_t term_busy_count();       // programs still running in terminals, hidden ones too
    static const std::string& term_folder(const TermView& v);  // where it was started (ssh://host/... there)
    void term_sync();
    void term_layout(FrameDesc& fd);
    void term_pictures(FrameDesc& fd, TermView& v, int x, int y, int w, int h);
    void term_copy(TermView& v);
    void term_paste(TermView& v);
    bool term_key(const SDL_KeyboardEvent& k);
    bool term_text(const char* text);
    bool term_hit(int px, int py, Pane** pane, u64* line, u32* col);
    bool term_mouse_cell(struct TermView& v, int px, int py, u32* row, u32* col);
    bool term_mouse_down(const SDL_MouseButtonEvent& b);
    bool term_mouse_motion(const SDL_MouseMotionEvent& m);
    void term_mouse_up();
    bool term_wheel(const SDL_MouseWheelEvent& w);
    std::string term_dump();
    bool term_wait(const std::string& text);
    bool term_go_place(TermView& v, u64 line, u32 col);  // Cmd+click: the path:line there opened
    bool term_click_text(const std::string& text);       // (tests) Cmd+click on this text in a terminal on show
    void term_end();

    // Every command and its keys (appcommands.cpp): the command search, and
    // the keys for what had none.
    struct Commands;
    struct PaletteItem {
        std::string title, keys;
        u32 id = 0;
    };
    static constexpr u32 kMappingItem = 0x40000000u;  // (a PaletteItem's id: a script's mapping of one chord, by its place)
    void commands_create();
    bool command_key(const SDL_KeyboardEvent& k);
    std::vector<PaletteItem> palette_items();
    // The key that does a command now (its title whole, else the first that
    // starts so): the first of its keys that Vim would not take (the tests'
    // keyof:). False if there is no such command or it has no keys.
    bool command_key_now(std::string_view title, SDL_KeyboardEvent* out);
    void palette_open();
    void palette_keys();  // appfind.cpp: the command search, looking up the next keys pressed
    void palette_run(u32 id);
    static std::string chord_text(SDL_Keycode key, SDL_Keymod mod);
    void tab_shift(int by);
    void prof_toggle_labels();

    // Debugging (appdebug.cpp, dap.hpp): breakpoints, a session, the Debug tab.
    struct DebugUi;
    void debug_create();
    bool debug_running() const;
    void debug_toggle_break();
    void debug_edit_break();
    void debug_send_breaks(const std::string& path);
    u32 debug_gutter(const Doc& d, u64 line);
    std::vector<u64> debug_break_lines(const Doc& d);  // (the file's breakpoints' lines, from 0: the scroll bar's marks)
    void debug_start();
    // A session's start (appdebug.cpp): the debugger started, then told to
    // launch, to attach to a process, or to open a core file.
    bool debug_open(const std::string& near, const std::string& dir, bool python);
    void debug_begin(const std::string& request, const std::string& arguments);
    void debug_attach_pick();
    void debug_attach(i64 pid);
    void debug_core_pick();
    void debug_core(const std::string& program, const std::string& core);
    void debug_send_functions();
    void debug_break_function();
    void debug_set_value(u32 var);
    void debug_watch_writes(u32 var);
    void debug_send_data();
    void debug_run_to_cursor();
    void debug_jump_to_cursor();
    bool debug_disasm_front() const;
    Doc* debug_tab(u64& id, const char* title);
    void debug_tab_fill(Doc& d, std::string text, std::vector<u32> off, std::vector<u8> style, i64 line, u64 col);
    void debug_disasm_open();
    void debug_disasm_ask(bool from_ip = false);
    void debug_send_instructions();
    void debug_memory_pick();
    void debug_memory_at(const std::string& expr, const std::string& name);
    void debug_memory_open(u64 addr, const std::string& name);
    void debug_memory_show(u64 addr, std::vector<i32> now, i64 line, i32 byte);
    void debug_memory_page(int by);
    void debug_memory_ask(i64 line, i32 byte);
    void debug_memory_write();
    void debug_end();
    void debug_park();
    void debug_switch(size_t i);
    void debug_each_other(const std::function<void()>& fn);
    void debug_start_another();
    void debug_sync_session(bool* stopped);
    void debug_command(const std::string& what);
    void debug_frame(u32 i, bool show);
    void debug_stack();
    void debug_thread(u32 i);
    void debug_name_waves();
    void debug_watch_eval();
    void debug_expand(u32 i);
    void debug_sync();
    void debug_evaluate(const std::string& start = {});
    void debug_complete(const std::string& typed);
    void debug_panel_draw(struct TextBox& box, bool focused);
    void debug_do(u32 i);
    void debug_panel_click(int row, int col, const SDL_MouseButtonEvent& b);
    bool debug_panel_key(const SDL_KeyboardEvent& k);
    void debug_marks(FrameDesc& fd);
    void debug_set(const std::string& what, const std::string& value);
    void debug_wait(const std::string& state);
    std::string debug_dump();
    std::string debug_breaks_dump();
    std::string debug_inline_dump();  // (the notes after the lines of the first view, as last drawn)
    bool debug_pick(const std::string& section, u32 n, const std::string& name = {});
    void panel_show_debug();

    // Building (appbuild.cpp, build.cpp): what the working folder builds
    // with, the Build tab, the Run button at the top right, and the output
    // terminal below the text that runs a task.
    struct BuildUi;
    void build_create();
    // The Tests tab, test runs and their marks in the text (apptest.cpp).
    struct TestUi;
    void test_create();
    void test_find();
    void test_sync();
    void test_run(const std::vector<size_t>& which, bool coverage = false);  // (none: all of them; coverage: and what lines they ran)
    void test_run_failed();
    void test_run_here();
    void test_debug(size_t i);
    void test_debug_here();
    void test_ended(u64 doc);
    void test_take(std::vector<std::string> texts);  // (the run's reports, as read)
    int test_at_cursor();
    const std::vector<std::pair<u32, int>>& test_defs(const Doc& d);
    u32 test_gutter(const Doc& d, u64 line);
    bool test_gutter_click(const SDL_MouseButtonEvent& b);
    std::vector<size_t> test_row_tests(u32 row);
    void test_panel_draw(struct TextBox& box, bool focused);
    void test_panel_click(int row, int col, const SDL_MouseButtonEvent& b);
    bool test_panel_key(const SDL_KeyboardEvent& k);
    std::string test_dump();
    const struct FileCoverage* test_coverage(const Doc& d);  // (the lines a run with coverage ran in it, while shown and the file is as it was)
    void test_coverage_toggle();
    std::string coverage_dump();
    void panel_show_tests();
    std::string build_root() const;
    void build_follow();   // after each frame: the folder looked at, when it changed
    void build_sync();     // what the worker that looked found
    void build_changed();  // look again (init.lua's systems changed, or Cmd+R)
    void build_settle();   // looked at now, and waited for
    std::vector<BuildTask> build_tasks();
    u64 build_command(const std::string& dir, const std::string& cmd, const std::string& title);
    void build_run(int index);
    void build_option(int row);  // a task of the Build tab's (-1: the Run button's)
    bool build_run_named(const std::string& name);
    void build_stop();
    void build_ended(u64 doc, int status);
    void build_panel_draw(struct TextBox& box, bool focused);
    void build_panel_click(int row, int col, const SDL_MouseButtonEvent& b);
    bool build_panel_key(const SDL_KeyboardEvent& k);
    void build_measure();
    int build_reserve(const Pane& p) const;  // the room the Run button takes from p's tab bar
    void build_layout(int w, FrameDesc& fd);
    bool build_mouse_down(const SDL_MouseButtonEvent& b);
    std::string build_dump();
    void build_wait();
    void panel_show_build();
    // What the last run's output said was wrong (buildscan.hpp): taken from
    // its terminal as it comes (term_sync), shown in the Problems tab's
    // Build section, counted on the Run button and in the status bar.
    void build_found(u64 doc, std::vector<BuildProblem> found);
    const std::vector<BuildProblem>& build_problems() const;
    std::string build_label() const;  // the Run button's words
    void build_go(size_t i);    // that one's file, at its line
    bool build_step(int by);    // the next (1) or previous (-1) of them; false: there are none
    void problem_step(int by);  // F8 / Shift+F8 (apppanel.cpp): the build's problems, else the open file's
    void panel_problem_seen(size_t i);  // (apppanel.cpp) the Problems tab's row for the one gone to, picked
    bool go_to_place(const std::string& path, u32 line, u32 col);  // a file at a line (from 1) and column (from 1; 0: none), in a split with text
    void show_place(u64 line, u64 col);  // (appfind.cpp) the file in front at a line and a byte in it (from 0), the line in the middle of the view
    std::string problems_dump();         // (apppanel.cpp) the Run button's words and the Problems tab's rows
    // What init.lua adds (applua.cpp).
    std::vector<std::string> script_hidden_builds() const;
    void script_build(const std::string& root, const std::vector<FsEntry>& files, std::vector<BuildTask>* tasks);
    std::vector<ProjectKind> script_project_kinds();  // the built-in kinds it kept, and its own
    bool script_project_create(const std::string& kind, const std::string& dir, const ProjectValues& values, std::string* open, std::string* then, std::string* err);
    void lsp_forget_failed(const std::string& suffix);  // (applsp.cpp) servers that failed there may be tried again

    // The status bar along the bottom (appstatus.cpp).
    struct StatusUi;
    int status_px() const;
    void status_layout(int w, int h, FrameDesc& fd);
    bool status_mouse_down(const SDL_MouseButtonEvent& b);
    std::string status_dump() const;
    bool status_click(const std::string& part);  // (tests) a click on the part of the bar that starts with this
    // What a click on the indentation (0), the line ends (1) or the language
    // (2) opens: a list that changes it for the file in front.
    void status_pick(int what);
    void set_line_ends(Doc& d, bool crlf);
    void set_language(Doc& d, const std::string& lang);
    // What the status bar says of the repository (appgit.cpp).
    struct GitSummary {
        bool repo = false, merging = false;
        std::string root, branch, head;
        int ahead = 0, behind = 0;
        size_t changed = 0;  // files that differ from the last commit, or are new
    };
    GitSummary git_summary() const;
    void vim_marks(FrameDesc& fd);
    std::string vim_dump();
    std::string vim_line_dump() const;

    // Git (appgit.cpp).
    struct GitUi;
    void git_create();
    void git_follow();
    void sync_git();
    void git_want(Doc& d);
    bool blame_shown(const Doc& d) const { return cfg_.git && d.blame_on; }
    int git_wait_ms() const;
    void git_tick();
    void git_do(u32 action, const std::string& arg = {});
    void git_open_text(std::vector<std::string> args, const std::string& title);
    void reload_changed();
    void reread(Pane& p, Doc& d, const std::string* bytes);
    void git_note(FrameDesc& fd);
    bool confirm(const std::string& what, const char* yes);
    void git_rows();
    void git_panel_draw(struct TextBox& box, bool focused);
    bool git_panel_key(const SDL_KeyboardEvent& k);
    bool git_panel_text(std::string_view text);
    void git_panel_click(int row, int col, const SDL_MouseButtonEvent& b);
    void git_panel_wheel(float dy);
    void panel_show_git();
    void panel_show_problems();
    const std::string* panel_root() const;  // the working directory, once there is one; else null
    std::string git_dump();
    void git_command(std::string_view name, const std::string& arg);  // an action by its name ("stage", "push")
    // Diff tabs whose changes can be staged a hunk or a line at a time, in
    // one column or two; and the offer a change's mark in the gutter makes.
    void git_open_view(std::vector<std::string> args, const std::string& title, u8 kind, const std::string& path, u32 goto_line = 0);
    void git_view_ask(Doc& d, bool again);
    void git_view_set(Pane& p, Doc& d, Git::Result& r);
    void git_view_want(Doc& d);
    void git_view_note(FrameDesc& fd);
    void git_offer_layout(FrameDesc& fd);
    bool git_view_key(const SDL_KeyboardEvent& k);  // (appcommands.cpp's command_key asks first)
    bool git_gutter_click(const SDL_MouseButtonEvent& b);  // (apptabs.cpp's tabs_mouse_down asks first)
    std::string git_graph_dump();
    void git_rebase_open(const std::string& base);
    void git_rebase_lay(Doc& d);
    bool git_rebase_key(const SDL_KeyboardEvent& k);
    void git_rebase_start();
    std::string git_rebase_dump();
    void git_merge_open(const std::string& full);
    void git_merge_lay(Doc& d, i64 chunk);
    bool git_merge_key(const SDL_KeyboardEvent& k);
    void git_merge_take(u8 take, bool all_simple);
    void git_merge_save();
    std::string git_merge_dump();
    std::string git_searched() const;  // what History is searched for
    std::string git_upstream() const;  // the branch the one checked out follows, if any
    void git_history(bool lines);
    void git_compare_pick();
    void git_compare(const std::string& rev, const std::string& rel);  // the file's commits, or the chosen lines', in a diff tab
    std::string git_commit_at(const Doc& d);
    void git_stage_here(bool stage);  // the hunk or the lines chosen at the cursor, in a diff tab or a file
    void git_view_toggle();           // a diff tab in two columns, or back in one
    void git_message_changed();
    std::string git_view_dump();
    std::string git_field_dump();

    // Splits (appsplit.cpp).
    struct Drag;
    void splits_create();
    Pane* add_pane();
    void place_panes(int w, int h);
    void place_node(SplitNode& n, int x0, int y0, int x1, int y1);
    void for_each_pane(std::function_ref<void()> fn);  // runs fn with each pane as the focused one in turn
    Pane* pane_at(int px, int py) const;
    void focus_pane(Pane* p);
    enum Edge : u8 { kEdgeLeft, kEdgeRight, kEdgeTop, kEdgeBottom, kEdgeNone };
    void tab_to_split(Edge edge);
    void split_grow(bool across, int by);
    Pane* split_pane(Pane* target, Edge edge);  // a new pane with no files yet, on that side of target
    void split_close();                         // Cmd+Shift+W: the split with the keyboard goes, its tabs to the one beside it
    void remove_pane(Pane* p);                  // takes it out of the arrangement and lets it go
    std::unique_ptr<Pane> take_pane(Pane* p, float* room);  // takes it out and keeps it whole (room: the share it had)
    bool put_pane(std::unique_ptr<Pane>& p, Pane* target, Edge edge, float room);  // puts it back beside target
    void split_room(Pane* p, float room);       // the share of the room it and its other half have that it gets
    void move_doc(Pane* from, size_t index, Pane* to, size_t at);
    void focus_towards(Edge edge);
    bool splits_key(const SDL_KeyboardEvent& k);
    bool splits_mouse_down(const SDL_MouseButtonEvent& b);
    bool splits_mouse_motion(const SDL_MouseMotionEvent& m);
    bool splits_mouse_up(const SDL_MouseButtonEvent& b);
    void splits_layout(int w, int h, FrameDesc& fd);
    void splits_carry_layout(int w, int h, FrameDesc& fd);  // the name of what is being dragged, by the mouse: over everything
    // Something picked up with the mouse: a divider, or a tab, a group of
    // tabs or a file from the tree on its way to another split.
    enum DragKind : u8 { kDragNone, kDragDivider, kDragTab, kDragGroup, kDragFile };
    void drag_arm(DragKind kind, u64 doc, const std::string& path, float x, float y);
    void drag_drop();
    void drag_cancel();  // every drag ends where it is, nothing dropped
    void wheel_pane(const SDL_MouseWheelEvent& w);
    void forget_doc(const Doc* d);  // a document is about to go: nothing may still point at it
    void layout_pane(FrameDesc& fd, bool focused);

    // Line numbers (apptabs.cpp).
    int gutter_px() const;
    void gutter_layout(int w, int h, FrameDesc& fd);
    std::string gutter_dump();  // (the tests: each row of the split in front's line numbers, and its change mark)

    // The side panel: tabs, the file tree (apppanel.cpp).
    struct Side;
    void panel_create();
    int panel_px() const;
    void panel_apply_config(bool at_start);
    std::string default_workdir() const;
    void panel_set_workdir(const std::string& path);
    void panel_toggle();
    void panel_focus(bool on);
    void panel_refresh();
    bool panel_has_keyboard() const;
    // The tab the side panel shows, in the order of its tabs: 0 Files,
    // 1 Problems, 2 Git, 3 Profile, 4 Debug, 5 Build.
    u32 panel_tab() const;
    void sync_panel();
    void panel_select(int row);
    void panel_activate(const std::string& path);
    void panel_begin_input(u8 kind);
    void panel_commit_input();
    void panel_trash(const std::string& path);
    void panel_menu_do(u32 item);
    bool panel_key(const SDL_KeyboardEvent& k);
    bool panel_text(std::string_view text);
    bool panel_mouse_down(const SDL_MouseButtonEvent& b);
    bool panel_mouse_motion(const SDL_MouseMotionEvent& m);
    void panel_mouse_up();
    bool panel_wheel(const SDL_MouseWheelEvent& w);
    bool dark() const;  // the system in its dark appearance (app.cpp)
    void tell(SDL_MessageBoxFlags kind, const char* what);
    void panel_layout(int w, int h, FrameDesc& fd);
    void panel_menu_layout(int w, int h, FrameDesc& fd);

    // Scripts in Lua (applua.cpp): init.lua, plugins, and what they set up.
    struct ScriptUi;
    friend struct ScriptApi;
    bool script_ensure();  // a Lua state, made now if there is none (for :lua)
    void script_start();   // at startup: init.lua and the plugins, if there are any
    void script_started(bool fresh);
    void script_shutdown();
    void script_restart();
    void script_stop();    // at the end: VimLeavePre, then everything goes
    bool script_call(int nargs, int nres, const char* what);
    bool script_file(const std::string& path);
    bool script_string(std::string_view code, const char* name);
    bool script_api(const char* name);
    bool script_lib(const char* table, const char* name);  // blit[table][name], a function, on the stack
    void script_plugins();
    void plugin_load(const std::string& name);
    void script_run_dir(const std::string& dir);
    void script_run(std::vector<std::string> argv, std::string cwd, std::function<void(int, const std::string&, const std::string&)> done);
    void plug_install();
    void plug_update();
    void plug_list();
    std::string script_set(const std::string& key, const std::string& value, const std::string& from);
    void script_set_command(const std::string& arg);
    bool set_complete(int by);  // Tab (1) or Shift+Tab (-1) on a ":set" command line
    // Choose a theme: every one tried as the choice comes to it, Esc puts
    // the one there was back, Enter writes it into init.lua (in its tab,
    // not saved) for the system's appearance, light or dark.
    void theme_choose();
    void theme_write(bool dark, const std::string& name);
    // 0 information, 1 a warning, 2 an error; `detail` (a traceback) goes to :messages and the standard error, not the box.
    void notify(const std::string& text, int level = 0, const std::string& detail = {});
    void keep_message(const std::string& text);
    char script_mode_char() const;
    bool script_key(const SDL_KeyboardEvent& k);
    bool script_text(const char* text);
    void script_dropped(const char* text);
    std::string script_pending_keys() const;
    bool script_take(ScriptKey p);
    bool script_match(bool late);
    void script_replay(const ScriptKey& p);
    void script_run_map(const Keymaps::Map& m);
    bool script_feed(std::string_view notation, bool remap, std::string* err);
    void typed_text(const char* text);  // typed text, wherever it goes
    void on_text(const char* text);     // typed text into the document
    void script_mask();
    std::string doc_language(const Doc& d) const;
    void script_event(u32 event, u64 doc, std::string file = {}, std::string match = {});
    void script_fire(u32 event, u64 doc, const std::string& file, const std::string& match, int data = 0);
    bool script_write(bool after, u64 doc);  // false: the document went meanwhile
    void script_opened(u64 doc, bool fresh);
    void script_filetype(u64 doc);  // its language was chosen: FileType again
    void script_closed(const Doc& d);
    void script_focus(bool gained);
    void script_saved(const std::string& path);
    void script_dir(const std::string& dir);
    void script_ex(const VimRequest& q);
    void script_chose(bool chosen, int index, const std::string& typed);
    void script_tick();
    void script_tick_queue();
    void script_watch();
    void script_expire();
    int script_wait_ms() const;
    void script_jobs();
    void script_close_floats();
    void script_replace(Doc& d, u64 start, u64 end, std::string_view text);
    void script_group(Doc& d);
    void script_layout(int w, int h, FrameDesc& fd);
    std::string script_dump();
    bool script_busy() const;  // programs running, timers waiting (not ones that repeat), keys held
    void apply_config(Config next);
    std::shared_ptr<ScriptUi> script_;  // null until there is a script (or a message)
    std::vector<ConfigSetting> script_settings_;  // made on top of the settings file's
    bool script_keys_ = false;       // some mapping is set, or keys are held for one: every key is looked at
    bool script_drop_text_ = false;  // a press was taken by a mapping: the text it types goes with it
    bool script_modes_ = false;      // some handler is for a change of mode: each one is noted as Vim makes it
    void script_mode_seen();
    std::string script_status_;      // what a script put at the right of the bottom line

    // Benchmarks and screenshots (appbench.cpp): Debug builds only.
    int run_bench();
    int run_shot();
    void run_script(const char* script);
    friend struct Bench;

    static bool SDLCALL event_watch(void* self, SDL_Event* e);

    Options opt_;
    SDL_Window* win_ = nullptr;
    Renderer gpu_;
    std::shared_ptr<Pictures> pictures_;  // (after gpu_: its reader stops first)
    u64 pictures_gen_ = 0;  // counts up whenever a picture has been read (or could not be)
    std::shared_ptr<Pdfs> pdfs_;  // (after gpu_ too)
    std::shared_ptr<MdWorker> md_worker_;
    std::unique_ptr<Font> font_;
    // The splits of the window, how they are arranged, and the one with
    // the keyboard. There is always at least one, and every one has at
    // least one file. (lsp_ is declared after them: it must let go of a
    // document's text before the document goes.)
    std::vector<std::unique_ptr<Pane>> panes_;
    std::unique_ptr<SplitNode> split_;
    std::shared_ptr<TermDock> dock_;  // (after panes_: a hidden terminals' split goes first)
    Pane* pane_ = nullptr;
    u64 next_doc_id_ = 1, next_pane_id_ = 1;
    Lsp lsp_;
    Doc* lsp_doc_ = nullptr;  // the document the language server is following, if any
    u64 lsp_marks_gen_ = 0;   // counts up whenever lsp_gutter()'s marks may have changed
    Git git_;                 // (after the documents too: its worker reads their text)
    std::shared_ptr<GitUi> gitui_;
    Vim vim_;                 // modal editing, for the document in front
    u64 vim_doc_ = 0;         // which document that was when Vim last looked
    u64 vim_pane_ = 0;        // and in which split (a file two splits show: its selection is drawn in that one)
    std::shared_ptr<VimUi> vimui_;
    std::shared_ptr<StatusUi> status_;
    std::shared_ptr<StartUi> start_;
    std::shared_ptr<Commands> commands_;
    bool command_sending_ = false;  // a command's keys sent by the command search: the editor's, never Vim's
    std::shared_ptr<DebugUi> debugui_;
    std::shared_ptr<BuildUi> buildui_;
    std::shared_ptr<TestUi> testui_;
    u64 test_gen_ = 1;  // counts up whenever what the Tests tab shows may have changed
    u64 build_gen_ = 1;  // counts up whenever what the Build tab or the Run button shows may have changed
    u64 build_prob_gen_ = 1;  // counts up whenever the build's problems change
    u32 build_errors_ = 0, build_warnings_ = 0;  // how many of each (the status bar asks at every key: not behind a pointer)
    u64 debug_gen_ = 1;  // counts up whenever breakpoints, the Debug tab or the stopped line may have changed
    std::vector<GridNote> debug_notes_[kMaxViews];
    std::shared_ptr<ProfUi> profui_;  // (before remote_: a recording on another machine ends when its connection does)
    u64 prof_gen_ = 1;                // counts up whenever what the Profile tab shows may have changed
    bool term_drop_text_ = false;     // the text a key sent to a terminal as itself also brings is not typed again
    std::string asked_;               // (scripted runs: the last question there was nobody to ask, for the tests)
    std::shared_ptr<RemoteUi> remote_;
    u64 remote_gen_ = 0;      // counts up whenever a connection or a file on its way has news
    u64 git_gen_ = 1;         // counts up whenever what the Git tab shows may have changed
    u64 git_due_ns_ = 0;      // when to look again whether a blame is wanted (0: no need)
    std::unique_ptr<Assist> assist_;  // popups and squiggles; see applsp.cpp
    GrammarInstaller grammars_;
    std::shared_ptr<Side> side_;  // the side panel; see apppanel.cpp
    std::shared_ptr<Drag> drag_;  // a divider, tab, tab group or file being dragged; see appsplit.cpp
    u32 focus_view_ = 0;          // which of the frame's text views is the focused pane's
    float mouse_x_ = -1, mouse_y_ = -1;  // where the mouse was last seen, window coordinates
    Config cfg_;
    std::string init_path_;  // init.lua: the settings, mappings and the rest (it need not be there)
    Theme theme_{};

    float scale_ = 1;      // pixels per point of text
    float density_ = 1;    // pixels per window coordinate
    float pt_ = 13;
    int px_w_ = 0, px_h_ = 0;
    int pad_x_ = 0;

    bool animating_ = false;
    u64 last_step_ns_ = 0;
    u64 precise_until_ns_ = 0;

    // Frames are never drawn faster than the display shows them: this is
    // the display's refresh period, when the last frame began, and whether
    // the window system held that frame back itself (see frame()).
    u64 frame_period_ns_ = 0, last_frame_ns_ = 0;
    bool last_frame_paced_ = true;

    u64 frames_ = 0;
    bool running_ = true;
    bool dirty_ = true;
    bool find_open_ = false;  // the box that finds things is up (here, not in its own state: every key and every frame asks)
    bool dragging_ = false;
    bool dragging_thumb_ = false;
    bool text_press_ = false;  // the text area took the left-button press that is held
    u32 guide_inks_ = 0;  // how many colours the theme has for indent lines
    u64 theme_gen_ = 0;   // counts up each time pick_theme() sets the colours (for what is drawn from them and kept)
    bool quit_after_save_ = false;
    bool close_after_save_ = false;  // the tab whose new file is being named closes once it is saved
    bool lsp_pending_ = false;  // start a language server after the next frame
    bool grammars_pending_ = false;  // and see whether a grammar should be fetched
    u64 grammars_gen_ = 0;           // counts up whenever a grammar has been installed (the status bar's "no colours")
    u32 popups_drawn_ = 0;

    bool title_modified_ = false;
    std::string title_path_ = "\x01";
    std::string title_root_, title_base_;  // the project folder it was set with; the one found for a file while the side panel has none
    std::string window_title();  // "render.cpp — blit (~/Dev/blit)"

    // Startup timeline (steady clock ns), for --bench startup.
    u64 t_sdl_ = 0, t_window_ = 0, t_gpu_ = 0, t_font_ = 0, t_file_ = 0;
    u64 t_script_ns_ = 0;  // how long init.lua and the plugins took

    // Files a later `blit` hands over, and folders dropped on the window
    // (appinstance.cpp).
    void instance_start();
    void instance_stop();
    void instance_take();         // (user event 21) the files handed over are opened
    void instance_check_waits();  // the askers whose tabs have all closed are told
    bool open_place(const OpenAsk& f);  // a file (at a line), or a folder to work in
    void install_command();       // `blit` on the PATH: a link to this program
    std::mutex instance_mu_;
    std::vector<InstanceAsk> instance_in_;     // handed over, not opened yet (under instance_mu_)
    std::vector<InstanceAsk> instance_waits_;  // opened; their askers wait for the tabs to close
    std::unique_ptr<InstanceServer> instance_;  // (declared after what it fills, so it goes first)
};

}  // namespace blit
