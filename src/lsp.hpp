#pragma once
// Language server client: the Language Server Protocol over a child
// process's stdin and stdout.
//
// A thread of its own does all the talking. It starts the server, writes
// messages to it, reads and parses what comes back. The main thread only
// queues small messages and, in poll(), picks up results that are already
// parsed. Nothing here can stall typing or a frame, whatever the server is
// doing, including not answering at all.
//
// Positions: the protocol counts columns in UTF-16 code units unless the
// server agrees to UTF-8, which is asked for. Everything this class hands
// out is in byte offsets into the document; the conversion happens inside.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
    std::string message;
    std::string source, code;
};

struct CompletionItem {
    std::string label, detail, doc;
    std::string filter;  // what typed characters are matched against
    std::string insert;  // what accepting it puts in
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

class Lsp final : public EditListener {
public:
    // What poll() reports as new.
    enum : u32 {
        kState = 1,        // the server became ready, failed or stopped
        kDiagnostics = 2,
        kCompletion = 4,
        kHover = 8,
        kSignature = 16,
        kDefinition = 32,
    };

    Lsp();
    Lsp(const Lsp&) = delete;
    Lsp& operator=(const Lsp&) = delete;
    ~Lsp();

    // Called on the worker thread whenever something is ready for poll().
    void set_notify(std::function<void()> fn) { notify_ = std::move(fn); }

    // The first server in the list that handles the language and whose
    // program can be found. *program gets the program's full path.
    static const LspServer* pick(const std::vector<LspServer>& servers, std::string_view language,
                                 std::string* program);
    // Where a program is: the name itself if it is a path, else the first
    // match on PATH and the usual install folders. Empty if nowhere.
    static std::string find_program(const std::string& name);

    // Starts the server for a file and returns at once; the server is
    // usable when poll() reports kState and ready() is true. False if no
    // server handles the language (or on a system without support).
    bool attach(const Buffer& buf, const std::string& path, const std::string& language,
                const std::vector<LspServer>& servers);
    // Lets go of the document and shuts the server down, without waiting.
    void detach();
    // Lets go of the document and keeps the server running, for an attach()
    // that follows to hand it another: moving from one tab to the next does
    // not start the server afresh when the next file is one for the same
    // server in the same project. With no such attach(), call detach().
    void release();
    bool attached() const { return conn_ != nullptr; }
    bool idle() const { return conn_ && !buf_; }  // running, with no document
    bool ready() const { return ready_; }
    const std::string& server_name() const { return server_name_; }
    const std::string& root() const { return root_; }
    const std::string& path() const { return path_; }  // the document's full path
    bool counts_utf16() const { return caps_.utf16; }

    void before_edit(u64 off, u64 remove_len) override;
    void on_edit(const TextEdit& e) override;
    void did_save();

    // Questions. Each answer arrives through poll(); asking again drops the
    // answer still under way.
    bool can_complete() const { return ready_ && opened_ && caps_.completion; }
    bool can_hover() const { return ready_ && opened_ && caps_.hover; }
    bool can_signature() const { return ready_ && opened_ && caps_.signature; }
    bool can_define() const { return ready_ && opened_ && caps_.definition; }
    bool completion_trigger(char c) const { return ready_ && opened_ && caps_.completion_triggers.find(c) != std::string::npos; }
    bool signature_trigger(char c) const { return ready_ && opened_ && caps_.signature_triggers.find(c) != std::string::npos; }
    void complete(u64 offset, char trigger);  // trigger: the character that set it off, or 0
    void hover(u64 offset);
    void signature(u64 offset);
    void definition(u64 offset);

    // Takes in what has arrived; returns which kinds of thing are new.
    u32 poll();

    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    u64 diagnostics_generation() const { return diags_gen_; }
    std::vector<CompletionItem>& completions() { return items_; }
    u64 completions_serial() const { return items_serial_; }  // changes whenever the list is another
    bool completions_incomplete() const { return items_incomplete_; }
    u64 completion_offset() const { return complete_at_; }  // where the question was asked
    const std::vector<InfoLine>& hover_lines() const { return hover_lines_; }
    u64 hover_offset() const { return hover_at_; }
    const SignatureInfo& signature_info() const { return sig_; }
    const LspLocation& definition_target() const { return def_; }
    // A problem worth telling the user about ("clangd stopped"), once.
    std::string take_message();

    // Byte offset of a position the server gave, in any document.
    u64 offset_at(const Buffer& b, u32 line, u32 character) const { return offset_at(b, line, character, caps_.utf16); }
    static u64 offset_at(const Buffer& b, u32 line, u32 character, bool utf16);

    // Tests and benchmarks: blocks until something has arrived for poll(),
    // or the time is up.
    bool wait(int timeout_ms);
    // Numbers for benchmarks: messages sent and received.
    u64 sent() const { return sent_; }

    // "markdown" as language servers write it, to plain lines.
    static void markdown_lines(std::string_view text, bool markdown, std::vector<InfoLine>* out);

    struct Conn;  // shared with the worker thread; defined in lsp.cpp

private:
    struct Caps {
        bool utf16 = true;
        int sync = 1;  // 0 none, 1 whole document on every change, 2 just the changes
        bool save = false;
        bool completion = false, hover = false, signature = false, definition = false;
        std::string completion_triggers, signature_triggers;
    };
    void send(std::string json, u32 id = 0, u8 kind = 0);
    void send_document(std::string head, std::string tail, bool replaces_unsent);
    void open_document();
    void close_document();
    void name_document(const std::string& full, const std::string& language);
    u32 character_of(u64 line_start, u64 off) const;
    void position_json(std::string& out, u64 off) const;
    u32 ask(const char* method, u64 offset, u8 kind, std::string_view extra, u32* latest);

    std::shared_ptr<Conn> conn_;
    std::function<void()> notify_;
    const Buffer* buf_ = nullptr;
    std::string server_name_, root_, path_, uri_, language_id_;
    std::string change_head_;  // the part of a didChange message that never changes
    Caps caps_;
    bool ready_ = false, opened_ = false;
    i64 version_ = 0;
    u32 next_id_ = 100;
    u32 old_end_char_ = 0;  // UTF-16 column of the end of the text about to go, from before_edit
    u64 sent_ = 0;
    std::string message_;

    std::vector<Diagnostic> diags_;
    u64 diags_gen_ = 0;
    std::vector<CompletionItem> items_;
    u64 items_serial_ = 0;
    bool items_incomplete_ = false;
    u32 complete_id_ = 0, hover_id_ = 0, sig_id_ = 0, def_id_ = 0;
    u64 complete_at_ = 0, hover_at_ = 0;
    u64 complete_first_edit_ = ~0ull;  // the earliest place the text changed since completion was asked for
    std::vector<InfoLine> hover_lines_;
    SignatureInfo sig_;
    LspLocation def_;
};

}  // namespace blit
