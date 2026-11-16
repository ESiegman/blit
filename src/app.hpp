#pragma once
// The application: window, input, scrolling, and the frame loop.
#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "build.hpp"
#include "config.hpp"
#include "editor.hpp"
#include "font.hpp"
#include "git.hpp"
#include "grammars.hpp"
#include "keymap.hpp"
#include "lsp.hpp"
#include "project.hpp"
#include "render.hpp"
#include "syntax.hpp"
#include "vim.hpp"

namespace blit {

struct Options {
    const char* file = nullptr;
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

// A path's folder, and a path made whole (from the current directory).
std::string parent_of(const std::string& path);
std::string full_path(const std::string& path);

// One open file: its text, its colours, and where its view was left.
struct Profile;      // (profile.hpp)
struct ProfileFunc;
struct ProfView;     // a profile's tab (appprofile.cpp)
struct TermView;     // a terminal's tab (appterm.cpp)
struct MdView;       // a Markdown preview's tab (appmarkdown.cpp)
struct ImageView;    // an image file's tab (appimage.cpp)
struct PdfView;      // a PDF's tab (apppdf.cpp)

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
};

struct Doc {
    Editor ed;
    Syntax syntax;  // after ed: it must let go of the buffer first
    u64 id = 0;     // stays the same however the tabs are reordered
    double scroll_x = 0, scroll_y = 0;  // the view, kept while another tab is in front
    // The widest line, in columns, and the revision of the text it was
    // measured at (see App::widest_line()).
    u64 widest_rev = ~0ull;
    u32 widest_tab = 0;
    u64 widest_cols = 0;
    // Columns per level of indentation, guessed from the text; 0: not yet.
    u64 indent_rev = ~0ull, indent_lines = 0;
    u32 indent_step = 0;
    bool indent_tabs = true;
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
    u64 seen_rev = ~0ull, changed_ns = 0;  // when its text was last seen to have changed
    i64 disk_stamp = 0;                    // the file's change time when it was last read or written
    // A tab that shows something made and not a file (a diff, a commit):
    // what it is called, that it is not to be typed in, and its colours.
    std::string title;
    bool read_only = false;
    bool loading = false;  // a file on another machine, on its way (read only until it is here: see appremote.cpp)
    u64 goto_line = ~0ull;  // and where the cursor goes when it is (a line from 0, and a byte in it)
    u32 goto_col = 0;
    std::shared_ptr<ProfView> prof;  // a profile's tab: drawn over the text area (appprofile.cpp)
    std::shared_ptr<TermView> term;  // a terminal's tab: drawn over it too (appterm.cpp)
    std::shared_ptr<MdView> md;      // a Markdown preview's tab: drawn over it too (appmarkdown.cpp)
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
struct ScriptKey;  // a key held back for the mappings (applua.cpp)

// One split of the window: a group of tabs, and the view of the one in
// front. A window that is not split has one.
struct Pane {
    u64 id = 0;
    // Its files, in the order of their tabs, and the one in front. Never
    // empty while the pane is on show.
    std::vector<std::unique_ptr<Doc>> docs;
    Doc* doc = nullptr;
    size_t tab = 0;
    // Where it is in the window, in pixels, and where in that the text
    // starts: right of the line numbers, under the tab bar.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int text_x = 0, text_y = 0;
    // Scroll position in pixels; scroll eases toward target.
    double scroll_x = 0, scroll_y = 0;
    double target_x = 0, target_y = 0;
    // Cursor and selection in (line, column), cached per edit/move.
    u64 cache_rev = ~0ull, cache_cursor = ~0ull, cache_anchor = ~0ull;
    u64 cur_line = 0;
    u32 cur_col = 0;
    u64 sel_line0 = 0, sel_line1 = 0;
    u32 sel_col0 = 0, sel_col1 = 0;
    bool has_sel = false;       // there is a selection to draw: the editor's own, or Visual mode's
    bool cache_visual = false;  // and it was Visual mode's when the cache was made
    // What the renderer remembers about this view from frame to frame.
    AnchorCache anchors;
    GuideCache guides;
    u32 view = 0;  // which of the last frame's text views was this pane's
    std::shared_ptr<TabBar> bar;
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
    void scroll_to_cursor();
    void clamp_scroll();
    double max_scroll_y() const;
    u64 offset_at_point(float x, float y) const;
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
    void open_config();
    void apply_present_mode();
    void update_refresh();
    void zoom(float dpt);
    bool open_path(const char* p);
    void save();
    void request_quit();
    int view_h() const { return std::max(1, pane_->y1 - pane_->text_y); }  // height of the focused pane's text area
    int view_w() const { return std::max(1, pane_->x1 - pane_->text_x); }
    int visible_rows() const { return view_h() / font_->cell_h(); }
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
    void lsp_after_key(SDL_Keycode key);
    bool lsp_click(float x, float y, bool cmd);
    void lsp_decorate(const GridSpec& g);
    void lsp_layout(int w, int h, FrameDesc& fd);
    void lsp_forgive();  // servers that failed may be tried again
    u32 lsp_popups_open() const;  // how many popups the last layout drew

    // The open files and their tab bar (apptabs.cpp).
    void tabs_create();
    Doc* add_doc(size_t at);
    Doc* find_doc(u64 id, Pane** pane = nullptr, size_t* index = nullptr);
    bool find_open(const std::string& full, Pane** pane, size_t* index) const;  // the tab showing a file, if any
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

    // Finding files, text and paths (appfind.cpp).
    struct Find;
    enum : int { kFindNames, kFindText, kFindOpen, kFindSave, kFindFolder, kFindPick, kFindInput, kFindAsk, kFindCommand };
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
    void find_settle();
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
    void remote_asks();
    void save_named(const std::string& path, u64 doc);
    void remember_place();  // where the cursor is now is somewhere to come back to
    // (The overhaul's entry points; each is implemented by the part named.)
    void find_step(int by);                      // appfind.cpp: the next (1) or previous (-1) hit of the last search in files
    std::vector<std::string> recent_files(size_t most) const;  // appstart.cpp: files opened lately, the latest first, that are still there
    void save_as();                              // app.cpp: the file in front, under a name asked for
    void save_all();                             // app.cpp: every file with changes that has a name
    void find_in_file();                         // appcommands.cpp: a search in the file in front (Vim's /, or a box without modes), from the word or selection
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
    bool project_create();
    std::string project_dump();
    std::string project_facts_dump();
    void project_cells(u32 cols, u32 rows);
    void project_facts(bool wait);  // what the machine the project goes on has: vcpkg, ROCm, GPUs, uv, pkg-config
    void project_facts_apply();
    void start_sync();  // a thread of the start screen has found something (user event 19)
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

    // Profiling (appprofile.cpp): the side panel's Profile tab, a tab for
    // each profile (a flame graph and the functions), inline performance labels.
    struct ProfUi;
    void prof_start();
    void prof_record();
    void prof_stop();
    void prof_sync();
    void prof_open(std::shared_ptr<Profile> p);
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

    // The Markdown preview (appmarkdown.cpp, markdown.cpp, mermaid.cpp): a tab
    // beside a Markdown file that shows it drawn, Mermaid diagrams and all.
    void md_open();
    void md_layout(FrameDesc& fd);
    bool md_hit(int px, int py, Pane** pane, u32* row, u32* col);
    bool md_key(const SDL_KeyboardEvent& k);
    bool md_wheel(const SDL_MouseWheelEvent& w);
    bool md_mouse_down(const SDL_MouseButtonEvent& b);
    std::string md_dump();
    struct MdWorker;  // draws previews on a thread of its own, so that typing never waits for one
    void md_sync();   // (event 20: a preview has been drawn)
    void md_wait();   // (for the tests: until every preview asked for is drawn)

    // Pictures (appimage.cpp): image files read on a thread of their own and
    // put on the GPU, for an image file's tab and the Markdown preview.
    struct Pictures;
    static bool is_picture_path(std::string_view path);
    const PictureInfo* picture(const std::string& path);
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
    std::string pdf_page_rect(int page);

    // Terminals (appterm.cpp): a tab running a shell, here or on another machine.
    // A terminal tab in dir (the working folder when none): a login shell, or
    // `command` run by sh, whose tab stays when it ends and says how (and
    // keeps `title`). Its document's id; 0 if it could not start.
    u64 term_open(const std::string& dir = {}, const std::string& command = {}, const std::string& title = {});
    void term_sync();
    void term_layout(FrameDesc& fd);
    void term_copy(TermView& v);
    void term_paste(TermView& v);
    bool term_key(const SDL_KeyboardEvent& k);
    bool term_text(const char* text);
    bool term_hit(int px, int py, Pane** pane, u64* line, u32* col);
    bool term_mouse_down(const SDL_MouseButtonEvent& b);
    bool term_mouse_motion(const SDL_MouseMotionEvent& m);
    void term_mouse_up();
    bool term_wheel(const SDL_MouseWheelEvent& w);
    std::string term_dump();
    bool term_wait(const std::string& text);
    void term_end();

    // Every command and its keys (appcommands.cpp): the command search, and
    // the keys for what had none.
    struct Commands;
    struct PaletteItem {
        std::string title, keys;
        u32 id = 0;
    };
    void commands_create();
    bool command_key(const SDL_KeyboardEvent& k);
    std::vector<PaletteItem> palette_items();
    void palette_open();
    void palette_run(u32 id);
    static std::string chord_text(SDL_Keycode key, SDL_Keymod mod);
    void tab_shift(int by);
    void prof_toggle_labels();

    // Debugging (appdebug.cpp, dap.hpp): breakpoints, a session, the Debug tab.
    struct DebugUi;
    void debug_create();
    bool debug_running() const;
    void debug_toggle_break();
    void debug_send_breaks(const std::string& path);
    u32 debug_gutter(const Doc& d, u64 line);
    void debug_start();
    void debug_end();
    void debug_command(const std::string& what);
    void debug_frame(u32 i, bool show);
    void debug_expand(u32 i);
    void debug_sync();
    void debug_evaluate();
    void debug_panel_draw(struct TextBox& box, bool focused);
    void debug_do(u32 i);
    void debug_panel_click(int row, int col, const SDL_MouseButtonEvent& b);
    bool debug_panel_key(const SDL_KeyboardEvent& k);
    void debug_marks(FrameDesc& fd);
    void debug_set(const std::string& what, const std::string& value);
    void debug_wait(const std::string& state);
    std::string debug_dump();
    void panel_show_debug();

    // Building (appbuild.cpp, build.cpp): what the working folder builds
    // with, the Build tab, the Run button at the top right, and the output
    // terminal below the text that runs a task.
    struct BuildUi;
    void build_create();
    std::string build_root() const;
    void build_follow();   // after each frame: the folder looked at, when it changed
    void build_sync();     // what the worker that looked found
    void build_changed();  // look again (init.lua's systems changed, or Cmd+R)
    void build_settle();   // looked at now, and waited for
    std::vector<BuildTask> build_tasks();
    u64 build_command(const std::string& dir, const std::string& cmd, const std::string& title);
    void build_run(int index);  // a task of the Build tab's (-1: the Run button's)
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
    // 0 information, 1 a warning, 2 an error; `detail` (a traceback) goes to :messages and the standard error, not the box.
    void notify(const std::string& text, int level = 0, const std::string& detail = {});
    char script_mode_char() const;
    bool script_key(const SDL_KeyboardEvent& k);
    bool script_text(const char* text);
    void script_dropped(const char* text);
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
    Pane* pane_ = nullptr;
    u64 next_doc_id_ = 1, next_pane_id_ = 1;
    Lsp lsp_;
    Doc* lsp_doc_ = nullptr;  // the document the language server is following, if any
    Git git_;                 // (after the documents too: its worker reads their text)
    std::shared_ptr<GitUi> gitui_;
    Vim vim_;                 // modal editing, for the document in front
    u64 vim_doc_ = 0;         // which document that was when Vim last looked
    std::shared_ptr<VimUi> vimui_;
    std::shared_ptr<StatusUi> status_;
    std::shared_ptr<StartUi> start_;
    std::shared_ptr<Commands> commands_;
    std::shared_ptr<DebugUi> debugui_;
    std::shared_ptr<BuildUi> buildui_;
    u64 build_gen_ = 1;  // counts up whenever what the Build tab or the Run button shows may have changed
    u64 debug_gen_ = 1;  // counts up whenever breakpoints, the Debug tab or the stopped line may have changed
    std::vector<GridNote> debug_notes_[kMaxViews];
    std::vector<u32> debug_note_cells_[kMaxViews];
    std::shared_ptr<ProfUi> profui_;  // (before remote_: a recording on another machine ends when its connection does)
    u64 prof_gen_ = 1;                // counts up whenever what the Profile tab shows may have changed
    bool term_drop_text_ = false;     // the text a key sent to a terminal as itself also brings is not typed again
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
    bool quit_after_save_ = false;
    bool close_after_save_ = false;  // the tab whose new file is being named closes once it is saved
    bool lsp_pending_ = false;  // start a language server after the next frame
    bool grammars_pending_ = false;  // and see whether a grammar should be fetched
    u32 popups_drawn_ = 0;

    bool title_modified_ = false;
    std::string title_path_ = "\x01";

    // Startup timeline (steady clock ns), for --bench startup.
    u64 t_sdl_ = 0, t_window_ = 0, t_gpu_ = 0, t_font_ = 0, t_file_ = 0;
    u64 t_script_ns_ = 0;  // how long init.lua and the plugins took
};

}  // namespace blit
