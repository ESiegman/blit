#pragma once
// Syntax highlighting with tree-sitter, entirely off the main thread.
//
// A worker thread owns the parser and the syntax tree. The main thread never
// parses and never runs a query; it only
//   - hands the worker a snapshot of the text after edits (a copy of the
//     piece list: the bytes themselves are immutable, so no text is copied),
//   - says which lines are on screen, and
//   - picks up finished highlights, a sorted list of style runs.
// Typing and scrolling therefore cost the same with highlighting on. Colours
// follow a few milliseconds behind an edit; until they arrive the old ones
// are kept in step with the text (see Highlights::edit).
//
// Grammars and highlight queries are not built in. They are loaded at run
// time from blit's own data folder, where grammars.hpp puts them.
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "buffer.hpp"
#include "config.hpp"
#include "editor.hpp"
#include "highlight.hpp"
#include "threads.hpp"

namespace blit {

struct SyntaxOptions {
    std::vector<std::string> parser_dirs;  // searched before the default folders
    std::vector<std::string> query_dirs;
    u64 max_bytes = 32ull << 20;  // larger files are left plain
    u32 margin_lines = 400;       // lines highlighted beyond each end of the window
    // How the highlight query is compiled. kAuto compiles it whole, and for
    // a fast start also in parts on several threads, used until the whole
    // one is ready. The other two are for tests and measurements.
    enum Compile : u8 { kAuto, kWholeOnly, kPartsOnly } compile = kAuto;
    // For a big file: colour the first screen, then wait for release()
    // before the long full parse. Used at launch, so that parse does not
    // compete with the window coming up.
    bool hold_full_parse = false;
};

// The shape of the text as its tree has it, for the lines around the
// window and what encloses them: where brackets pair up, which runs of
// lines can be folded, and the definitions (functions, classes,
// namespaces) with their names. Made on the worker with the colours, kept
// in step with edits until the next comes (see edit()).
struct SyntaxFold {
    u32 header = 0, last = 0;  // lines header + 1 to last fold away under header
};
struct SyntaxScope {
    std::string name;
    u8 kind = 0;          // as a language server's symbols have it: 12 a function, 6 a method, 5 a class, 3 a namespace...
    u32 depth = 0;        // how many scopes it is inside
    u64 start = 0, end = 0;  // bytes, the whole of it
    u64 name_at = 0;      // where its name is
};
struct SyntaxStructure {
    std::vector<std::pair<u64, u64>> pairs;  // brackets: where each opens and where it closes, by where it opens
    std::vector<SyntaxFold> folds;           // by header; at most one a line
    std::vector<SyntaxScope> scopes;         // in the order they start (an enclosing one first)
    bool empty() const { return pairs.empty() && folds.empty() && scopes.empty(); }
    void clear() {
        pairs.clear();
        folds.clear();
        scopes.clear();
    }
    // The text changed: what is after the edit moves, what it cut into goes.
    void edit(const TextEdit& e);
};

struct SyntaxStats {
    double setup_ms = 0;    // from attach until a compiled query was available
    double quick_ms = 0;    // big files: parsing and colouring just the start, before the full parse
    double parse_ms = 0;    // last parse
    double query_ms = 0;    // last highlight pass over the whole requested range
    double visible_ms = 0;  // last highlight pass over just the visible lines
    u64 jobs = 0;
    u64 marks = 0;
    u32 parts = 0;          // query parts in use by the last pass (1 = the whole query)
};

class Syntax final : public EditListener {
public:
    using Notify = std::move_only_function<void()>;

    Syntax() = default;
    Syntax(const Syntax&) = delete;
    Syntax& operator=(const Syntax&) = delete;
    ~Syntax() { detach(); }

    // Called on the worker thread whenever poll() has something to collect.
    // Set it before attach().
    void set_notify(Notify fn) { notify_ = std::move(fn); }

    // Language name for a file ("cpp", "python", ...), or "" if unknown.
    // first_line is used for "#!" lines when the name says nothing. `own`
    // are the grammars from the settings: a file ending one of them lists
    // (blit.opt.syntax.grammars.NAME.extensions) is that language, before
    // anything built in. Built in are only C, C++ (HIP too), Python and
    // what their projects' other files are written in.
    static std::string language_for(std::string_view path, std::string_view first_line, std::span<const GrammarSource> own = {});
    // True if a grammar for the language is installed.
    static bool installed(const std::string& lang, const SyntaxOptions& opt);
    // The installed language a name stands for, as a Markdown code block
    // names one ("c++", "py", "sh", "yml", a language's name or one of its
    // file endings, built in or from the settings); "" if none.
    static std::string language_named(std::string_view name, std::span<const GrammarSource> own, const SyntaxOptions& opt);
    // Colours a piece of text on its own, all at once, on the calling
    // thread (for a worker: it parses, and the first time a language is
    // used it waits for its query to compile). Out: the style id of each
    // byte (an index into *names; 0 is plain). False if there is no grammar.
    static bool highlight_text(const std::string& lang, std::string_view text, const SyntaxOptions& opt, std::vector<u8>* styles,
                               std::vector<std::string>* names);

    // Starts highlighting buf. Returns false (and stays off) if the file is
    // too large or no grammar is installed for the language.
    bool attach(const Buffer& buf, const std::string& lang, const SyntaxOptions& opt);
    // Stops, and returns only once the worker no longer touches the buffer.
    // Must be called before the buffer is replaced or destroyed.
    void detach();
    bool active() const { return active_; }
    const std::string& language() const { return lang_; }

    // Lets a held full parse go ahead (see SyntaxOptions). Cheap to call again.
    void release();

    // From the editor, after each change.
    void on_edit(const TextEdit& e) override;

    // Says which lines are visible and sends the worker any edits it has not
    // seen. Cheap when nothing changed; call it every frame.
    void want(const Buffer& buf, u64 first_line, u64 last_line);

    // Collects finished work. True if highlights() or style_names() changed.
    bool poll();

    const Highlights& highlights() const { return hl_; }
    const SyntaxStructure& structure() const { return shape_; }
    // Asked of the tree as it is now (the edits not yet sent go first),
    // waiting up to `wait_ms` for the worker. False: no answer (no tree yet,
    // or the worker is busy past the wait).
    // The nodes around bytes [lo, hi), the smallest first, each larger than
    // the one before (Expand selection).
    bool ask_around(const Buffer& buf, u64 lo, u64 hi, std::vector<std::pair<u64, u64>>* out, int wait_ms = 200);
    // The whole file's folds and definitions (the outline, folding all).
    bool ask_all(const Buffer& buf, SyntaxStructure* out, int wait_ms = 500);
    // Capture names by style id, e.g. "keyword.function". Index 0 is plain text.
    const std::vector<std::string>& style_names() const { return names_; }
    // Something the user should know (grammar failed to load, ...), once.
    std::string take_message();

    // For tests, screenshots and benchmarks: block until the worker has
    // finished everything it was given.
    void wait_idle();
    SyntaxStats stats();

private:
    struct Snapshot {
        std::vector<Piece> pieces;
        std::vector<u32> starts;  // byte offset of each piece
        u32 size = 0;
        mutable size_t hint = 0;

        void index();
        const char* read(u32 byte, u32* len) const;
        void copy(u32 start, u32 len, std::string* out) const;
    };
    struct Job {
        bool valid = false;
        bool reparse = false;
        u64 version = 0;
        Snapshot snap;
        std::vector<TextEdit> edits;
        u64 lo = 0, hi = 0;          // bytes to highlight
        u64 vis_lo = 0, vis_hi = 0;  // the part of that on screen
    };
    struct Result {
        bool valid = false;
        bool failed = false;
        bool has_names = false;
        u64 version = 0;
        Highlights hl;
        SyntaxStructure shape;
        std::vector<std::string> names;
        std::string message;
    };
    struct Engine;  // everything tree-sitter, used only by the worker
    // A question for the worker (ask_around, ask_all), answered once its
    // tree is of `version`.
    struct Ask {
        enum : u8 { kNone, kAround, kAll } kind = kNone;
        u64 version = 0;
        u64 lo = 0, hi = 0;
        bool answered = false, ok = false;
        std::vector<std::pair<u64, u64>> ranges;
        SyntaxStructure all;
    };
    bool ask(const Buffer& buf, Ask& q, int wait_ms);
    void answer(Ask& q);

    void run();
    void work(Job& job);
    void publish(Result&& r);

    // Main thread.
    bool active_ = false;
    std::string lang_;
    SyntaxOptions opt_;
    u64 version_ = 0;
    std::vector<TextEdit> unsent_;                   // edits the worker has not been given
    std::vector<std::pair<u64, TextEdit>> recent_;  // edits newer than the last result
    bool sent_any_ = false;
    u64 asked_first_ = 0, asked_last_ = 0;  // lines covered by the last request
    Highlights hl_;
    SyntaxStructure shape_;
    std::vector<std::string> names_;
    std::string message_;
    Notify notify_;

    // Shared.
    std::mutex mu_;
    std::condition_variable wake_, idle_;
    Job job_;
    Result result_;
    Ask ask_;  // (kind kNone: none waiting)
    u64 tree_version_ = 0;  // the text the worker's tree is of
    bool busy_ = false;
    bool quit_ = false;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> pending_{false};  // a newer request is waiting for the worker
    std::atomic<bool> held_{false};     // the full parse of a big file waits for release()
    u64 attached_ns_ = 0;
    SyntaxStats stats_;

    // Worker.
    DeepThread thread_;  // (its stack kSyntaxStack: shape() walks the tree by recursion)
    Engine* engine_ = nullptr;
};

}  // namespace blit
