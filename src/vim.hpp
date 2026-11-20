#pragma once
// Modal editing, the way Vim and Neovim do it: Normal, Insert, Replace,
// Visual, Visual Line and Visual Block modes and a command line, with counts, operators,
// motions, text objects, registers, marks, macros, search and "." to
// repeat. The keys are Neovim's defaults.
//
// This is all on top of Editor (a cursor between characters, replace a
// range, undo) and knows nothing of windows or of SDL: keys come in as
// VimKey, and what is not the text's business (save, close, another split)
// goes out as a VimRequest for the application to carry out.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor.hpp"
#include "regex.hpp"

namespace blit {

// How a language's lines open and close a level of indentation, for
// Enter and o, and for a closer typed at the start of a line in Insert
// mode. The words are between spaces.
struct IndentRules {
    bool brackets = false;      // a line that ends in ( [ or { opens a level; one that starts with ) ] or } closes one
    bool colon = false;         // a line that ends in ":" opens one (Python)
    const char* first = "";     // words that open a level when they start a line
    const char* last = "";      // and when they end one
    const char* anywhere = "";  // and wherever they are in it (Lua's function), unless it ends in a closer
    const char* closers = "";   // words that close one when they start a line
    const char* enders = "";    // words whose line is the last of its level (Python's return)
};

struct VimKey {
    enum : u8 { kChar, kEsc, kEnter, kBackspace, kTab, kLeft, kRight, kUp, kDown, kHome, kEnd, kPageUp, kPageDown, kDelete };
    u8 kind = kChar;
    u32 ch = 0;         // kChar: the character (for a Ctrl combination, the letter in lower case)
    bool ctrl = false;
};

struct VimRequest {
    enum : u8 {
        kSave,        // :w   (then_close: :wq, :x, ZZ)
        kSaveAll,     // :wa  (then_close: :wqa, :xa)
        kClose,       // :q, ZQ, <C-w>c  (force: without asking about changes)
        kQuitAll,     // :qa
        kOpen,        // :e FILE (n 1: gf, the name under the cursor)
        kTab,         // n: +1 the next tab, -1 the one before; 0 a new one; arg "first" or "last": that one
        kSplit,       // n: 0 to the right, 1 below
        kFocus,       // n: 0 left, 1 right, 2 up, 3 down: the keyboard to that split
        kScroll,      // n: the line to bring to the top of the view
        kDefinition,  // gd
        kHover,       // K
        kBack,        // <C-o>
        kNoHighlight, // :noh
        kDiagnostic,  // ]d [d: n diagnostics on (or back); arg "first" or "last": ]D [D
        kFind,        // :find NAME (n 0) and :grep WORDS (n 1): the box that finds files and text, with that typed
        kEx,          // a command that is not Vim's own (:lua, :set, a script's): name, arg, force for "!", the lines if ranged
        kReferences,  // gr: the references to the name at the cursor
        kPlaces,      // gD (n 2: the declaration), gy (n 1: the type definition)
        kOutline,     // gO: the file's outline
        kFormat,      // gq: the lines line0 to line1, formatted by the language server
        kFold,        // zc zo za (n 0, 1, 2: the fold at the cursor closed, opened, switched), zM zR (n 3, 4: all closed, all opened)
        kReload,      // :e!: the file read again, its changes here dropped
    };
    u8 kind = 0;
    std::string arg;
    i64 n = 0;
    bool force = false, then_close = false;
    std::string name{};
    u64 line0 = 0, line1 = 0;  // from 0
    bool ranged = false;
};

class Vim final : public EditWatch {
public:
    enum Mode : u8 { kNormal, kInsert, kReplace, kVisual, kVisualLine, kCommand, kVisualBlock };
    enum : u32 {
        kHandled = 1,  // the key was Vim's
        kTyped = 2,    // and put text into the document: typed() says what
    };

    // One key. In Insert mode a key that is not Vim's comes back as 0, for
    // whoever would have had it otherwise.
    u32 feed(Editor& ed, const VimKey& k);
    // Keys in Vim's own notation: "dw", "3j", "<Esc>", "<CR>", "<C-r>",
    // "<BS>", "<lt>" for a "<". For tests and for mappings.
    void keys(Editor& ed, std::string_view notation);

    Mode mode() const { return mode_; }
    bool visual() const { return mode_ == kVisual || mode_ == kVisualLine || mode_ == kVisualBlock; }
    // Visual Block mode's rectangle: lines line0..line1 and display columns
    // [col0, col1). False in any other mode.
    bool block(const Editor& ed, u64* line0, u64* line1, u32* col0, u32* col1) const;
    bool pending() const { return op_ != 0; }  // an operator waits for its motion
    // The bytes Visual mode has selected, [lo, hi).
    void visual_range(const Editor& ed, u64* lo, u64* hi) const;
    // What the bottom line of the window says: the mode, the command line
    // being typed, or the last message. And at its right: the keys of the
    // command under way.
    std::string status() const;
    std::string showcmd() const;
    // Where the command line's cursor is, in the cells status() takes up (a wide character two, a mark none); -1 when no command line is open.
    int status_cursor() const;
    const std::string& typed() const { return typed_; }
    // A count typed before the keys of a mapping (0: none), and dropping it
    // with whatever else of a command was under way.
    u32 count() const { return counted_ ? count_ : 0; }
    void cancel() { clear_pending(); }
    // The next key is the character a command waits for (after f, r, m, ",
    // q, @, and Ctrl-R or Ctrl-V in Insert mode): no mapping applies to it.
    bool awaiting_char() const {
        return await_ == kFindChar || await_ == kReplaceChar || await_ == kRegisterName || await_ == kMarkSet || await_ == kMarkJump || await_ == kMacroStart ||
               await_ == kMacroPlay || await_ == kInsertRegister || await_ == kInsertLiteral;
    }
    // Runs a command line, as typed after ":" (for scripts).
    void ex(Editor& ed, std::string_view line) { run_command(ed, std::string(line)); }
    std::vector<VimRequest> take() { return std::exchange(requests_, {}); }

    // Another document comes to the front. suspend() is for the one that
    // was there (Insert mode ends in it); forget() instead, if it has gone;
    // then resume() for the new one, in Normal mode.
    void suspend(Editor& ed);
    void forget();
    void resume(Editor& ed);
    // A selection made with the mouse becomes Visual mode; a plain click
    // leaves it.
    void adopt_selection(Editor& ed);
    // Where Visual mode began (each of several carets has its own: the
    // application keeps them, and puts each back before that caret's key).
    u64 visual_start() const { return vstart_; }
    void set_visual_start(u64 at) { vstart_ = at; }
    // (kept: an error said is kept for :messages too; false for one that is there already.)
    void say(std::string message, bool kept = true) {
        if (kept) message_ = std::move(message);
        else message_.text = std::move(message);
    }
    const std::string& message() const { return message_.text; }
    // The errors said since the application last took them (E492 and the
    // like), for :messages.
    std::vector<std::string> take_errors() { return std::exchange(message_.errors, {}); }
    // The cursor to another place in the document, as a jump (<C-o> comes back).
    void jump(Editor& ed, u64 off);
    // Whether the cursor has jumped (a search, G, %, a mark) since the
    // application last asked: the view shows where it went a third of the
    // way down, rather than at the edge it came in at.
    bool take_jumped() { return std::exchange(jumped_, false); }
    // Set: the application keeps the jump list, across files. It hears where each long move starts, and <C-o> and <C-i> ask it (kBack, n back or forward).
    std::function<void(u64 from)> on_jump;
    // Marks, the jump list and the like stay with their text when it
    // changes: the document in front has its Editor::watch set to this.
    void edited(u64 start, u64 old_end, u64 new_end) override;

    // What Vim needs to know of its surroundings, set before feed().
    u64 view_top = 0;    // the first line on screen
    u32 view_rows = 24;  // how many lines fit
    u64 buffer_id = 0;   // marks belong to a document
    bool read_only = false;
    std::string shift = "    ";  // what > puts before a line and < takes away
    // What goes before and after a line to make a comment of it (gc): "// "
    // and "", or "<!-- " and " -->". Both empty: the language has none.
    std::string comment_left, comment_right;
    // How its lines indent (none: each new line as far in as the one before).
    const IndentRules* indent_rules = nullptr;
    // Whether a command whose name starts with a capital is a script's
    // (it is then asked for with kEx, even if Vim has one of that name).
    std::function<bool(std::string_view)> has_user_command;
    std::function<std::string()> clipboard_get;
    std::function<void(const std::string&)> clipboard_set;

    // The search pattern whose matches are to be shown (hlsearch), or empty.
    // A pattern with nothing special in it is looked for as text (the
    // fastest way); any other is a regular expression (regex.hpp).
    struct Pattern {
        std::string text;  // what to look for, literally; with `re`, the pattern as typed
        bool ignore_case = false, word = false;
        std::shared_ptr<const VimRegex> re;
        std::string error;  // it is no pattern: Vim's message saying why
        bool empty() const { return text.empty() && !re; }
        bool bad() const { return !error.empty(); }
    };
    const Pattern& highlight() const {
        static const Pattern none;
        return highlight_on_ ? pattern_ : none;
    }
    // The next place the pattern is, from `from` (not at it), or ~0. No more
    // than `reach` bytes are looked at, wrapping round included. *m, if
    // given, gets the whole match.
    static u64 find(const Buffer& buf, const Pattern& p, u64 from, bool forward, bool wrap, bool* wrapped = nullptr, u64 reach = ~0ull, RegexMatch* m = nullptr);
    // Calls hit(at) with the start of each match that starts in [lo, hi), in
    // order (overlapping ones too), until it returns true; returns where
    // that was, or ~0.
    static u64 each_match(const Buffer& buf, const Pattern& p, u64 lo, u64 hi, const std::function<bool(u64)>& hit);
    // The matches that start in [lo, hi) one after another, none inside
    // the one before, as :s and the highlighting take them (with
    // per_line, only the first on each line), until fn returns false.
    static void each_whole(const Buffer& buf, const Pattern& p, u64 lo, u64 hi, bool per_line, const std::function<bool(const RegexMatch&)>& fn);
    static void each_whole(std::string_view text, const Pattern& p, u64 lo, u64 hi, bool per_line, const std::function<bool(const RegexMatch&)>& fn);
    // What is typed, as Vim's / reads it.
    static Pattern parse_pattern(std::string_view typed, const std::string& last_sub = {});
    // What a box that finds things reads: a regular expression (`regex`) or
    // text as it stands; with smart case, as typed when it has a capital,
    // else in either case; or exactly as typed.
    static Pattern make_pattern(std::string_view typed, bool regex, bool exact_case);
    // Where the pattern is in a piece of text, at or after `from`; npos if nowhere.
    static size_t match_in(std::string_view hay, const Pattern& p, size_t from, RegexMatch* m = nullptr);
    // What a match becomes: by :s's rules (& \1 \r ...), or as it stands.
    // `nl` is what \r puts in: the line break of the text.
    static void replacement(const RegexMatch& m, std::string_view with, bool rules, const std::function<std::string(u64, u64)>& text, std::string* out, std::string_view nl = "\n");
    // Every match in [lo, hi) of the document replaced (by :s's rules, or
    // as it stands), all in one undo step; how many. *lines gets how many
    // lines had one, *last where the last one's replacement starts now.
    static u64 replace_all(Editor& ed, const Pattern& p, std::string_view with, bool rules, u64 lo, u64 hi, bool per_line = false, u64* lines = nullptr,
                           u64* last = nullptr);
    // The same in a text (a file not open); *count gets how many.
    static std::string replace_text(std::string_view text, const Pattern& p, std::string_view with, bool rules, u64* count);
    // The command line opened with `text` on it, as if it had been typed
    // (a search: the cursor goes to the first match), and selected if asked:
    // the first character typed then takes its place, Backspace clears it.
    void open_command(Editor& ed, char kind, std::string_view text, bool selected);
    bool command_selected() const { return mode_ == kCommand && cmd_selected_; }
    // :s with the c flag is asking about a match.
    bool confirming() const { return confirm_.on; }
    // What a box found in the file becomes the last search (n and N go on with it).
    void set_search(const Pattern& p) {
        pattern_ = p;
        highlight_on_ = true;
        search_forward_ = true;
    }

private:
    struct Motion {
        u64 to = 0;
        bool ok = true;
        bool linewise = false;
        bool inclusive = false;
        bool keep_column = false;  // an up or down move: the column wanted stays as it was
        bool jump = false;         // a long way: where it started is worth going back to (<C-o>)
    };
    struct Register {
        std::string text;
        bool linewise = false;
        bool blockwise = false;  // a rectangle: its lines go side by side with the text, not between
    };
    enum Await : u8 { kNothing, kFindChar, kReplaceChar, kRegisterName, kMarkSet, kMarkJump, kMacroStart, kMacroPlay, kObject, kPrefixG, kPrefixZ, kPrefixWindow, kInsertRegister, kPrefixBracket, kInsertLiteral };

    u32 normal_key(Editor& ed, const VimKey& k);
    u32 insert_key(Editor& ed, const VimKey& k);
    u32 literal_key(Editor& ed, const VimKey& k);
    u32 command_key(Editor& ed, const VimKey& k);
    void incsearch(Editor& ed);
    bool motion(Editor& ed, u32 c, bool ctrl, u32 count, bool counted, Motion* out);
    bool text_object(Editor& ed, u32 c, bool around, u32 count, u64* lo, u64* hi, bool* linewise);
    void operate(Editor& ed, char op, u64 lo, u64 hi, bool linewise);
    void operate_lines(Editor& ed, char op, u64 line0, u64 line1, bool keep_column = false);
    void toggle_comment(Editor& ed, u64 line0, u64 line1);
    bool commented(const Editor& ed, u64 line) const;
    void put(Editor& ed, bool before, u32 count, bool then_after = false);
    void enter_insert(Editor& ed, u32 count, Mode mode = kInsert);
    void leave_insert(Editor& ed);
    bool drop_indent(Editor& ed, u64 at);
    std::string indent_after(const Editor& ed, u64 line, u64 end, std::string indent) const;
    std::string outdent(const Editor& ed, std::string indent) const;
    void indent_closer(Editor& ed, u64 line);
    void leave_visual(Editor& ed);
    void run_command(Editor& ed, std::string line);
    void substitute(Editor& ed, u64 line0, u64 line1, std::string_view rest, bool flags = true);
    u32 confirm_key(Editor& ed, const VimKey& k);
    void confirm_next(Editor& ed);
    void confirm_end(Editor& ed);
    bool search(Editor& ed, bool forward, u32 count);
    void clamp(Editor& ed);
    void go(Editor& ed, u64 off, bool keep_column = false);
    void clear_pending();
    void store(char name, std::string text, bool linewise, bool yank, const char* nl, bool blockwise = false);  // (nl: the text's line break, for "A to join with)
    void remember_jump(const Editor& ed, u64 off);
    void list_jump(const Editor& ed, u64 off);
    void block_do(Editor& ed, char what, u32 arg = 0);
    // (Never null. A register's text is shared, not copied, between the
    // registers that hold it: "", "1 and the rest.)
    std::shared_ptr<const Register> fetch(char name);
    bool refuse();  // true (with a message) if the document is not to be changed
    u64 lines_of(const Editor& ed);

    Mode mode_ = kNormal;
    // The command being put together.
    u32 count_ = 0, op_count_ = 0;
    bool counted_ = false;
    char op_ = 0;       // d c y < > and ~ u U for g~ gu gU
    char reg_ = 0;
    u64 op_from_ = ~0ull;  // where the motion an operator is given went from or to, whichever is first (~0: lo)
    Await await_ = kNothing;
    char await_key_ = 0;  // f F t T; i or a
    std::string shown_;   // its keys, as typed

    u64 vstart_ = 0;  // where Visual mode began
    bool vis_eol_ = false;  // Visual mode after "$": the end of the line is selected with its last character
    u64 last_vstart_ = ~0ull, last_vend_ = ~0ull;  // the last selection (~0: none yet)
    Mode last_vmode_ = kVisual;
    u32 want_col_ = 0;
    char find_key_ = 0;
    u32 find_char_ = 0;
    bool repeat_find_ = false;  // the find under way is ; or , and not f t F T themselves

    std::unordered_map<char, std::shared_ptr<const Register>> regs_;
    std::unordered_map<u64, u64> marks_;  // buffer id * 256 + the mark's letter -> offset

    // Insert mode.
    u32 insert_count_ = 1;
    u64 insert_from_ = 0;
    bool insert_plain_ = true;  // nothing but typing so far: the text can be repeated for a count
    u8 insert_once_ = 0;        // <C-o>: one Normal command, then back (2: just asked for, 1: the command is under way)
    bool once_eol_ = false;     // <C-o> was pressed with the cursor after the end of line once_line_
    u64 once_line_ = 0;
    std::string insert_prefix_; // o and O with a count: what starts each further line
    bool auto_indent_ = false;  // the line's indent was put there (o, O, cc, S, Enter) and nothing has been typed after it
    std::string last_inserted_; // the "." register
    u64 last_insert_at_ = ~0ull;  // where Insert mode was last left (gi; ~0: it has not been)
    std::vector<std::string> replaced_;  // Replace mode: what each character typed went over, for Backspace
    std::vector<VimKey> insert_keys_;    // the keys typed in Insert mode, which a count types again ("3i<CR><Esc>")
    bool retyping_ = false;              // and they are being typed again
    // <C-v> in Insert mode: the number typed after it so far, its digits,
    // and its kind (x hex, o octal, u or U Unicode; none of them, decimal).
    u32 literal_ = 0;
    u8 literal_digits_ = 0;
    bool literal_hex_ = false, literal_octal_ = false;
    char literal_unicode_ = 0;
    // Visual Block mode: I, A and c type on the first line, and Esc puts the same on the others.
    struct BlockInsert {
        bool on = false;
        u64 line0 = 0, line1 = 0;
        u32 col = 0;
        bool append = false, to_end = false;
        i64 home = -1;  // I and A: the block's left edge, where the cursor goes afterwards
    } block_insert_;
    std::string last_sub_;       // the last :s, for &
    std::string last_flags_;     // its flags, for :s with &
    std::string last_with_;      // what it put in, for ~
    // :s with the c flag: the match being asked about, and what is left.
    struct Confirm {
        bool on = false;
        Pattern p;
        std::string with;
        u64 at = 0, end = 0;     // the match asked about; the end of the lines it may be on
        u64 next = 0;            // where the next is looked for
        u64 last_end = ~0ull;    // where the last one taken ended (an empty match is not taken there)
        bool per_line = false;
        u64 count = 0, lines = 0, last_line = ~0ull;
        RegexMatch m;
    } confirm_;
    // Places in the document in front. They move with its text (edited()),
    // and are put by while another document is in front.
    struct Places {
        std::vector<u64> jumps;  // where long moves started from
        size_t jump_at = 0;
        u64 prev_jump = 0, last_change = ~0ull, last_insert_at = ~0ull, last_vstart = ~0ull, last_vend = ~0ull;
        Mode last_vmode = kVisual;
    };
    std::vector<u64> jumps_;
    size_t jump_at_ = 0;     // which of them the cursor went back to; jumps_.size() when it has not gone back
    u64 prev_jump_ = 0;      // the '' mark
    u64 last_change_ = ~0ull;  // the '. mark
    std::unordered_map<u64, Places> shelved_;  // by buffer id

    // The command line.
    Mode cmd_return_ = kNormal;      // the mode a search was begun in, to go back to
    char cmd_op_ = 0, cmd_reg_ = 0;  // "d/word": the operator a search is the motion of
    char cmd_kind_ = ':';
    std::string cmd_;
    bool cmd_selected_ = false;  // what is on it is selected: typing takes its place
    u64 cmd_origin_ = 0;  // where the cursor was when a search began
    // What the bottom line says. An error ("E486: Pattern not found") is
    // kept as well, until the application takes it for :messages.
    struct Said {
        std::string text;
        std::vector<std::string> errors;
        Said& operator=(std::string s) {
            if (s.size() > 2 && s[0] == 'E' && s[1] >= '0' && s[1] <= '9') {
                if (errors.size() >= 64) errors.erase(errors.begin());
                errors.push_back(s);
            }
            text = std::move(s);
            return *this;
        }
        void clear() { text.clear(); }
    } message_;
    std::vector<std::string> history_;
    size_t history_at_ = 0;
    bool jumped_ = false;  // (see take_jumped())

    // Search.
    Pattern pattern_, saved_pattern_;
    // What incsearch last looked for and found, while the pattern is typed:
    // a pattern typed onto is looked for from there on.
    Pattern inc_pattern_;
    u64 inc_hit_ = 0, inc_rev_ = ~0ull;
    bool search_forward_ = true;
    bool highlight_on_ = false;

    // "." and macros: the keys of commands, to feed again.
    std::vector<VimKey> current_, dot_;
    bool changing_ = false, replaying_ = false;
    char macro_into_ = 0, macro_last_ = 0, macro_rec_ = 0;
    bool macro_append_ = false;  // "qA": what is recorded goes on the end of the macro in a
    bool failed_ = false;        // the command under way did not work (a motion, search or find that found nothing): a macro stops
    u64 cmd_from_ = 0;           // where the cursor was when the command under way began (undone, its change goes back there)
    std::vector<VimKey> macro_keys_;
    std::unordered_map<char, std::vector<VimKey>> macros_;
    int macro_depth_ = 0;

    u64 lines_rev_ = ~0ull, lines_size_ = ~0ull, lines_ = 1;
    const Editor* lines_buf_ = nullptr;
    std::string typed_;
    std::vector<VimRequest> requests_;
};

}  // namespace blit
