#pragma once
// Language server client: the Language Server Protocol over child
// processes' stdin and stdout, as many servers at once as the open files
// want.
//
// A connection is one server for one project: it is keyed by the server
// and the project's folder, and keeps open every document of that project
// that has a tab (didOpen and didClose follow the tabs). A connection with
// no documents left is stopped a while later.
//
// Each connection has a thread of its own that does all the talking. It
// starts the server, writes messages to it, reads and parses what comes
// back. The main thread only queues small messages and, in poll(), picks up
// results that are already parsed. Nothing here can stall typing or a
// frame, whatever a server is doing, including not answering at all.
//
// One document is the one in front (follow()): completion, hover and the
// other questions are about it, and its text alone is handed to a worker as
// pieces of the buffer (no copy). The others' whole text, when it has to
// go, is copied first: their buffers may be read again from disk at any
// time.
//
// Positions: the protocol counts columns in UTF-16 code units unless the
// server agrees to UTF-8, which is asked for. Everything this class hands
// out is in byte offsets into the document; the conversion happens inside.
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.hpp"
#include "editor.hpp"

namespace blit {

// The servers blit can start without being told how.
const std::vector<LspServer>& builtin_lsp_servers();
// The servers to choose from: those from the settings file first (each
// filled in from the built-in server of the same name, if there is one),
// then the remaining built-in ones.
std::vector<LspServer> lsp_server_list(const std::vector<LspServer>& from_settings);

// How well `pattern` (what has been typed) matches `text` (a completion):
// -1 for not at all, higher for better. `at`, if given, receives the byte
// position in `text` of each matched letter.
int lsp_match(std::string_view pattern, std::string_view text, std::vector<u32>* at = nullptr);

struct Diagnostic {
    u64 start = 0, end = 0;  // byte offsets, kept in step with edits
    u8 severity = 1;         // 1 error, 2 warning, 3 information, 4 hint
    bool fixes = false;      // the server says it has a fix for it
    std::string message;
    std::string source, code;
    std::string raw;  // as the server sent it (JSON): handed back when its fixes are asked for
};

struct CompletionItem {
    std::string label, detail, doc;
    std::string filter;  // what typed characters are matched against
    std::string insert;  // what accepting it puts in
    bool snippet = false;  // `insert` is a snippet ($1, ${2:x} ...: snippet.hpp)
    u8 kind = 0;         // the protocol's CompletionItemKind
    // Start of the text `insert` replaces (it ends at the cursor); ~0 means
    // the word being typed.
    u64 replace_from = ~0ull;
    struct Extra {
        u64 start, end;
        std::string text;
    };
    std::vector<Extra> extra;  // edits elsewhere, such as an import to add
};

// One line of text for a popup.
struct InfoLine {
    enum Kind : u8 { kProse, kCode, kHeading, kRule };
    std::string text;
    Kind kind = kProse;
};

struct SignatureInfo {
    std::string label;                     // "int add(int a, int b)"
    u32 param_start = 0, param_end = 0;    // bytes of the label: the argument being typed
    u32 index = 0, count = 0;              // which overload, out of how many
    std::vector<InfoLine> doc;
    bool empty() const { return label.empty(); }
};

struct LspLocation {
    std::string path;
    u32 line = 0, character = 0;  // as the server counts; see Lsp::offset_at
    bool valid() const { return !path.empty(); }
};

// A place in a file, as found references are: the line (from 0), the
// bytes of the name on it, and the line's text where it could be read.
// (Where it could not, on another machine, the columns are as the server
// counts, UTF-16 units if `utf16`: bytes once the file's text is here.)
struct LspPlace {
    std::string path;
    u32 line = 0, col = 0, end_col = 0;
    std::string text;
    bool utf16 = false;
};

// An item of a call or type hierarchy: what it is, where to go for it
// (its name; for a call coming in, where it calls), and the item as the
// server gave it, to send back for the level under it.
struct LspHierItem {
    std::string name, detail;
    int kind = 0;
    LspLocation at;
    std::string raw;
};

// A symbol of a document (its outline) or of the whole project.
struct LspSymbol {
    std::string name, detail, container;  // its name, what the server adds (a signature, a type), what it is inside
    std::string path;                     // its file (the project's; a document's symbols are the document's)
    u8 kind = 0;                          // LSP's SymbolKind (12 a function, 5 a class, 23 a struct, ...)
    u32 depth = 0;                        // how deep in a document's outline (0: at the top)
    u32 line = 0, character = 0;          // where its name starts, as the server counts
    u64 offset = ~0ull;                   // and, for the outline of the document in front, as a byte offset
};
// A SymbolKind in a word ("function", "class"; "" for one LSP does not have).
const char* symbol_kind_name(u8 kind);

// Changes to files, as a rename or a fix sends them. Positions are as the
// server counts (see utf16); they become byte offsets only against the text
// they are applied to (Lsp::offset_in).
struct LspTextEdit {
    u32 l0 = 0, c0 = 0, l1 = 0, c1 = 0;
    std::string text;
};
struct LspFileEdit {
    std::string path;
    std::vector<LspTextEdit> edits;
};
struct LspWorkspaceEdit {
    std::vector<LspFileEdit> files;
    bool utf16 = true;
    std::string unsupported;  // what it asked that is not done (creating, renaming or deleting a file)
    bool empty() const { return files.empty(); }
};

// A fix or a refactoring a server offers (a code action).
struct LspAction {
    std::string title, kind;
    bool preferred = false;
    std::string disabled;    // why it cannot be used now, if it cannot
    LspWorkspaceEdit edit;   // what it changes, if it says
    std::string command;     // the command to run (JSON), if any
    std::string resolve;     // the whole action (JSON), when the server fills in the rest on asking
};

// A note the server would put between characters: a parameter's name
// before an argument, a deduced type after a name.
struct InlayHint {
    u64 off = 0;             // byte offset it goes before, kept in step with edits
    std::string label;
    u8 kind = 0;             // 1 a type, 2 a parameter, 0 other
    bool pad_left = false, pad_right = false;
};

// A stretch of a document the server colours: a token of one of its types
// (an index into the legend, see Lsp::token_types()) with modifier bits.
struct SemanticToken {
    u64 start = 0, end = 0;
    u16 type = 0;
    u32 mods = 0;
};

// What the servers report about one file, open or not.
struct ProblemFile {
    std::string path;
    u64 key = 0;  // the document's, if it is open (0: a file with no tab)
    struct Item {
        u32 line = 0, col = 0;  // from 0; col in bytes, where it could be worked out
        u8 severity = 1;
        u64 off = ~0ull;        // in the open document (~0: not open)
        std::string message;
    };
    std::vector<Item> items;
};

class Lsp final : public EditListener {
public:
    // What poll() reports as new.
    enum : u32 {
        kState = 1,        // a server became ready, failed or stopped
        kDiagnostics = 2,  // some document's diagnostics changed (problems() too)
        kCompletion = 4,
        kHover = 8,
        kSignature = 16,
        kDefinition = 32,
        kReferences = 64,
        kRenameRange = 128,   // the answer to prepare_rename()
        kRename = 256,        // the answer to rename()
        kFormat = 512,
        kActions = 1024,      // code actions, or one of them resolved
        kHints = 2048,        // some document's inlay hints
        kTokens = 4096,       // some document's semantic tokens
        kApplyEdit = 8192,    // a server asks for an edit to be made (see take_apply())
        kPlaces = 16384,      // implementations, type definitions or declarations (see places())
        kHighlights = 32768,  // the uses of the symbol at a place, in the document in front
        kOutline = 65536,     // the document in front's symbols
        kSymbols = 131072,    // the project's symbols that match what was asked
        kSwitch = 262144,     // the header for a source file, or the source for a header
        kHierarchy = 524288,  // a call or type hierarchy's items (see hierarchy_list())
        kCounts = 1048576,    // how many references places have (see take_counts())
    };
    // The levels of a hierarchy asked for under an item.
    enum HierKind : u8 { kCallsIn, kCallsOut, kSupertypes, kSubtypes };
    // What places() asks for.
    enum PlaceKind : u8 { kImplementation, kTypeDefinition, kDeclaration };

    Lsp();
    Lsp(const Lsp&) = delete;
    Lsp& operator=(const Lsp&) = delete;
    ~Lsp();

    // Called on a worker thread whenever something is ready for poll().
    void set_notify(std::function<void()> fn);

    // The first server in the list that handles the language and whose
    // program can be found. *program gets the program's full path.
    static const LspServer* pick(const std::vector<LspServer>& servers, std::string_view language,
                                 std::string* program);
    // Where a program is: the name itself if it is a path, else the first
    // match on PATH and the usual install folders. Empty if nowhere.
    static std::string find_program(const std::string& name);

    // ---- documents: one for each tab

    // Opens a document on the server for its language and project, starting
    // that server if none is running (the document goes to it once it is
    // ready). `key` is the caller's name for it. False if no server handles
    // it; *wait set means one that might is still finding its project's
    // folder on another machine, and asking again later may do.
    bool open(u64 key, Editor& ed, const std::string& path, const std::string& language,
              const std::vector<LspServer>& servers, bool* wait = nullptr);
    // Tells its server it is closed, and forgets what was said about it.
    void close_doc(u64 key);
    bool is_open(u64 key) const;
    // The name it was opened under (as given to open()); null if not open.
    const std::string* opened_as(u64 key) const;
    // The open document a server's path is (0: none).
    u64 key_of(const std::string& path) const;
    // Its text was replaced without an edit (read again from disk): the
    // server is sent it whole. Cheap when it was not.
    void refresh(u64 key);
    // The document in front, or none (0).
    void follow(u64 key);
    void unfollow();
    u64 front() const;
    // Every server stops (the settings changed), every document goes.
    void shutdown();
    // Stops the servers that have had no document for longer than
    // idle_stop_ms. When the next one is due, in ns of now_ns() (0: none).
    void tick();
    u64 next_tick_ns() const;
    void set_idle_stop_ms(u64 ms) { idle_stop_ms_ = ms; }
    // (Tests.) Each server: its name, its project, how many documents.
    std::string servers_dump() const;
    size_t server_count() const;

    // ---- the one document of tests and benchmarks (attach() and the
    // caller's ed.listen(&lsp)): it is the one in front

    // Opens a document as open() does, closing the one attached before.
    bool attach(const Buffer& buf, const std::string& path, const std::string& language,
                const std::vector<LspServer>& servers);
    // Lets go of the document and shuts its server down, without waiting.
    void detach();
    // Lets go of the document and keeps the server running.
    void release();
    void before_edit(u64 off, u64 remove_len) override;
    void on_edit(const TextEdit& e) override;

    // ---- the document in front

    bool attached() const;  // a server for it (or, after release(), the one it had)
    bool idle() const;      // that server, with no document in front
    bool ready() const;
    const std::string& server_name() const;
    const std::string& root() const;
    const std::string& path() const;  // the document's full path
    bool counts_utf16() const;
    void did_save();

    // Questions. Each answer arrives through poll(); asking again drops the
    // answer still under way.
    bool can_complete() const;
    bool can_hover() const;
    bool can_signature() const;
    bool can_define() const;
    bool can_references() const;
    bool can_rename() const;
    bool can_format(bool range) const;
    bool can_act() const;
    bool can_place(PlaceKind kind) const;
    bool can_highlight() const;
    bool can_outline() const;
    bool can_find_symbols() const;
    bool can_switch() const;  // (clangd's own question)
    bool completion_trigger(char c) const;
    bool signature_trigger(char c) const;
    void complete(u64 offset, char trigger);  // trigger: the character that set it off, or 0
    void hover(u64 offset);
    void signature(u64 offset);
    void definition(u64 offset);
    void references(u64 offset, bool declaration = true);
    void places(u64 offset, PlaceKind kind);    // the answer, place_list(), comes as kPlaces
    void highlight(u64 offset);                 // highlights(), as kHighlights
    void outline();                             // outline_list(), as kOutline
    void find_symbols(const std::string& query);  // found_symbols(), as kSymbols
    void switch_header();                       // switch_target(), as kSwitch
    // A call (or type) hierarchy: the items at a place, then those under one
    // of them; hierarchy_list(), as kHierarchy.
    bool can_hierarchy(bool types) const;
    void hierarchy_prepare(u64 offset, bool types);
    void hierarchy_expand(const std::string& raw, HierKind kind);
    const std::vector<LspHierItem>& hierarchy_list() const { return hier_; }
    // How many references the name at a place in the document in front has
    // (the declaration left out), as kCounts; several asked at once, each
    // answer with the tag it was asked with.
    struct RefCount {
        u64 tag = 0, offset = 0;
        u32 count = 0;
    };
    void count_references(u64 offset, u64 tag);
    std::vector<RefCount> take_counts() { return std::exchange(counts_, {}); }
    size_t counts_asked() const { return counting_.size(); }  // asked and not answered (for the tests)
    // Asks whether the name at offset can be renamed, and which bytes it is.
    // A server that does not say answers at once: renamable, the word there.
    void prepare_rename(u64 offset);
    void rename(u64 offset, const std::string& name);
    // The whole document, or the lines from lo to hi (bytes). With `key`,
    // that document rather than the one in front.
    void format(u64 lo, u64 hi, bool whole, u64 key = 0);
    // The actions for the bytes lo..hi, with the problems there.
    void code_actions(u64 lo, u64 hi);
    void resolve_action(const LspAction& a);  // its edit filled in: arrives as kActions
    void execute(const LspAction& a);         // its command run (whatever it changes comes as kApplyEdit)
    // Inlay hints for some lines of a document (they come as kHints), and
    // the semantic tokens of a whole one (kTokens).
    void ask_hints(u64 key, u64 first_line, u64 last_line);
    void ask_tokens(u64 key);

    // Takes in what has arrived; returns which kinds of thing are new.
    u32 poll();

    const std::vector<Diagnostic>& diagnostics() const;  // the document in front's
    u64 diagnostics_generation() const { return diags_gen_; }  // counts up when any document's change
    const std::vector<Diagnostic>* diagnostics_of(u64 key) const;
    // Every file's problems, open or not, files in the order of their paths.
    const std::vector<ProblemFile>& problems();
    u64 problems_generation() const { return problems_gen_; }
    std::vector<CompletionItem>& completions() { return items_; }
    u64 completions_serial() const { return items_serial_; }  // changes whenever the list is another
    bool completions_incomplete() const { return items_incomplete_; }
    u64 completion_offset() const { return complete_at_; }  // where the question was asked
    const std::vector<InfoLine>& hover_lines() const { return hover_lines_; }
    u64 hover_offset() const { return hover_at_; }
    const SignatureInfo& signature_info() const { return sig_; }
    const LspLocation& definition_target() const { return def_; }
    const std::vector<LspPlace>& reference_list() const { return refs_; }
    const std::vector<LspPlace>& place_list() const { return places_; }
    PlaceKind place_kind() const { return place_kind_; }
    // Byte ranges in the document in front, and the place they were asked about.
    const std::vector<std::pair<u64, u64>>& highlights() const { return highlights_; }
    u64 highlight_offset() const { return highlight_at_; }
    const std::vector<LspSymbol>& outline_list() const { return outline_; }
    const std::vector<LspSymbol>& found_symbols() const { return found_; }
    const std::string& found_query() const { return found_query_; }  // what found_symbols() answers
    const std::string& switch_target() const { return switch_; }     // the other file's path (empty: none)
    // prepare_rename's answer: the bytes of the name (lo == hi: it cannot be
    // renamed; rename_error() may say why) and what to start the new name from.
    u64 rename_lo() const { return rename_lo_; }
    u64 rename_hi() const { return rename_hi_; }
    const std::string& rename_placeholder() const { return rename_text_; }
    const std::string& rename_error() const { return rename_error_; }
    const LspWorkspaceEdit& rename_edit() const { return rename_edit_; }
    // The formatting asked for: its edits, as byte offsets in the document
    // (empty: nothing to change, or the text changed meanwhile), and which
    // document they are for.
    struct ByteEdit {
        u64 start, end;
        std::string text;
    };
    const std::vector<ByteEdit>& format_edits() const { return format_edits_; }
    u64 format_key() const { return format_key_; }
    bool format_failed() const { return format_failed_; }
    const std::vector<LspAction>& actions() const { return actions_; }
    // What a server asked to have changed (workspace/applyEdit); answer it
    // with answer_apply() once it is done (or not).
    bool take_apply(LspWorkspaceEdit* edit, std::string* label);
    void answer_apply(bool applied);
    const std::vector<InlayHint>* hints(u64 key) const;
    u64 hints_generation(u64 key) const;
    bool hints_cover(u64 key, u64 first_line, u64 last_line) const;  // asked for these lines since the text last changed
    bool can_hint(u64 key) const;
    const std::vector<SemanticToken>* tokens(u64 key) const;
    u64 tokens_generation(u64 key) const;
    bool tokens_current(u64 key) const;  // for the text as it is now (or asked for it)
    bool can_tokens(u64 key) const;
    const std::vector<std::string>* token_types(u64 key) const;      // the server's legend
    const std::vector<std::string>* token_modifiers(u64 key) const;
    // A problem worth telling the user about ("clangd stopped"), once.
    std::string take_message();
    // The servers that failed or stopped since the last call, by name (as
    // server_name() gives it), so that another one may be tried.
    std::vector<std::string> take_failed();

    // Byte offset of a position the server gave, in any document.
    u64 offset_at(const Buffer& b, u32 line, u32 character) const { return offset_at(b, line, character, counts_utf16()); }
    static u64 offset_at(const Buffer& b, u32 line, u32 character, bool utf16);
    // The same in a text held whole (a file that is not open).
    static u64 offset_in(std::string_view text, u32 line, u32 character, bool utf16);
    // UTF-16 code units in some text (what a column counts, in that encoding).
    static u32 utf16_units(std::string_view s);

    // Tests and benchmarks: blocks until something has arrived for poll(),
    // or the time is up.
    bool wait(int timeout_ms);
    // Numbers for benchmarks: messages sent and received.
    u64 sent() const { return sent_; }

    // "markdown" as language servers write it, to plain lines.
    static void markdown_lines(std::string_view text, bool markdown, std::vector<InfoLine>* out);

    struct Conn;     // shared with a worker thread; defined in lsp.cpp
    struct Inbox;    // what the workers have parsed, for poll()
    struct Server;   // one connection, as the main thread sees it
    struct Document; // one open document

private:
    Server* start_server(const LspServer& s, const std::string& program, const std::string& root, const std::string& remote,
                         std::vector<LspServer> candidates);
    Document* doc_of(u64 key) const;
    Document* add_document(Server* s, u64 key, Editor* ed, const Buffer* buf, const std::string& as_given, const std::string& full,
                           const std::string& language);
    void drop_document(Document* d, bool tell);
    void flush_opens();
    void stop_server(Server* s, bool kill_now);
    void server_gone(Server* s, const std::string& why);
    void materialize(Document* d);
    u32 ask(Document* d, const char* method, u64 offset, u8 kind, std::string_view extra, u32* latest);
    void cancel(Server* s, u32* latest);
    void pull_document(Document* d);
    void pull_workspace(Server* s);
    bool front_ready() const;

    std::shared_ptr<Inbox> inbox_;
    std::vector<std::unique_ptr<Server>> servers_;
    std::vector<std::unique_ptr<Document>> docs_;
    Document* front_ = nullptr;
    Server* last_ = nullptr;  // the front document's server (kept after release())
    u32 next_server_ = 1;
    u32 next_id_ = 100;
    u64 sent_ = 0;
    u64 idle_stop_ms_ = 30000;
    bool unopened_ = false;  // some document is still to be told to its server (see flush_opens())
    std::string message_;
    std::vector<std::string> failed_;

    u64 diags_gen_ = 0, problems_gen_ = 0, problems_built_ = ~0ull;
    std::vector<ProblemFile> problems_;
    std::vector<CompletionItem> items_;
    u64 items_serial_ = 0;
    bool items_incomplete_ = false;
    u32 complete_id_ = 0, hover_id_ = 0, sig_id_ = 0, def_id_ = 0, refs_id_ = 0, prepare_id_ = 0, rename_id_ = 0, format_id_ = 0,
        actions_id_ = 0, resolve_id_ = 0, place_id_ = 0, highlight_id_ = 0, outline_id_ = 0, found_id_ = 0, switch_id_ = 0;
    Server* asked_ = nullptr;  // the server the front's questions went to
    u64 complete_at_ = 0, hover_at_ = 0;
    u64 complete_first_edit_ = ~0ull;  // the earliest place the text changed since completion was asked for
    std::vector<InfoLine> hover_lines_;
    SignatureInfo sig_;
    LspLocation def_;
    std::vector<LspPlace> refs_;
    std::vector<LspHierItem> hier_;
    u32 hier_id_ = 0;
    // (Asked, by request, and of which server: those of an earlier tag, or of
    // a server that has gone, are let go of, and there are never more than
    // a thousand or so.)
    struct Counting {
        RefCount r;
        Server* server = nullptr;
    };
    std::map<u32, Counting> counting_;
    std::vector<RefCount> counts_;
    std::vector<LspPlace> places_;
    PlaceKind place_kind_ = kImplementation;
    std::vector<std::pair<u64, u64>> highlights_;
    u64 highlight_at_ = 0;
    std::vector<LspSymbol> outline_, found_;
    const void* highlight_doc_ = nullptr;  // (the documents those were asked about: an answer for another is dropped)
    const void* outline_doc_ = nullptr;
    std::string found_query_, found_asked_, switch_;
    u32 ask_with(Server* s, const char* method, const std::string& params, u8 kind, u32* latest);
    u64 rename_lo_ = 0, rename_hi_ = 0, rename_at_ = 0;
    // The front document's version a rename, the code actions and an action
    // resolved were asked at: edits that arrive about a text changed since
    // are not used (their places are the old text's).
    i64 rename_version_ = 0, actions_version_ = 0, resolve_version_ = 0;
    std::string rename_text_, rename_error_;
    LspWorkspaceEdit rename_edit_;
    std::vector<ByteEdit> format_edits_;
    u64 format_key_ = 0;
    i64 format_version_ = 0;
    bool format_failed_ = false;
    std::vector<LspAction> actions_;
    struct Apply {
        Server* server = nullptr;
        std::string id;  // the request's id (JSON)
        std::string label;
        LspWorkspaceEdit edit;
    };
    std::vector<Apply> applies_;  // asked for, not yet taken
    Apply applying_;              // taken, not yet answered
};

}  // namespace blit
