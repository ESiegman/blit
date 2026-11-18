#include "lsp.hpp"

#include "remote.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "json.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace blit {

using json::Value;

// ---------------------------------------------------------------- servers

const std::vector<LspServer>& builtin_lsp_servers() {
    // Each server's usual command line. Where several servers handle one
    // language, the first one that is installed is used.
    static const std::vector<LspServer> list = {
        // (clangd would otherwise index the whole project in the background,
        // on every core; an editor for one file at a time has little use for
        // that. blit.opt.lsp.servers.clangd in init.lua can turn it back on.)
        {"clangd", {"clangd", "--background-index=false"}, {"c", "cpp"},
         {"compile_commands.json", "compile_flags.txt", ".clangd", ".git"}, "", "", true},
        {"pyright", {"pyright-langserver", "--stdio"}, {"python"},
         {"pyproject.toml", "setup.py", "setup.cfg", "requirements.txt", "pyrightconfig.json", ".git"}, "", "", true},
        {"basedpyright", {"basedpyright-langserver", "--stdio"}, {"python"},
         {"pyproject.toml", "setup.py", "setup.cfg", "requirements.txt", "pyrightconfig.json", ".git"}, "", "", true},
        {"pylsp", {"pylsp"}, {"python"}, {"pyproject.toml", "setup.py", "setup.cfg", "requirements.txt", ".git"}, "", "", true},
        // For those projects' other files (init.lua too).
        {"neocmakelsp", {"neocmakelsp", "stdio"}, {"cmake"}, {".git"}, "", "", true},
        {"cmake-language-server", {"cmake-language-server"}, {"cmake"}, {".git"}, "", "", true},
        {"vscode-json-language-server", {"vscode-json-language-server", "--stdio"}, {"json"}, {".git"}, "", "", true},
        {"taplo", {"taplo", "lsp", "stdio"}, {"toml"}, {".git"}, "", "", true},
        {"yaml-language-server", {"yaml-language-server", "--stdio"}, {"yaml"}, {".git"}, "", "", true},
        {"marksman", {"marksman", "server"}, {"markdown"}, {".marksman.toml", ".git"}, "", "", true},
        {"bash-language-server", {"bash-language-server", "start"}, {"bash"}, {".git"}, "", "", true},
        {"lua-language-server", {"lua-language-server"}, {"lua"}, {".luarc.json", ".luarc.jsonc", ".git"}, "", "", true},
    };
    return list;
}

std::vector<LspServer> lsp_server_list(const std::vector<LspServer>& from_settings) {
    std::vector<LspServer> list;
    const std::vector<LspServer>& builtin = builtin_lsp_servers();
    for (LspServer s : from_settings) {
        for (const LspServer& b : builtin) {
            if (b.name != s.name) continue;
            if (s.command.empty()) s.command = b.command;
            if (s.languages.empty()) s.languages = b.languages;
            if (s.roots.empty()) s.roots = b.roots;
        }
        list.push_back(std::move(s));
    }
    for (const LspServer& b : builtin) {
        const bool named = std::any_of(from_settings.begin(), from_settings.end(), [&](const LspServer& s) { return s.name == b.name; });
        if (!named) list.push_back(b);
    }
    return list;
}

namespace {

#if !defined(_WIN32)
// Has the child start in a folder: the call's name from macOS 26 on, the
// older one before that and with glibc.
inline int spawn_chdir(posix_spawn_file_actions_t* fa, const char* dir) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
    return posix_spawn_file_actions_addchdir(fa, dir);
#else
    return posix_spawn_file_actions_addchdir_np(fa, dir);
#endif
}
#endif


// The protocol's name for a language, where it differs from blit's.
std::string language_id(const std::string& language) {
    if (language == "bash") return "shellscript";
    return language;
}

std::string path_to_uri(const std::string& path) {
    std::string u = "file://";
    for (const char ch : path) {
        const u8 c = static_cast<u8>(ch);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                           c == '-' || c == '.' || c == '_' || c == '~';
        if (plain) {
            u += ch;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            u += buf;
        }
    }
    return u;
}

std::string uri_to_path(std::string_view uri) {
    if (!uri.starts_with("file://")) return {};
    uri.remove_prefix(7);
    if (const size_t slash = uri.find('/'); slash != std::string_view::npos) uri.remove_prefix(slash);  // skip a host
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string p;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
            p += static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
            i += 2;
        } else {
            p += uri[i];
        }
    }
    return p;
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

// PATH, plus the folders package managers install into. A program started
// from the Dock gets a PATH without them.
std::string search_path() {
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    const std::string home = home_dir();
    std::vector<std::string> more = {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"};
    if (!home.empty())
        for (const char* sub : {"/.local/bin", "/.cargo/bin", "/go/bin", "/.bun/bin", "/.deno/bin"}) more.push_back(home + sub);
    for (const std::string& dir : more) {
        const std::string wrapped = ":" + path + ":";
        if (wrapped.find(":" + dir + ":") != std::string::npos) continue;
        if (!path.empty()) path += ':';
        path += dir;
    }
    return path;
}

#if !defined(_WIN32)
bool runnable(const std::string& file) {
    struct stat st;
    return stat(file.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(file.c_str(), X_OK) == 0;
}

bool exists(const std::string& file) {
    struct stat st;
    return stat(file.c_str(), &st) == 0;
}

// The nearest folder at or above `dir` that holds one of the marker files;
// `dir` itself if none does.
std::string find_root(const std::string& dir, const std::vector<std::string>& markers) {
    std::string at = dir;
    for (int depth = 0; depth < 64 && !at.empty(); ++depth) {
        for (const std::string& m : markers)
            if (exists(at + "/" + m)) return at;
        const size_t slash = at.rfind('/');
        if (slash == std::string::npos || slash == 0) break;
        at.resize(slash);
    }
    return dir;
}
#endif

// Text with snippet marks ("${1:name}", "$0") as plain text.
std::string strip_snippet(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            out += s[++i];
        } else if (c == '$' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
            while (i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') ++i;
        } else if (c == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            // ${1:default}, ${1|a,b|} or ${1}: keep the default, or the first choice.
            size_t j = i + 2;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
            int depth = 1;
            size_t k = j;
            while (k < s.size() && depth > 0) {
                if (s[k] == '\\' && k + 1 < s.size()) ++k;
                else if (s[k] == '{') ++depth;
                else if (s[k] == '}') --depth;
                ++k;
            }
            std::string_view body = s.substr(j, k > j ? k - j - 1 : 0);
            if (body.starts_with(":")) {
                out += strip_snippet(body.substr(1));
            } else if (body.starts_with("|")) {
                body.remove_prefix(1);
                out += body.substr(0, body.find_first_of(",|"));
            }
            i = k - 1;
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

// How well `pattern` (what was typed) matches `text` (a completion's filter
// text): -1 for not at all, higher for better. A prefix beats letters
// scattered through the word; letters at the start of a word part
// (get_width, getWidth) beat letters in the middle. `at`, if given, receives
// the byte position of each matched letter.
int lsp_match(std::string_view pattern, std::string_view text, std::vector<u32>* at) {
    if (at) at->clear();
    if (pattern.empty()) return 0;
    if (pattern.size() > text.size()) return -1;
    auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); };  // (ASCII: no locale to ask)
    bool prefix = true, exact = true;
    for (size_t i = 0; prefix && i < pattern.size(); ++i) {
        prefix = lower(pattern[i]) == lower(text[i]);
        exact = exact && pattern[i] == text[i];
    }
    if (prefix) {
        if (at)
            for (u32 i = 0; i < pattern.size(); ++i) at->push_back(i);
        return 10000 + (exact ? 1000 : 0) - static_cast<int>(std::min<size_t>(text.size() - pattern.size(), 900));
    }
    int score = 100;
    size_t j = 0;
    bool previous = false;
    for (size_t i = 0; i < text.size() && j < pattern.size(); ++i) {
        if (lower(text[i]) != lower(pattern[j])) {
            previous = false;
            continue;
        }
        const bool boundary = i == 0 || text[i - 1] == '_' || text[i - 1] == '.' || text[i - 1] == ':' ||
                              (text[i] >= 'A' && text[i] <= 'Z' && text[i - 1] >= 'a' && text[i - 1] <= 'z');
        score += boundary ? 30 : previous ? 15 : 0;
        if (j == 0) score -= static_cast<int>(std::min<size_t>(i, 50));
        if (at) at->push_back(static_cast<u32>(i));
        previous = true;
        ++j;
    }
    if (j < pattern.size()) return -1;
    return score - static_cast<int>(std::min<size_t>(text.size(), 60) / 4);
}

// ---------------------------------------------------------------- markdown

// Language servers describe things in a small part of Markdown: headings,
// fenced code, emphasis marks, links. This flattens it to lines of plain
// text, remembering which lines were code.
void Lsp::markdown_lines(std::string_view text, bool markdown, std::vector<InfoLine>* out) {
    bool in_code = false;
    auto blank_last = [&] { return out->empty() || out->back().text.empty(); };
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!markdown) {
            out->push_back({std::string(line), InfoLine::kProse});
            continue;
        }
        std::string_view lead = line;
        while (!lead.empty() && lead.front() == ' ') lead.remove_prefix(1);
        if (lead.starts_with("```") || lead.starts_with("~~~")) {
            in_code = !in_code;
            continue;
        }
        if (in_code) {
            std::string s;
            for (const char c : line) {
                if (c == '\t') s += "    ";
                else s += c;
            }
            out->push_back({std::move(s), InfoLine::kCode});
            continue;
        }
        if (lead.empty()) {
            if (!blank_last()) out->push_back({});  // (a rule counts as blank: its text is empty)
            continue;
        }
        if (lead.find_first_not_of("-*_ ") == std::string_view::npos && lead.size() >= 3) {  // a rule: --- or ***
            while (!out->empty() && out->back().text.empty() && out->back().kind != InfoLine::kRule) out->pop_back();
            if (!out->empty() && out->back().kind != InfoLine::kRule) out->push_back({"", InfoLine::kRule});
            continue;
        }
        InfoLine::Kind kind = InfoLine::kProse;
        if (lead.front() == '#') {
            while (!lead.empty() && lead.front() == '#') lead.remove_prefix(1);
            while (!lead.empty() && lead.front() == ' ') lead.remove_prefix(1);
            line = lead;
            kind = InfoLine::kHeading;
        }
        std::string s;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '\\' && i + 1 < line.size() && std::ispunct(static_cast<unsigned char>(line[i + 1]))) {
                s += line[++i];
            } else if (c == '`') {
                // code span: the marks go, the text stays
            } else if ((c == '*' || c == '_') && i + 1 < line.size() && line[i + 1] == c) {
                ++i;  // ** or __
            } else if (c == '*' && ((i + 1 < line.size() && line[i + 1] != ' ') || (i > 0 && line[i - 1] != ' '))) {
                // *emphasis*
            } else if (c == '[') {
                // [text](target) keeps the text
                const size_t close = line.find("](", i);
                const size_t end = close == std::string_view::npos ? close : line.find(')', close);
                if (end != std::string_view::npos && line.substr(i + 1, close - i - 1).find('[') == std::string_view::npos) {
                    s += line.substr(i + 1, close - i - 1);
                    i = end;
                } else {
                    s += c;
                }
            } else if (c == '&') {
                static const struct {
                    const char* name;
                    const char* text;
                } entities[] = {{"&nbsp;", " "}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}};
                bool hit = false;
                for (const auto& e : entities) {
                    if (line.substr(i).starts_with(e.name)) {
                        s += e.text;
                        i += std::strlen(e.name) - 1;
                        hit = true;
                        break;
                    }
                }
                if (!hit) s += c;
            } else if (c == '\t') {
                s += "    ";
            } else {
                s += c;
            }
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        out->push_back({std::move(s), kind});
    }
    while (!out->empty() && (out->back().text.empty() || out->back().kind == InfoLine::kRule)) out->pop_back();
}


// ---------------------------------------------------------------- shared state

namespace {

struct RawRange {
    u32 l0 = 0, c0 = 0, l1 = 0, c1 = 0;
};

struct RawDiag {
    RawRange r;
    u8 severity = 1;
    bool fixes = false;
    std::string message, source, code, raw;
};

struct RawItem {
    CompletionItem item;
    std::string sort;
    bool has_range = false;
    RawRange range;
    struct Extra {
        RawRange r;
        std::string text;
    };
    std::vector<Extra> extra;
};

struct RawHint {
    u32 line = 0, character = 0;
    std::string label;
    u8 kind = 0;
    bool pad_left = false, pad_right = false;
};

enum : u8 {
    kInReady = 1, kInFailed, kInStopped, kInDiagnostics, kInCompletion, kInHover, kInSignature, kInDefinition,
    kInReferences, kInPrepareRename, kInRename, kInFormat, kInActions, kInResolve, kInHints, kInTokens,
    kInPull, kInWorkspacePull, kInApplyEdit, kInRefresh, kInIgnore, kInPlaces, kInHighlight, kInOutline, kInSymbols, kInSwitch,
};
// What a server asks to have asked again (kInRefresh).
enum : u8 { kRefreshTokens = 1, kRefreshHints = 2, kRefreshDiagnostics = 4 };

RawRange range_of(Value r) {
    RawRange out;
    const Value s = r["start"], e = r["end"];
    out.l0 = static_cast<u32>(std::max<i64>(0, s["line"].integer()));
    out.c0 = static_cast<u32>(std::max<i64>(0, s["character"].integer()));
    out.l1 = static_cast<u32>(std::max<i64>(0, e["line"].integer()));
    out.c1 = static_cast<u32>(std::max<i64>(0, e["character"].integer()));
    return out;
}

// Documentation as servers send it: a string, or {kind, value}.
void doc_lines(Value doc, std::vector<InfoLine>* out) {
    if (doc.is_string()) Lsp::markdown_lines(doc.str(), false, out);
    else if (doc.is_object()) Lsp::markdown_lines(doc["value"].str(), doc["kind"].str() == "markdown", out);
}

// Byte offset in s of a column counted the way the server counts.
size_t column_in(std::string_view s, u32 col, bool utf16) {
    if (!utf16) return std::min<size_t>(col, s.size());
    u32 units = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const u8 b = static_cast<u8>(s[i]);
        if ((b & 0xC0) == 0x80) continue;
        if (units >= col) return i;
        units += b >= 0xF0 ? 2 : 1;
    }
    return s.size();
}

u64 steady_ms() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Caps {
    bool utf16 = true;
    int sync = 1;  // 0 none, 1 whole document on every change, 2 just the changes
    bool save = false;
    bool completion = false, hover = false, signature = false, definition = false;
    bool references = false, rename = false, prepare_rename = false, formatting = false, range_formatting = false;
    bool actions = false, resolve = false, hints = false, tokens = false, tokens_delta = false, pull = false, workspace_pull = false;
    bool execute = false;
    bool implementation = false, type_definition = false, declaration = false, highlight = false, outline = false, symbols = false;
    std::string completion_triggers, signature_triggers;
    std::vector<std::string> token_types, token_mods;
};

// One message from a worker to the main thread.
struct In {
    u8 kind = 0;
    u32 server = 0;  // which connection it came from
    u32 id = 0;
    Caps caps;
    std::string text;  // why it failed, which document, or the id of a request to answer (JSON)
    i64 version = -1;
    std::vector<RawDiag> diags;
    std::vector<RawItem> items;
    bool incomplete = false;
    std::vector<InfoLine> lines;
    SignatureInfo sig;
    LspLocation loc;
    std::vector<LspPlace> places;
    std::vector<RawRange> ranges;  // (each place's, as the server counts)
    RawRange range;
    bool flag = false;
    std::vector<LspTextEdit> edits;
    LspWorkspaceEdit wedit;
    std::vector<LspAction> actions;
    std::vector<RawHint> hints;
    std::vector<u32> tokens;
    std::string result_id;
    std::vector<LspSymbol> symbols;
};

}  // namespace

struct Lsp::Inbox {
    std::mutex mu;
    std::condition_variable arrived;
    std::vector<In> in;
    std::atomic<bool> any{false};
    std::function<void()> notify;

    void push(In&& m) {
        {
            std::lock_guard lock(mu);
            in.push_back(std::move(m));
            any.store(true, std::memory_order_release);
        }
        arrived.notify_all();
        if (notify) notify();
    }
};

struct Lsp::Conn {
    struct Out {
        std::string head, tail;                            // the message, around the document text if there is one
        std::shared_ptr<const std::vector<Piece>> pieces;  // the document, when the message carries it as its buffer's pieces
        std::shared_ptr<const std::string> text;           // or as a copy
        const void* doc = nullptr;                         // whose text it is
        u32 id = 0;                                        // of a request; 0 for a notification
        u8 kind = 0;                                       // what its answer will be
        std::string about;                                 // the document a request is about (its uri), where the answer needs it
        bool whole = false;                                // a whole-document change, superseding an unsent older one
        bool dead = false;                                 // superseded while queued: not to be sent
        bool forget = false;                               // not a message: the worker forgets what it kept about `about`
    };

    std::mutex mu;
    std::vector<Out> out;  // main thread -> worker
    // The pieces of the document in front point into its buffer, which may
    // go (or be read again) once it is no longer in front. The worker holds
    // text_mu from taking the queue to having copied every piece in it;
    // the main thread, holding it, turns what is still queued into copies
    // before letting the document go (Lsp::materialize).
    std::mutex text_mu;
    bool stop = false;
    int wake_r = -1, wake_w = -1;  // a byte written here wakes the worker
    std::atomic<int> pid{0};
    std::shared_ptr<Inbox> inbox;
    u32 server_id = 0;

    // Set before the worker starts.
    LspServer server;
    std::string program, root, path_env;
    // A server on another machine: "ssh://host", which the paths here start
    // with and the server's do not; the servers that may be the one (the
    // first that is installed there is, chosen by the worker); and the
    // program, run there.
    std::string remote;
    std::vector<LspServer> candidates;
    std::shared_ptr<Remote> rconn;
    u32 rproc = 0;

    void push(In&& m) {
        m.server = server_id;
        inbox->push(std::move(m));
    }

#if !defined(_WIN32)
    void wake() {
        const char byte = 1;
        [[maybe_unused]] const ssize_t n = write(wake_w, &byte, 1);
    }
    ~Conn() {
        if (wake_r >= 0) close(wake_r);
        if (wake_w >= 0) close(wake_w);
    }

    struct Worker;
    static void run(std::shared_ptr<Conn> self);
#endif
};

// One connection as the main thread keeps it.
struct Lsp::Server {
    u32 id = 0;
    std::shared_ptr<Conn> conn;
    std::string name;   // the server's, as the settings name it
    std::string shown;  // as the user is told it (another machine's: name@host)
    std::string program, root, remote;
    std::vector<std::string> candidates;  // (another machine's: the names it may turn out to be)
    Caps caps;
    bool ready = false;
    int docs = 0;           // documents on it
    u64 idle_since = 0;     // ns: when its last document went (0: it has some)
    // Problems in files that are not open, by path.
    std::map<std::string, std::vector<ProblemFile::Item>> closed;
    u32 workspace_id = 0;   // a workspace/diagnostic under way
};

// One open document.
struct Lsp::Document final : EditListener {
    Lsp* hub = nullptr;
    Server* server = nullptr;
    u64 key = 0;
    Editor* ed = nullptr;  // null for attach()'s
    const Buffer* buf = nullptr;
    std::string as_given, path, uri, language_id, change_head;
    i64 version = 0;
    bool opened = false;
    u64 rev = 0;  // the editor's revision as last seen, to tell a text read afresh
    u32 old_end_char = 0;  // UTF-16 column of the end of the text about to go, from before_edit
    // A place on a line whose UTF-16 column is known, so that the next
    // column asked for on that line is counted from there rather than from
    // the line's start (typing far along a long line).
    u64 col_line = ~0ull, col_off = 0;
    u32 col_units = 0;
    std::vector<Diagnostic> diags;
    u32 pull_id = 0;
    i64 pull_version = -1;
    bool pull_again = false;
    std::vector<InlayHint> hints;
    u64 hints_gen = 0;
    u32 hints_id = 0;
    i64 hints_version = -1;
    u64 hints_first = 1, hints_last = 0;  // the lines they were asked for (none yet)
    bool hints_stale = true;              // the text changed since
    std::vector<SemanticToken> tokens;
    u64 tokens_gen = 0;
    u32 tokens_id = 0;
    i64 tokens_version = -1;  // asked for (or got) at this version
    std::string tokens_result;  // the last answer's resultId, for a delta

    u32 character_of(u64 line_start, u64 off);
    void position_json(std::string& out, u64 off);
    void before_edit(u64 off, u64 remove_len) override;
    void on_edit(const TextEdit& e) override;
};

#if !defined(_WIN32)

// ---------------------------------------------------------------- the worker thread

struct Lsp::Conn::Worker {
    explicit Worker(Conn& conn) : c(conn) {}

    Conn& c;
    int to_fd = -1, from_fd = -1;  // the server's stdin and stdout
    std::string inbuf, outbuf;
    size_t in_at = 0, out_at = 0;
    struct Waiting {
        u8 kind;
        std::string about;
    };
    std::unordered_map<u32, Waiting> waiting;  // request id -> what kind of answer it gets
    json::Doc settings;
    bool utf16 = true;
    // Semantic tokens as last sent for each document (by uri), which a
    // delta is applied to.
    struct Tokens {
        std::string result;
        std::vector<u32> data;
    };
    std::unordered_map<std::string, Tokens> tokens;

    // Paths as the server knows them (another machine's, without "ssh://host").
    std::string to_uri(const std::string& p) const { return path_to_uri(c.remote.empty() || !p.starts_with(c.remote) ? p : p.substr(c.remote.size())); }
    std::string there(const std::string& p) const { return c.remote.empty() || !p.starts_with(c.remote) ? p : p.substr(c.remote.size()); }
    std::string from_uri(std::string_view u) const {
        std::string p = uri_to_path(u);
        return p.empty() || c.remote.empty() ? p : c.remote + p;
    }

    // Runs a program on the other machine and gives what it printed (false if it could not be run).
    bool run_there(const std::vector<std::string>& argv, std::string* out, int* status) {
        RemoteProc p;
        p.errors = false;  // (not read: not sent)
        std::string err;
        if (!c.rconn->spawn(argv, "", {}, &p, &err)) return false;
        close(p.in);
        close(p.err);
        char buf[4096];
        for (ssize_t n; (n = read(p.out, buf, sizeof buf)) != 0;) {
            if (n > 0) out->append(buf, static_cast<size_t>(n));
            else if (errno != EINTR) break;
        }
        close(p.out);
        *status = c.rconn->wait(p.id);
        return true;
    }

    // Which of the servers is installed there, and the project's folder: the
    // nearest at or above the file's with one of the server's marker files.
    bool probe_there(std::string* why) {
        std::string host, dir;
        split_remote(c.root, &host, &dir);
        c.rconn = remote_for(c.root, &dir, kRemoteWaitMs);
        if (!c.rconn) {
            *why = remote_error(c.root);
            return false;
        }
        c.program.clear();
        for (const LspServer& s : c.candidates) {
            const std::string found = c.rconn->which(s.command[0]);
            if (found.empty()) continue;
            c.server = s;
            c.program = found;
            break;
        }
        if (c.program.empty()) {
            *why = "no language server for this file is installed on " + host;
            return false;
        }
        std::vector<std::string> argv = {"/bin/sh", "-c",
                                         "d=$1; shift; while :; do for m do [ -e \"$d/$m\" ] && { printf %s \"$d\"; exit 0; }; done; "
                                         "case $d in /|'') exit 1;; esac; d=${d%/*}; [ -n \"$d\" ] || d=/; done",
                                         "sh", dir};
        argv.insert(argv.end(), c.server.roots.begin(), c.server.roots.end());
        std::string out;
        int status = 0;
        if (!c.server.roots.empty() && run_there(argv, &out, &status) && status == 0 && out.starts_with('/')) dir = out;
        c.root = c.remote + dir;
        return true;
    }

    void frame(std::string_view body) {
        char head[48];
        const int n = std::snprintf(head, sizeof head, "Content-Length: %zu\r\n\r\n", body.size());
        outbuf.append(head, static_cast<size_t>(n));
        outbuf.append(body);
    }

    bool spawn(std::string* why) {
        if (!c.remote.empty()) {  // on another machine, by blit's agent there
            if (!probe_there(why)) return false;
            std::vector<std::string> argv = {c.program};
            for (size_t i = 1; i < c.server.command.size(); ++i) argv.push_back(c.server.command[i]);
            RemoteProc p;
            p.errors = false;  // (what it says for people is not kept: it stays there)
            if (!c.rconn->spawn(argv, there(c.root), {}, &p, why)) {
                *why = "cannot start " + c.program + ": " + *why;
                return false;
            }
            close(p.err);  // (what it says for people is not kept)
            c.rproc = p.id;
            to_fd = p.in;
            from_fd = p.out;
            fcntl(to_fd, F_SETFL, fcntl(to_fd, F_GETFL) | O_NONBLOCK);
            fcntl(from_fd, F_SETFL, fcntl(from_fd, F_GETFL) | O_NONBLOCK);
            signal(SIGPIPE, SIG_IGN);
            return true;
        }
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) != 0) {
            *why = "cannot create a pipe";
            return false;
        }
        if (pipe(out_pipe) != 0) {
            close(in_pipe[0]);
            close(in_pipe[1]);
            *why = "cannot create a pipe";
            return false;
        }
        // Our ends must not be inherited by the server.
        fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
        fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
        // What the server prints for humans goes nowhere, or to the file
        // named by BLIT_LSP_LOG when someone wants to read it.
        const char* log = std::getenv("BLIT_LSP_LOG");
        posix_spawn_file_actions_addopen(&fa, 2, log && *log ? log : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
#if defined(__APPLE__) || defined(__GLIBC__)
        spawn_chdir(&fa, c.root.c_str());
#endif
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
        // Everything else this program has open stays out of the server.
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__)
        posix_spawn_file_actions_addclosefrom_np(&fa, 3);  // (glibc: the same, as the last of the file actions)
#endif

        std::vector<char*> argv;
        argv.push_back(c.program.data());
        for (size_t i = 1; i < c.server.command.size(); ++i) argv.push_back(c.server.command[i].data());
        argv.push_back(nullptr);
        std::vector<std::string> env_store;
        for (char** e = environ; e && *e; ++e)
            if (std::strncmp(*e, "PATH=", 5) != 0) env_store.emplace_back(*e);
        env_store.push_back("PATH=" + c.path_env);
        std::vector<char*> envp;
        for (std::string& e : env_store) envp.push_back(e.data());
        envp.push_back(nullptr);

        pid_t pid = 0;
        const int rc = posix_spawn(&pid, c.program.c_str(), &fa, &attr, argv.data(), envp.data());
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&fa);
        close(in_pipe[0]);
        close(out_pipe[1]);
        if (rc != 0) {
            close(in_pipe[1]);
            close(out_pipe[0]);
            // "No such file" about a file that is there means the program
            // it is a script for is not.
            *why = "cannot start " + c.program + ": " +
                   (rc == ENOENT && runnable(c.program) ? "the interpreter named on its first line is missing" : std::strerror(rc));
            return false;
        }
        c.pid = pid;
        to_fd = in_pipe[1];
        from_fd = out_pipe[0];
        fcntl(to_fd, F_SETFL, fcntl(to_fd, F_GETFL) | O_NONBLOCK);
        fcntl(from_fd, F_SETFL, fcntl(from_fd, F_GETFL) | O_NONBLOCK);
        return true;
    }

    void send_initialize() {
        const std::string root_uri = to_uri(c.root);
        json::Writer w;
        w.begin_object().member("jsonrpc", "2.0").member("id", 1).member("method", "initialize").key("params").begin_object();
        if (c.remote.empty()) w.member("processId", static_cast<i64>(getpid()));
        else w.key("processId").null();  // (this process is not on that machine: a server that watches for it to end would end at once)
        w.key("clientInfo").begin_object().member("name", "blit").end_object();
        w.member("rootUri", root_uri).member("rootPath", there(c.root));
        w.key("workspaceFolders").begin_array().begin_object().member("uri", root_uri).member("name", c.root.substr(c.root.rfind('/') + 1)).end_object().end_array();
        json::Doc options;
        if (!c.server.init_options.empty() && options.parse(c.server.init_options)) w.key("initializationOptions").raw(c.server.init_options);
        // (codeActionsInline is clangd's: its problems say when it has a fix.)
        w.key("capabilities")
            .raw(R"({"general":{"positionEncodings":["utf-8","utf-16"]},)"
                 R"("offsetEncoding":["utf-8","utf-16"],)"
                 R"("textDocument":{)"
                 R"("synchronization":{"didSave":true},)"
                 R"("publishDiagnostics":{"versionSupport":true,"relatedInformation":false,"codeActionsInline":true},)"
                 R"("completion":{"contextSupport":true,"completionItem":{"snippetSupport":true,"insertReplaceSupport":true,)"
                 R"("labelDetailsSupport":true,"documentationFormat":["plaintext","markdown"]}},)"
                 R"("hover":{"contentFormat":["markdown","plaintext"]},)"
                 R"("signatureHelp":{"signatureInformation":{"documentationFormat":["plaintext","markdown"],)"
                 R"("parameterInformation":{"labelOffsetSupport":true},"activeParameterSupport":true}},)"
                 R"("definition":{"linkSupport":true},)"
                 R"("references":{},)"
                 R"("rename":{"prepareSupport":true},)"
                 R"("formatting":{},"rangeFormatting":{},)"
                 R"("codeAction":{"codeActionLiteralSupport":{"codeActionKind":{"valueSet":["","quickfix","refactor","refactor.extract",)"
                 R"("refactor.inline","refactor.rewrite","source","source.organizeImports","source.fixAll"]}},)"
                 R"("isPreferredSupport":true,"disabledSupport":true,"dataSupport":true,"resolveSupport":{"properties":["edit"]}},)"
                 R"("inlayHint":{},)"
                 R"("semanticTokens":{"requests":{"full":{"delta":true}},"formats":["relative"],)"
                 R"("tokenTypes":["namespace","type","class","enum","interface","struct","typeParameter","parameter","variable",)"
                 R"("property","enumMember","event","function","method","macro","keyword","modifier","comment","string","number",)"
                 R"("regexp","operator","decorator","concept"],)"
                 R"("tokenModifiers":["declaration","definition","readonly","static","deprecated","abstract","async",)"
                 R"("modification","documentation","defaultLibrary"],"overlappingTokenSupport":false,"multilineTokenSupport":false},)"
                 R"("diagnostic":{"relatedDocumentSupport":false}},)"
                 R"("workspace":{"configuration":true,"workspaceFolders":true,"applyEdit":true,"workspaceEdit":{"documentChanges":true},)"
                 R"("executeCommand":{},"semanticTokens":{"refreshSupport":true},"inlayHint":{"refreshSupport":true},)"
                 R"("diagnostics":{"refreshSupport":true}},)"
                 R"("window":{"workDoneProgress":false}})");
        w.end_object().end_object();
        frame(w.out);
    }

    static bool truthy(Value v) { return v.is_object() || v.boolean(false); }

    void on_initialized(Value result) {
        Caps caps;
        const Value sc = result["capabilities"];
        std::string_view enc = sc["positionEncoding"].str();
        if (enc.empty()) enc = result["offsetEncoding"].str();  // how older clangd says it
        caps.utf16 = enc != "utf-8";
        utf16 = caps.utf16;
        const Value sync = sc["textDocumentSync"];
        if (sync.is_number()) {
            caps.sync = static_cast<int>(sync.integer());
        } else if (sync.is_object()) {
            caps.sync = static_cast<int>(sync["change"].integer(0));
            caps.save = truthy(sync["save"]);
        } else {
            caps.sync = 0;
        }
        auto triggers = [](Value list, std::string* out) {
            for (Value t : list) {
                const std::string_view s = t.str();
                if (s.size() == 1 && out->find(s[0]) == std::string::npos) *out += s[0];
            }
        };
        const Value cp = sc["completionProvider"];
        caps.completion = truthy(cp);
        triggers(cp["triggerCharacters"], &caps.completion_triggers);
        caps.hover = truthy(sc["hoverProvider"]);
        const Value sp = sc["signatureHelpProvider"];
        caps.signature = truthy(sp);
        triggers(sp["triggerCharacters"], &caps.signature_triggers);
        triggers(sp["retriggerCharacters"], &caps.signature_triggers);
        caps.definition = truthy(sc["definitionProvider"]);
        caps.references = truthy(sc["referencesProvider"]);
        caps.implementation = truthy(sc["implementationProvider"]);
        caps.type_definition = truthy(sc["typeDefinitionProvider"]);
        caps.declaration = truthy(sc["declarationProvider"]);
        caps.highlight = truthy(sc["documentHighlightProvider"]);
        caps.outline = truthy(sc["documentSymbolProvider"]);
        caps.symbols = truthy(sc["workspaceSymbolProvider"]);
        const Value rp = sc["renameProvider"];
        caps.rename = truthy(rp);
        caps.prepare_rename = rp["prepareProvider"].boolean(false);
        caps.formatting = truthy(sc["documentFormattingProvider"]);
        caps.range_formatting = truthy(sc["documentRangeFormattingProvider"]);
        const Value ap = sc["codeActionProvider"];
        caps.actions = truthy(ap);
        caps.resolve = ap["resolveProvider"].boolean(false);
        caps.execute = truthy(sc["executeCommandProvider"]);
        caps.hints = truthy(sc["inlayHintProvider"]);
        const Value tp = sc["semanticTokensProvider"];
        caps.tokens = tp.is_object() && truthy(tp["full"]);
        caps.tokens_delta = tp["full"]["delta"].boolean(false);
        for (Value t : tp["legend"]["tokenTypes"]) caps.token_types.emplace_back(t.str());
        for (Value t : tp["legend"]["tokenModifiers"]) caps.token_mods.emplace_back(t.str());
        const Value dp = sc["diagnosticProvider"];
        caps.pull = dp.is_object();
        caps.workspace_pull = dp["workspaceDiagnostics"].boolean(false);

        frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        if (!c.server.settings.empty() && settings.parse(c.server.settings)) {
            frame(R"({"jsonrpc":"2.0","method":"workspace/didChangeConfiguration","params":{"settings":)" + c.server.settings + "}}");
        }
        In m;
        m.kind = kInReady;
        m.caps = std::move(caps);
        if (!c.remote.empty()) m.text = c.server.name + "@" + c.remote.substr(6) + "\n" + c.server.name + "\n" + c.root;  // (the one chosen there, and the project's folder found there)
        c.push(std::move(m));
    }

    // A request from the server. All of them get an answer, because a
    // server may wait for it; workspace/applyEdit is answered by the main
    // thread, once the edit has been made.
    void answer_request(Value id, std::string_view method, Value params) {
        std::string reply = R"({"jsonrpc":"2.0","id":)";
        json::dump(reply, id);
        if (method == "workspace/configuration") {
            reply += R"(,"result":[)";
            bool first = true;
            for (Value item : params["items"]) {
                if (!first) reply += ',';
                first = false;
                // "a.b.c" walks into the settings.
                Value at = settings.root();
                std::string_view section = item["section"].str();
                while (!section.empty() && at.exists()) {
                    const size_t dot = section.find('.');
                    at = at[section.substr(0, dot)];
                    section = dot == std::string_view::npos ? std::string_view{} : section.substr(dot + 1);
                }
                json::dump(reply, at);
            }
            reply += "]}";
        } else if (method == "workspace/workspaceFolders") {
            reply += R"(,"result":[{"uri":)";
            json::quote(reply, to_uri(c.root));
            reply += R"(,"name":)";
            json::quote(reply, c.root.substr(c.root.rfind('/') + 1));
            reply += "}]}";
        } else if (method == "workspace/applyEdit") {
            In m;
            m.kind = kInApplyEdit;
            json::dump(m.text, id);
            m.result_id = params["label"].str();
            m.wedit = workspace_edit(params["edit"]);
            c.push(std::move(m));
            return;
        } else if (method == "workspace/semanticTokens/refresh" || method == "workspace/inlayHint/refresh" || method == "workspace/diagnostic/refresh") {
            In m;
            m.kind = kInRefresh;
            m.id = method == "workspace/semanticTokens/refresh" ? kRefreshTokens : method == "workspace/inlayHint/refresh" ? kRefreshHints : kRefreshDiagnostics;
            c.push(std::move(m));
            reply += R"(,"result":null})";
        } else if (method == "client/registerCapability" || method == "client/unregisterCapability" ||
                   method == "window/workDoneProgress/create" || method == "window/showMessageRequest" ||
                   method == "workspace/codeLens/refresh") {
            reply += R"(,"result":null})";
        } else {
            reply += R"(,"error":{"code":-32601,"message":"not supported"}})";
        }
        frame(reply);
    }

    void diagnostic(Value d, RawDiag* r) {
        r->r = range_of(d["range"]);
        r->severity = static_cast<u8>(std::clamp<i64>(d["severity"].integer(1), 1, 4));
        r->message = d["message"].str();
        while (!r->message.empty() && (r->message.back() == '\n' || r->message.back() == ' ')) r->message.pop_back();
        r->source = d["source"].str();
        const Value code = d["code"];
        if (code.is_string()) r->code = code.str();
        else if (code.is_number()) r->code = std::to_string(code.integer());
        r->fixes = d["codeActions"].size() > 0;
        json::dump(r->raw, d);
    }

    void on_diagnostics(Value params) {
        In m;
        m.kind = kInDiagnostics;
        m.text = from_uri(params["uri"].str());
        m.version = params["version"].is_number() ? params["version"].integer() : -1;
        for (Value d : params["diagnostics"]) {
            RawDiag r;
            diagnostic(d, &r);
            m.diags.push_back(std::move(r));
        }
        c.push(std::move(m));
    }

    // A document's diagnostics asked for (textDocument/diagnostic). An
    // answer that they have not changed changes nothing.
    void on_pull(u32 id, Value result, const std::string& path) {
        In m;
        m.kind = kInPull;
        m.id = id;
        m.text = path;
        m.flag = result["kind"].str() == "full";
        for (Value d : result["items"]) {
            RawDiag r;
            diagnostic(d, &r);
            m.diags.push_back(std::move(r));
        }
        c.push(std::move(m));
    }

    // The whole project's (workspace/diagnostic): a message for each file.
    void on_workspace_pull(u32 id, Value result) {
        for (Value report : result["items"]) {
            if (report["kind"].str() != "full") continue;
            In m;
            m.kind = kInDiagnostics;
            m.text = from_uri(report["uri"].str());
            m.version = report["version"].is_number() ? report["version"].integer() : -1;
            for (Value d : report["items"]) {
                RawDiag r;
                diagnostic(d, &r);
                m.diags.push_back(std::move(r));
            }
            c.push(std::move(m));
        }
        In done;
        done.kind = kInWorkspacePull;
        done.id = id;
        c.push(std::move(done));
    }

    void on_completion(u32 id, Value result) {
        In m;
        m.kind = kInCompletion;
        m.id = id;
        const Value list = result.is_array() ? result : result["items"];
        m.incomplete = result["isIncomplete"].boolean(false);
        m.items.reserve(list.size());
        for (Value it : list) {
            RawItem r;
            CompletionItem& o = r.item;
            o.label = it["label"].str();
            if (o.label.empty()) continue;
            o.kind = static_cast<u8>(std::clamp<i64>(it["kind"].integer(0), 0, 255));
            o.detail = it["detail"].str();
            const Value ld = it["labelDetails"];
            if (o.detail.empty()) o.detail = ld["description"].str();
            if (const std::string_view sig = ld["detail"].str(); !sig.empty()) o.label += sig;
            o.filter = it["filterText"].str();
            if (o.filter.empty()) o.filter = it["label"].str();
            r.sort = it["sortText"].str();
            if (r.sort.empty()) r.sort = it["label"].str();
            const Value edit = it["textEdit"];
            if (edit.is_object()) {
                o.insert = edit["newText"].str();
                r.has_range = true;
                r.range = range_of(edit["range"].exists() ? edit["range"] : edit["insert"]);
            } else {
                o.insert = it["insertText"].str();
                if (o.insert.empty()) o.insert = it["label"].str();
            }
            o.snippet = it["insertTextFormat"].integer(1) == 2;  // (put in with its tab stops: appsnippet.cpp)
            std::vector<InfoLine> doc;
            doc_lines(it["documentation"], &doc);
            for (const InfoLine& l : doc) {
                if (o.doc.size() > 2000) break;
                if (!o.doc.empty()) o.doc += '\n';
                o.doc += l.text;
            }
            for (Value e : it["additionalTextEdits"]) r.extra.push_back({range_of(e["range"]), std::string(e["newText"].str())});
            // Leading blanks some servers put in labels for alignment.
            while (!o.label.empty() && o.label.front() == ' ') o.label.erase(0, 1);
            m.items.push_back(std::move(r));
        }
        std::stable_sort(m.items.begin(), m.items.end(), [](const RawItem& a, const RawItem& b) { return a.sort < b.sort; });
        c.push(std::move(m));
    }

    void on_hover(u32 id, Value result) {
        In m;
        m.kind = kInHover;
        m.id = id;
        const Value contents = result["contents"];
        auto one = [&](Value v) {
            if (v.is_string()) {
                markdown_lines(v.str(), true, &m.lines);
            } else if (v.is_object()) {
                if (v["kind"].exists()) {
                    markdown_lines(v["value"].str(), v["kind"].str() == "markdown", &m.lines);
                } else {  // {language, value}: code
                    std::vector<InfoLine> code;
                    markdown_lines(v["value"].str(), false, &code);
                    for (InfoLine& l : code) l.kind = InfoLine::kCode;
                    m.lines.insert(m.lines.end(), code.begin(), code.end());
                }
            }
        };
        if (contents.is_array()) {
            for (Value v : contents) {
                if (!m.lines.empty()) m.lines.push_back({});
                one(v);
            }
        } else {
            one(contents);
        }
        c.push(std::move(m));
    }

    void on_signature(u32 id, Value result) {
        In m;
        m.kind = kInSignature;
        m.id = id;
        const Value sigs = result["signatures"];
        const size_t active = static_cast<size_t>(std::max<i64>(0, result["activeSignature"].integer(0)));
        const Value sig = sigs.at(active < sigs.size() ? active : 0);
        if (sig.is_object()) {
            SignatureInfo& s = m.sig;
            s.label = sig["label"].str();
            s.index = static_cast<u32>(active < sigs.size() ? active : 0);
            s.count = static_cast<u32>(sigs.size());
            doc_lines(sig["documentation"], &s.doc);
            const Value params = sig["parameters"];
            const Value own = sig["activeParameter"];
            const i64 ap = own.is_number() ? own.integer() : result["activeParameter"].integer(0);
            // Parameters are named by text or by position in the label.
            // Text is looked for after the previous parameter, so "int a,
            // int b" finds the second "int".
            size_t from = 0, index = 0;
            for (Value p : params) {
                const Value label = p["label"];
                size_t a = std::string::npos, b = 0;
                if (label.is_array()) {
                    a = column_in(s.label, static_cast<u32>(label.at(0).integer()), utf16);
                    b = column_in(s.label, static_cast<u32>(label.at(1).integer()), utf16);
                } else if (!label.str().empty()) {
                    a = s.label.find(label.str(), from);
                    if (a != std::string::npos) b = a + label.str().size();
                }
                if (a != std::string::npos && b >= a) {
                    from = b;
                    if (static_cast<i64>(index) == ap) {
                        s.param_start = static_cast<u32>(a);
                        s.param_end = static_cast<u32>(b);
                    }
                }
                ++index;
            }
        }
        c.push(std::move(m));
    }

    void on_definition(u32 id, Value result) {
        In m;
        m.kind = kInDefinition;
        m.id = id;
        const Value first = result.is_array() ? result.at(0) : result;
        if (first.is_object()) {
            const bool link = first["targetUri"].exists();
            m.loc.path = from_uri((link ? first["targetUri"] : first["uri"]).str());
            const RawRange r = range_of(link ? (first["targetSelectionRange"].exists() ? first["targetSelectionRange"] : first["targetRange"])
                                             : first["range"]);
            m.loc.line = r.l0;
            m.loc.character = r.c0;
        }
        c.push(std::move(m));
    }

    // Found references, each with its line's text: read here, from the
    // files on this machine (a file open in a tab gets its own text from
    // the main thread instead).
    void on_references(u32 id, Value result, u8 kind = kInReferences) {
        In m;
        m.kind = kind;
        m.id = id;
        std::string file_path, file_text;
        std::vector<size_t> line_starts;
        bool readable = false;
        // (A Location, a list of them, or of LocationLinks: implementations
        // and the like come in any of those shapes.)
        std::vector<Value> locs;
        if (result.is_array()) {
            for (Value loc : result) locs.push_back(loc);
        } else if (result.is_object()) {
            locs.push_back(result);
        }
        for (Value loc : locs) {
            LspPlace p;
            const bool link = loc["targetUri"].exists();
            p.path = from_uri((link ? loc["targetUri"] : loc["uri"]).str());
            if (p.path.empty()) continue;
            const RawRange r = range_of(link ? (loc["targetSelectionRange"].exists() ? loc["targetSelectionRange"] : loc["targetRange"]) : loc["range"]);
            m.ranges.push_back(r);
            p.line = r.l0;
            if (p.path != file_path) {
                file_path = p.path;
                file_text.clear();
                line_starts.assign(1, 0);
                readable = c.remote.empty() && read_file(p.path, &file_text);
                if (readable)
                    for (size_t i = 0; i < file_text.size(); ++i)
                        if (file_text[i] == '\n') line_starts.push_back(i + 1);
            }
            if (readable && r.l0 < line_starts.size()) {
                const size_t a = line_starts[r.l0];
                size_t b = file_text.find('\n', a);
                if (b == std::string::npos) b = file_text.size();
                std::string_view line = std::string_view(file_text).substr(a, b - a);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                p.col = static_cast<u32>(column_in(line, r.c0, utf16));
                p.end_col = r.l1 == r.l0 ? static_cast<u32>(column_in(line, r.c1, utf16)) : static_cast<u32>(line.size());
                p.text = line.substr(0, std::min<size_t>(line.size(), 400));
            } else {
                p.col = r.c0;  // (as the server counts: converted where the text is known)
                p.end_col = r.l1 == r.l0 ? r.c1 : r.c0;
            }
            m.places.push_back(std::move(p));
        }
        c.push(std::move(m));
    }

    static bool read_file(const std::string& path, std::string* out) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
            out->append(buf, n);
            if (out->size() > (64u << 20)) break;
        }
        std::fclose(f);
        return true;
    }

    std::vector<LspTextEdit> text_edits(Value list) {
        std::vector<LspTextEdit> out;
        for (Value e : list) {
            LspTextEdit t;
            const RawRange r = range_of(e["range"]);
            t.l0 = r.l0, t.c0 = r.c0, t.l1 = r.l1, t.c1 = r.c1;
            t.text = e["newText"].str();
            if (e["insertTextFormat"].integer(1) == 2) t.text = strip_snippet(t.text);  // (a SnippetTextEdit's)
            out.push_back(std::move(t));
        }
        return out;
    }

    LspWorkspaceEdit workspace_edit(Value e) {
        LspWorkspaceEdit out;
        out.utf16 = utf16;
        auto file = [&](const std::string& path) -> LspFileEdit& {
            for (LspFileEdit& f : out.files)
                if (f.path == path) return f;
            out.files.push_back({path, {}});
            return out.files.back();
        };
        const Value changes = e["documentChanges"];
        if (changes.is_array()) {
            for (Value ch : changes) {
                const std::string_view kind = ch["kind"].str();
                if (!kind.empty()) {  // create, rename or delete a file
                    out.unsupported = std::string(kind) + " " + uri_to_path(kind == "rename" ? ch["oldUri"].str() : ch["uri"].str());
                    continue;
                }
                const std::string path = from_uri(ch["textDocument"]["uri"].str());
                if (path.empty()) continue;
                std::vector<LspTextEdit> edits = text_edits(ch["edits"]);
                LspFileEdit& f = file(path);
                f.edits.insert(f.edits.end(), std::make_move_iterator(edits.begin()), std::make_move_iterator(edits.end()));
            }
        } else {
            for (Value ch : e["changes"]) {
                const std::string path = from_uri(ch.key());
                if (path.empty()) continue;
                std::vector<LspTextEdit> edits = text_edits(ch);
                LspFileEdit& f = file(path);
                f.edits.insert(f.edits.end(), std::make_move_iterator(edits.begin()), std::make_move_iterator(edits.end()));
            }
        }
        std::erase_if(out.files, [](const LspFileEdit& f) { return f.edits.empty(); });
        return out;
    }

    LspAction action(Value a) {
        LspAction out;
        out.title = a["title"].str();
        if (a["command"].is_string()) {  // a bare Command
            json::Writer w;
            w.begin_object().member("command", a["command"].str()).key("arguments");
            std::string args;
            json::dump(args, a["arguments"].exists() ? a["arguments"] : Value());
            w.raw(a["arguments"].exists() ? args : "[]").end_object();
            out.command = std::move(w.out);
            return out;
        }
        out.kind = a["kind"].str();
        out.preferred = a["isPreferred"].boolean(false);
        out.disabled = a["disabled"]["reason"].str();
        if (a["edit"].is_object()) out.edit = workspace_edit(a["edit"]);
        const Value cmd = a["command"];
        if (cmd.is_object()) {
            json::Writer w;
            w.begin_object().member("command", cmd["command"].str()).key("arguments");
            std::string args;
            if (cmd["arguments"].exists()) json::dump(args, cmd["arguments"]);
            w.raw(args.empty() ? "[]" : args).end_object();
            out.command = std::move(w.out);
        }
        if (!a["edit"].exists() && cmd.exists() == false && a["data"].exists()) json::dump(out.resolve, a);
        return out;
    }

    // The uses of a symbol in one document: their ranges.
    void on_highlight(u32 id, Value result) {
        In m;
        m.kind = kInHighlight;
        m.id = id;
        for (Value h : result) m.ranges.push_back(range_of(h["range"]));
        c.push(std::move(m));
    }

    // A document's symbols: nested DocumentSymbols (each with its children),
    // or SymbolInformation, flat, from older servers.
    void on_outline(u32 id, Value result) {
        In m;
        m.kind = kInOutline;
        m.id = id;
        auto add = [&](auto&& self, Value v, u32 depth) -> void {
            LspSymbol y;
            y.name = v["name"].str();
            y.detail = v["detail"].str();
            y.container = v["containerName"].str();
            y.kind = static_cast<u8>(v["kind"].integer());
            y.depth = depth;
            const Value at = v["selectionRange"].exists() ? v["selectionRange"] : v["range"].exists() ? v["range"] : v["location"]["range"];
            const RawRange r = range_of(at);
            y.line = r.l0;
            y.character = r.c0;
            m.symbols.push_back(std::move(y));
            for (Value kid : v["children"]) self(self, kid, depth + 1);
        };
        for (Value v : result) add(add, v, 0);
        c.push(std::move(m));
    }

    // The project's symbols that match: SymbolInformation, or WorkspaceSymbols
    // (whose place may be a file alone).
    void on_symbols(u32 id, Value result) {
        In m;
        m.kind = kInSymbols;
        m.id = id;
        for (Value v : result) {
            LspSymbol y;
            y.name = v["name"].str();
            y.container = v["containerName"].str();
            y.kind = static_cast<u8>(v["kind"].integer());
            const Value loc = v["location"];
            y.path = from_uri(loc["uri"].str());
            if (y.path.empty()) continue;
            if (loc["range"].exists()) {
                const RawRange r = range_of(loc["range"]);
                y.line = r.l0;
                y.character = r.c0;
            }
            m.symbols.push_back(std::move(y));
        }
        c.push(std::move(m));
    }

    void on_actions(u32 id, Value result, bool resolved) {
        In m;
        m.kind = resolved ? kInResolve : kInActions;
        m.id = id;
        if (resolved) {
            if (result.is_object()) m.actions.push_back(action(result));
        } else {
            for (Value a : result) {
                LspAction x = action(a);
                if (!x.title.empty()) m.actions.push_back(std::move(x));
            }
        }
        c.push(std::move(m));
    }

    void on_hints(u32 id, Value result) {
        In m;
        m.kind = kInHints;
        m.id = id;
        for (Value h : result) {
            RawHint r;
            r.line = static_cast<u32>(std::max<i64>(0, h["position"]["line"].integer()));
            r.character = static_cast<u32>(std::max<i64>(0, h["position"]["character"].integer()));
            const Value label = h["label"];
            if (label.is_string()) {
                r.label = label.str();
            } else {
                for (Value part : label) r.label += part["value"].str();
            }
            // (Line breaks in one would not fit between two characters.)
            std::replace(r.label.begin(), r.label.end(), '\n', ' ');
            if (r.label.empty()) continue;
            r.kind = static_cast<u8>(std::clamp<i64>(h["kind"].integer(0), 0, 2));
            r.pad_left = h["paddingLeft"].boolean(false);
            r.pad_right = h["paddingRight"].boolean(false);
            m.hints.push_back(std::move(r));
        }
        std::stable_sort(m.hints.begin(), m.hints.end(), [](const RawHint& a, const RawHint& b) { return a.line != b.line ? a.line < b.line : a.character < b.character; });
        c.push(std::move(m));
    }

    // Semantic tokens: the whole list, or the changes to the last one
    // (applied here, to the copy kept for that document).
    void on_tokens(u32 id, Value result, const std::string& uri) {
        In m;
        m.kind = kInTokens;
        m.id = id;
        Tokens& t = tokens[uri];
        if (result["data"].is_array()) {
            t.data.clear();
            t.data.reserve(result["data"].size());
            for (Value v : result["data"]) t.data.push_back(static_cast<u32>(v.integer()));
        } else if (result["edits"].is_array()) {
            // Edits are against the old list; applied from the last, they leave the earlier ones' places alone.
            std::vector<std::pair<size_t, Value>> edits;
            for (Value e : result["edits"]) edits.push_back({static_cast<size_t>(std::max<i64>(0, e["start"].integer())), e});
            std::stable_sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (const auto& [start, e] : edits) {
                const size_t at = std::min(start, t.data.size());
                const size_t gone = std::min<size_t>(static_cast<size_t>(std::max<i64>(0, e["deleteCount"].integer())), t.data.size() - at);
                std::vector<u32> in;
                for (Value v : e["data"]) in.push_back(static_cast<u32>(v.integer()));
                t.data.erase(t.data.begin() + static_cast<std::ptrdiff_t>(at), t.data.begin() + static_cast<std::ptrdiff_t>(at + gone));
                t.data.insert(t.data.begin() + static_cast<std::ptrdiff_t>(at), in.begin(), in.end());
            }
        } else {
            m.flag = true;  // nothing (null, or an error): the request failed
        }
        t.result = result["resultId"].str();
        m.result_id = t.result;
        if (!m.flag) m.tokens = t.data;
        c.push(std::move(m));
    }

    void handle(std::string body) {
        json::Doc doc;
        if (!doc.parse(std::move(body))) return;
        const Value v = doc.root();
        const Value id = v["id"];
        const std::string_view method = v["method"].str();
        if (!method.empty()) {
            if (id.exists()) answer_request(id, method, v["params"]);
            else if (method == "textDocument/publishDiagnostics") on_diagnostics(v["params"]);
            return;
        }
        if (!id.is_number()) return;
        const u32 n = static_cast<u32>(id.integer());
        const Value result = v["result"];  // absent after an error: everything below reads as empty
        if (n == 1) {
            if (v["error"].exists()) {
                In m;
                m.kind = kInFailed;
                m.text = c.server.name + " refused to start: " + std::string(v["error"]["message"].str());
                c.push(std::move(m));
            } else {
                on_initialized(result);
            }
            return;
        }
        const auto it = waiting.find(n);
        if (it == waiting.end()) return;
        const Waiting w = std::move(it->second);
        waiting.erase(it);
        switch (w.kind) {
            case kInCompletion: on_completion(n, result); break;
            case kInHover: on_hover(n, result); break;
            case kInSignature: on_signature(n, result); break;
            case kInDefinition: on_definition(n, result); break;
            case kInReferences: on_references(n, result); break;
            case kInPlaces: on_references(n, result, kInPlaces); break;
            case kInHighlight: on_highlight(n, result); break;
            case kInOutline: on_outline(n, result); break;
            case kInSymbols: on_symbols(n, result); break;
            case kInSwitch: {  // (clangd's: the other file's URI, or null)
                In m;
                m.kind = kInSwitch;
                m.id = n;
                m.text = result.is_string() ? from_uri(result.str()) : std::string();
                c.push(std::move(m));
                break;
            }
            case kInPrepareRename: {
                In m;
                m.kind = kInPrepareRename;
                m.id = n;
                if (v["error"].exists()) {
                    m.text = v["error"]["message"].str();
                    m.flag = true;  // refused
                } else if (result["range"].is_object()) {
                    m.range = range_of(result["range"]);
                    m.result_id = result["placeholder"].str();
                } else if (result["start"].is_object()) {
                    m.range = range_of(result);
                } else if (result["defaultBehavior"].boolean(false)) {
                    m.version = 0;  // the word there
                } else {
                    m.flag = true;  // null: not something to rename
                }
                c.push(std::move(m));
                break;
            }
            case kInRename: {
                In m;
                m.kind = kInRename;
                m.id = n;
                if (v["error"].exists()) m.text = v["error"]["message"].str();
                else m.wedit = workspace_edit(result);
                c.push(std::move(m));
                break;
            }
            case kInFormat: {
                In m;
                m.kind = kInFormat;
                m.id = n;
                m.flag = v["error"].exists();
                if (m.flag) m.text = v["error"]["message"].str();
                m.edits = text_edits(result);
                c.push(std::move(m));
                break;
            }
            case kInActions: on_actions(n, result, false); break;
            case kInResolve: on_actions(n, result, true); break;
            case kInHints: on_hints(n, result); break;
            case kInTokens: on_tokens(n, result, w.about); break;
            case kInPull: on_pull(n, result, from_uri(w.about)); break;
            case kInWorkspacePull: on_workspace_pull(n, result); break;
            default: break;
        }
    }

    // Splits what has been read into messages.
    void drain_input() {
        for (;;) {
            const std::string_view rest = std::string_view(inbuf).substr(in_at);
            const size_t head_end = rest.find("\r\n\r\n");
            if (head_end == std::string_view::npos) break;
            size_t length = 0;
            bool found = false;
            std::string_view head = rest.substr(0, head_end);
            while (!head.empty()) {
                const size_t eol = head.find("\r\n");
                const std::string_view line = head.substr(0, eol);
                head = eol == std::string_view::npos ? std::string_view{} : head.substr(eol + 2);
                static const char name[] = "content-length:";
                if (line.size() >= sizeof name - 1) {
                    bool same = true;
                    for (size_t i = 0; same && i < sizeof name - 1; ++i) same = std::tolower(static_cast<unsigned char>(line[i])) == name[i];
                    if (same) {
                        length = std::strtoull(std::string(line.substr(sizeof name - 1)).c_str(), nullptr, 10);
                        found = true;
                    }
                }
            }
            if (!found || length > (1u << 30)) {  // not a message we can make sense of: resynchronize past it
                in_at += head_end + 4;
                continue;
            }
            if (rest.size() < head_end + 4 + length) break;
            handle(std::string(rest.substr(head_end + 4, length)));
            in_at += head_end + 4 + length;
        }
        if (in_at > 0 && in_at >= inbuf.size() / 2) {
            inbuf.erase(0, in_at);
            in_at = 0;
        }
    }

    void take_outgoing() {
        // (text_mu first, then mu: the order the main thread takes them in too.)
        std::lock_guard text_lock(c.text_mu);
        std::vector<Out> out;
        {
            std::lock_guard lock(c.mu);
            out.swap(c.out);
        }
        // Of several whole-document changes with nothing between that
        // needs the older one, only the newest is worth sending.
        bool later_whole = false;
        const void* later_doc = nullptr;
        for (size_t i = out.size(); i-- > 0;) {
            Out& o = out[i];
            if (o.whole) {
                if (later_whole && later_doc == o.doc) o.dead = true;
                later_whole = true;
                later_doc = o.doc;
            } else if (o.id || o.pieces || o.text || o.head.find("didClose") != std::string::npos) {
                later_whole = false;
            }
        }
        std::string body, text;
        for (Out& o : out) {
            if (o.dead) continue;
            if (o.forget) {
                tokens.erase(o.about);
                continue;
            }
            if (o.id) waiting[o.id] = {o.kind, std::move(o.about)};
            if (!o.pieces && !o.text) {
                frame(o.head);
                continue;
            }
            // The document's text, as one JSON string made piece by piece.
            body = std::move(o.head);
            if (o.text) {
                json::quote(body, *o.text);
            } else {
                text.clear();
                size_t total = 0;
                for (const Piece& p : *o.pieces) total += p.len;
                text.reserve(total);
                for (const Piece& p : *o.pieces) text.append(p.p, p.len);
                json::quote(body, text);
            }
            body += o.tail;
            frame(body);
        }
    }

    void main() {
        std::string why;
        if (!spawn(&why)) {
            In m;
            m.kind = kInFailed;
            m.text = why;
            c.push(std::move(m));
            return;
        }
        send_initialize();
        bool stopping = false, eof = false;
        u64 deadline = 0;
        char buf[65536];
        while (!eof) {
            take_outgoing();
            if (!stopping) {
                std::lock_guard lock(c.mu);
                stopping = c.stop;
                if (stopping) {
                    frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":null})");
                    frame(R"({"jsonrpc":"2.0","method":"exit","params":null})");
                    deadline = steady_ms() + 300;
                }
            }
            if (stopping && (out_at >= outbuf.size() || steady_ms() >= deadline)) break;

            pollfd fds[3] = {{from_fd, POLLIN, 0}, {c.wake_r, POLLIN, 0}, {to_fd, static_cast<short>(out_at < outbuf.size() ? POLLOUT : 0), 0}};
            const int timeout = stopping ? static_cast<int>(deadline - std::min(deadline, steady_ms())) + 1 : -1;
            if (::poll(fds, 3, timeout) < 0 && errno != EINTR) break;
            if (fds[1].revents & POLLIN) {
                while (read(c.wake_r, buf, sizeof buf) > 0) {
                }
            }
            if (fds[2].revents & (POLLOUT | POLLERR | POLLHUP)) {
                while (out_at < outbuf.size()) {
                    const ssize_t n = write(to_fd, outbuf.data() + out_at, outbuf.size() - out_at);
                    if (n > 0) {
                        out_at += static_cast<size_t>(n);
                    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                        break;
                    } else {
                        eof = true;  // the server is gone
                        break;
                    }
                }
                if (out_at >= outbuf.size()) {
                    outbuf.clear();
                    out_at = 0;
                }
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                for (;;) {
                    const ssize_t n = read(from_fd, buf, sizeof buf);
                    if (n > 0) {
                        inbuf.append(buf, static_cast<size_t>(n));
                    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                        break;
                    } else {
                        eof = true;
                        break;
                    }
                }
                if (!stopping) drain_input();
            }
        }
        close(to_fd);
        close(from_fd);

        if (c.rconn) {  // (on another machine: the same, there)
            if (c.rconn->wait(c.rproc, 250) == -2) {
                c.rconn->kill(c.rproc, SIGTERM);
                if (c.rconn->wait(c.rproc, 400) == -2) {
                    c.rconn->kill(c.rproc, SIGKILL);
                    c.rconn->wait(c.rproc, 2000);
                }
            }
            if (!stopping) {
                In m;
                m.kind = kInStopped;
                m.text = c.server.name + " stopped";
                c.push(std::move(m));
            }
            return;
        }
        // The server gets a moment to leave by itself, then is made to.
        const pid_t pid = c.pid.load();
        auto gone = [&](int ms) {
            const u64 until = steady_ms() + static_cast<u64>(ms);
            for (;;) {
                int status;
                const pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid || (r < 0 && errno != EINTR)) return true;
                if (steady_ms() >= until) return false;
                usleep(5000);
            }
        };
        if (!gone(250)) {
            kill(pid, SIGTERM);
            if (!gone(400)) {
                kill(pid, SIGKILL);
                gone(2000);
            }
        }
        c.pid = 0;
        if (!stopping) {
            In m;
            m.kind = kInStopped;
            m.text = c.server.name + " stopped";
            c.push(std::move(m));
        }
    }
};

// (This thread keeps the priority of the thread that made it, the one that
// draws: that thread wakes it at every edit, and waking one of lower
// priority (see qos.hpp) cost the edit 0.9 µs more, measured.)
void Lsp::Conn::run(std::shared_ptr<Conn> self) {
    Worker w(*self);
    w.main();
}

#endif  // !_WIN32

// ---------------------------------------------------------------- main thread: messages

namespace {

void queue(Lsp::Conn& c, Lsp::Conn::Out&& o) {
#if !defined(_WIN32)
    bool was_empty;
    {
        std::lock_guard lock(c.mu);
        was_empty = c.out.empty();
        if (o.whole && !c.out.empty() && c.out.back().whole && c.out.back().doc == o.doc) c.out.back() = std::move(o);
        else c.out.push_back(std::move(o));
    }
    if (was_empty) c.wake();  // otherwise the worker is already on its way
#else
    (void)c;
    (void)o;
#endif
}

void send(Lsp::Conn& c, std::string json, u32 id = 0, u8 kind = 0, std::string about = {}) {
    Lsp::Conn::Out o;
    o.head = std::move(json);
    o.id = id;
    o.kind = kind;
    o.about = std::move(about);
    queue(c, std::move(o));
}

}  // namespace

// A message that carries the whole document. The document in front hands
// its text over as a list of pieces, turned into JSON on the worker thread,
// so the size of the file costs the main thread nothing; any other's is
// copied (its buffer may be read again from disk while the message waits).
static void send_document(Lsp::Document& d, bool pinned, std::string head, std::string tail, bool replaces_unsent) {
    Lsp::Conn::Out o;
    o.head = std::move(head);
    o.tail = std::move(tail);
    o.doc = &d;
    o.whole = replaces_unsent;
    if (pinned) {
        auto pieces = std::make_shared<std::vector<Piece>>();
        d.buf->snapshot(pieces.get());
        o.pieces = std::move(pieces);
    } else {
        o.text = std::make_shared<const std::string>(d.buf->text(0, d.buf->size()));
    }
    queue(*d.server->conn, std::move(o));
}

// The document in front, no longer: what is still queued with its pieces
// becomes copies, so that its buffer may go or be read again.
void Lsp::materialize(Document* d) {
    if (!d || !d->server || !d->server->conn) return;
    Conn& c = *d->server->conn;
    std::lock_guard text_lock(c.text_mu);
    std::lock_guard lock(c.mu);
    for (Conn::Out& o : c.out) {
        if (o.doc != d || !o.pieces) continue;
        std::string text;
        for (const Piece& p : *o.pieces) text.append(p.p, p.len);
        o.text = std::make_shared<const std::string>(std::move(text));
        o.pieces.reset();
    }
}

// ---------------------------------------------------------------- documents

// Column of `off` within its line, the way the server counts. In UTF-16
// that is counted from the last place asked about on the same line when
// that is before `off` (a long line is not walked from its start at every
// key), else from the line's start.
u32 Lsp::Document::character_of(u64 line_start, u64 off) {
    if (!server->caps.utf16) return static_cast<u32>(off - line_start);
    u64 from = line_start;
    u32 units = 0;
    if (col_line == line_start && col_off <= off) {
        from = col_off;
        units = col_units;
    }
    // One unit per character, two for those outside the first plane: every
    // byte that starts a character counts one, a four-byte starter one more.
    u64 left = off - from;
    for (Buffer::Reader rd = buf->reader(from); left > 0;) {
        const std::string_view ch = rd.chunk();
        if (ch.empty()) break;
        const size_t n = static_cast<size_t>(std::min<u64>(left, ch.size()));
        const u8* p = reinterpret_cast<const u8*>(ch.data());
        u32 run = 0;
        for (size_t i = 0; i < n; ++i) run += static_cast<u32>((p[i] & 0xC0) != 0x80) + static_cast<u32>(p[i] >= 0xF0);
        units += run;
        rd.advance(static_cast<u32>(n));
        left -= n;
    }
    col_line = line_start;
    col_off = off;
    col_units = units;
    return units;
}

void Lsp::Document::position_json(std::string& out, u64 off) {
    const u64 line = buf->line_of(off);
    char text[64];
    const int n = std::snprintf(text, sizeof text, R"({"line":%llu,"character":%u})", static_cast<unsigned long long>(line),
                                character_of(buf->line_start(line), off));
    out.append(text, static_cast<size_t>(std::min<int>(n, sizeof text - 1)));
}

void Lsp::Document::before_edit(u64 off, u64 remove_len) {
    // In UTF-16 the column where the removed text ends has to be worked out
    // while that text is still there.
    if (!opened || !server->caps.utf16 || server->caps.sync != 2) return;
    const u64 end = off + remove_len;
    old_end_char = character_of(buf->line_start(buf->line_of(end)), end);
}

void Lsp::Document::on_edit(const TextEdit& e) {
    // (The editor counts its revision up once this returns.)
    const bool same_text = !ed || ed->revision() == rev;
    if (ed) rev = ed->revision() + 1;
    ++version;
    if (hub->front_ == this) hub->complete_first_edit_ = std::min(hub->complete_first_edit_, e.start);
    const i64 delta = static_cast<i64>(e.new_end) - static_cast<i64>(e.old_end);
    auto moved = [&](u64 x, u64 inside) {
        if (x <= e.start) return x;
        if (x >= e.old_end) return static_cast<u64>(static_cast<i64>(x) + delta);
        return inside;
    };
    // Diagnostics and hints stay on the text they were about until the
    // server sends new ones.
    if (!diags.empty()) {
        for (Diagnostic& d : diags) {
            d.start = moved(d.start, e.start);
            d.end = std::max(d.start, moved(d.end, e.new_end));
        }
        ++hub->diags_gen_;
        ++hub->problems_gen_;
    }
    if (!hints.empty()) {
        // (One inside what was replaced goes with it.)
        std::erase_if(hints, [&](const InlayHint& h) { return h.off > e.start && h.off < e.old_end; });
        for (InlayHint& h : hints) h.off = moved(h.off, e.start);
        ++hints_gen;
    }
    hints_stale = true;
    if (!tokens.empty()) {
        std::erase_if(tokens, [&](const SemanticToken& t) { return t.end > e.start && t.start < e.old_end; });
        for (SemanticToken& t : tokens) {
            t.start = moved(t.start, e.start);
            t.end = moved(t.end, e.start);
        }
        ++tokens_gen;
    }
    // The cached UTF-16 column holds while the text before it on its line
    // is as it was: a change after it leaves it be, one wholly before its
    // line (not taking the line's own start with it) moves it.
    if (col_line != ~0ull && e.start < col_off) {
        if (e.old_end < col_line) {
            col_line = static_cast<u64>(static_cast<i64>(col_line) + delta);
            col_off = static_cast<u64>(static_cast<i64>(col_off) + delta);
        } else {
            col_line = ~0ull;
        }
    }
    if (!opened || server->caps.sync == 0) return;
    const bool pinned = hub->front_ == this;
    std::string m = change_head;
    m += std::to_string(version);
    if (server->caps.sync == 1 || !same_text) {
        // (A text read afresh since the last change: the server's copy is
        // not that text, and only the whole of it will do.)
        m += R"(},"contentChanges":[{"text":)";
        send_document(*this, pinned, std::move(m), "}]}}", same_text);
    } else {
        char num[192];
        const u32 c0 = server->caps.utf16 ? character_of(buf->line_start(e.start_row), e.start) : e.start_col;
        const u32 c1 = server->caps.utf16 ? old_end_char : e.old_end_col;
        const int n = std::snprintf(num, sizeof num, R"(},"contentChanges":[{"range":{"start":{"line":%u,"character":%u},"end":{"line":%u,"character":%u}},"text":)",
                                    e.start_row, c0, e.old_end_row, c1);
        m.append(num, static_cast<size_t>(std::min<int>(n, sizeof num - 1)));
        const u64 len = e.new_end - e.start;
        if (len > 256 * 1024) {
            // A big paste: cheaper to hand over the document than to copy the
            // pasted text here. (A change without a range means "all of it".)
            std::string whole = change_head;
            whole += std::to_string(version);
            whole += R"(},"contentChanges":[{"text":)";
            send_document(*this, pinned, std::move(whole), "}]}}", false);
        } else {
            const std::string text = buf->text(e.start, len);
            json::quote(m, text);
            m += "}]}}";
            send(*server->conn, std::move(m));
            ++hub->sent_;
            // Typing along a line: the column after what went in is known
            // from the one before it, without counting from the line's start.
            if (server->caps.utf16 && e.new_end_row == e.start_row) {
                col_line = buf->line_start(e.start_row);
                col_off = e.new_end;
                col_units = c0 + utf16_units(text);
            }
        }
    }
    if (server->caps.pull) hub->pull_document(this);
}

Lsp::Document* Lsp::doc_of(u64 key) const {
    for (const std::unique_ptr<Document>& d : docs_)
        if (d->key == key) return d.get();
    return nullptr;
}

static void open_on_server(Lsp::Document& d, bool pinned, u64* sent) {
    std::string head = R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":)";
    json::quote(head, d.uri);
    head += R"(,"languageId":)";
    json::quote(head, d.language_id);
    head += R"(,"version":)" + std::to_string(d.version) + R"(,"text":)";
    send_document(d, pinned, std::move(head), "}}}", false);
    d.opened = true;
    if (d.ed) d.rev = d.ed->revision();
    ++*sent;
}

Lsp::Document* Lsp::add_document(Server* s, u64 key, Editor* ed, const Buffer* buf, const std::string& as_given, const std::string& full,
                                 const std::string& language) {
    auto d = std::make_unique<Document>();
    d->hub = this;
    d->server = s;
    d->key = key;
    d->ed = ed;
    d->buf = buf;
    d->as_given = as_given;
    d->path = full;
    std::string host, there;
    d->uri = path_to_uri(split_remote(full, &host, &there) ? there : full);  // (another machine's: the path there)
    d->language_id = language_id(language);
    d->change_head = R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":)";
    json::quote(d->change_head, d->uri);
    d->change_head += R"(,"version":)";
    if (ed) {
        d->rev = ed->revision();
        ed->listen(d.get());
    }
    ++s->docs;
    s->idle_since = 0;
    // (Problems it had while it was not open are now the open document's.)
    if (s->closed.erase(full)) ++problems_gen_;
    Document* raw = d.get();
    docs_.push_back(std::move(d));
    unopened_ = true;
    return raw;  // (told to its server by flush_opens(), once it is known whether it is the one in front)
}

// Documents not yet told to their servers (just opened, or their server
// just came up) are: the one in front with its buffer's pieces, the others
// copied.
void Lsp::flush_opens() {
    if (!unopened_) return;
    unopened_ = false;
    for (const std::unique_ptr<Document>& d : docs_) {
        if (!d->opened && !d->server->ready) unopened_ = true;  // (its server is still starting)
        if (d->opened || !d->server->ready) continue;
        open_on_server(*d, d.get() == front_, &sent_);
        if (d->server->caps.pull) pull_document(d.get());
    }
}

// Lets go of a document: its server is told (tell), its editor no longer
// reports to it, and whatever was said about it is forgotten.
void Lsp::drop_document(Document* d, bool tell) {
    if (!d) return;
    materialize(d);
    if (front_ == d) {
        front_ = nullptr;
        complete_id_ = hover_id_ = sig_id_ = def_id_ = refs_id_ = prepare_id_ = rename_id_ = actions_id_ = resolve_id_ = 0;
        place_id_ = highlight_id_ = outline_id_ = switch_id_ = 0;
        highlights_.clear();
        outline_.clear();
        complete_first_edit_ = ~0ull;
        items_.clear();
        ++items_serial_;
        hover_lines_.clear();
        sig_ = SignatureInfo{};
        def_ = LspLocation{};
    }
    if (d->ed) d->ed->unlisten(d);
    Server* s = d->server;
    if (s && s->conn && tell && d->opened) {
        std::string m = R"({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":)";
        json::quote(m, d->uri);
        m += "}}}";
        send(*s->conn, std::move(m));
        Conn::Out forget;
        forget.forget = true;
        forget.about = d->uri;
        queue(*s->conn, std::move(forget));
        ++sent_;
    }
    if (s && --s->docs == 0) s->idle_since = std::max<u64>(1, now_ns());
    if (format_key_ == d->key) format_id_ = 0;
    if (!d->diags.empty()) ++diags_gen_, ++problems_gen_;
    std::erase_if(docs_, [&](const std::unique_ptr<Document>& x) { return x.get() == d; });
}

bool Lsp::open(u64 key, Editor& ed, const std::string& path, const std::string& language, const std::vector<LspServer>& servers, bool* wait) {
    if (wait) *wait = false;
    if (Document* d = doc_of(key)) {
        if (d->as_given == path && d->ed == &ed) return true;
        drop_document(d, true);
    }
#if defined(_WIN32)
    (void)path;
    (void)language;
    (void)servers;
    return false;
#else
    if (language.empty() || path.empty()) return false;
    std::string host, dir;
    if (split_remote(path, &host, &dir)) {  // another machine's: which server is installed there is found out there
        std::vector<LspServer> candidates;
        for (const LspServer& s : servers)
            if (s.enabled && !s.command.empty() && std::ranges::find(s.languages, language) != s.languages.end()) candidates.push_back(s);
        if (candidates.empty()) return false;
        const std::string remote = "ssh://" + host;
        for (const std::unique_ptr<Server>& s : servers_) {
            if (s->remote != remote) continue;
            const bool same = s->ready ? std::ranges::any_of(candidates, [&](const LspServer& c) { return c.name == s->name; })
                                       : s->candidates.size() == candidates.size() &&
                                             std::ranges::equal(s->candidates, candidates, [](const std::string& a, const LspServer& b) { return a == b.name; });
            if (!same) continue;
            if (!s->ready) {  // still finding its project there: this file may or may not be in it
                if (wait) *wait = true;
                return false;
            }
            if (path.starts_with(s->root + "/")) {
                add_document(s.get(), key, &ed, &ed.buf, path, path, language);
                return true;
            }
        }
        Server* s = start_server(candidates[0], candidates[0].command[0], path.substr(0, path.rfind('/')), remote, candidates);
        if (!s) return false;
        add_document(s, key, &ed, &ed.buf, path, path, language);
        return true;
    }
    std::string program;
    const LspServer* server = pick(servers, language, &program);
    if (!server) return false;
    char real[PATH_MAX];
    if (!realpath(path.c_str(), real)) return false;  // not on disk yet: nothing a server could use
    const std::string full = real;
    const std::string root = find_root(full.substr(0, full.rfind('/')), server->roots);
    Server* s = nullptr;
    for (const std::unique_ptr<Server>& have : servers_)
        if (have->remote.empty() && have->name == server->name && have->program == program && have->root == root) s = have.get();
    if (!s) s = start_server(*server, program, root, {}, {});
    if (!s) return false;
    add_document(s, key, &ed, &ed.buf, path, full, language);
    return true;
#endif
}

Lsp::Server* Lsp::start_server(const LspServer& server, const std::string& program, const std::string& root, const std::string& remote,
                               std::vector<LspServer> candidates) {
#if defined(_WIN32)
    (void)server, (void)program, (void)root, (void)remote, (void)candidates;
    return nullptr;
#else
    auto conn = std::make_shared<Conn>();
    int wake[2];
    if (pipe(wake) != 0) return nullptr;
    for (const int fd : wake) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    conn->wake_r = wake[0];
    conn->wake_w = wake[1];
    conn->server = server;
    conn->program = program;
    conn->root = root;
    conn->remote = remote;
    conn->path_env = search_path();
    conn->inbox = inbox_;
    // Writing to a server that has died must be an error, not the end of
    // this program.
    signal(SIGPIPE, SIG_IGN);

    auto s = std::make_unique<Server>();
    s->id = next_server_++;
    conn->server_id = s->id;
    s->name = server.name;
    s->shown = server.name;
    s->program = program;
    s->root = root;
    s->remote = remote;
    if (!remote.empty()) {
        s->shown += "@" + remote.substr(6);  // (one that fails there may yet do here, and the other way round)
        for (const LspServer& c : candidates) s->candidates.push_back(c.name);
        conn->candidates = std::move(candidates);
    }
    s->conn = conn;
    Server* raw = s.get();
    servers_.push_back(std::move(s));
    std::thread(Conn::run, std::move(conn)).detach();
    return raw;
#endif
}

void Lsp::close_doc(u64 key) { drop_document(doc_of(key), true); }

bool Lsp::is_open(u64 key) const { return doc_of(key) != nullptr; }

const std::string* Lsp::opened_as(u64 key) const {
    const Document* d = doc_of(key);
    return d ? &d->as_given : nullptr;
}

u64 Lsp::key_of(const std::string& path) const {
    for (const std::unique_ptr<Document>& d : docs_)
        if (d->path == path && d->key != ~0ull) return d->key;
    return 0;
}

void Lsp::refresh(u64 key) {
    Document* d = doc_of(key);
    if (!d || !d->ed || d->ed->revision() == d->rev) return;
    d->rev = d->ed->revision();
    d->col_line = ~0ull;
    // What was said about the old text no longer lines up with this one.
    const u64 size = d->buf->size();
    for (Diagnostic& x : d->diags) x.start = std::min(x.start, size), x.end = std::min(x.end, size);
    d->hints.clear();
    ++d->hints_gen;
    d->tokens.clear();
    ++d->tokens_gen;
    ++diags_gen_;
    ++problems_gen_;
    if (!d->opened || d->server->caps.sync == 0) return;
    ++d->version;
    std::string m = d->change_head + std::to_string(d->version) + R"(},"contentChanges":[{"text":)";
    send_document(*d, d == front_, std::move(m), "}]}}", true);
    ++sent_;
    d->hints_stale = true;
    d->tokens_version = -1;
    if (d->server->caps.pull) pull_document(d);
}

void Lsp::follow(u64 key) {
    Document* d = key ? doc_of(key) : nullptr;
    if (d == front_) {
        if (d) last_ = d->server;
        return;
    }
    Document* was = front_;
    materialize(was);
    // What was asked about the one in front is for nobody now.
    complete_id_ = hover_id_ = sig_id_ = def_id_ = refs_id_ = prepare_id_ = rename_id_ = actions_id_ = resolve_id_ = 0;
    place_id_ = highlight_id_ = outline_id_ = switch_id_ = 0;
    highlights_.clear();
    outline_.clear();
    complete_first_edit_ = ~0ull;
    items_.clear();
    ++items_serial_;
    hover_lines_.clear();
    sig_ = SignatureInfo{};
    def_ = LspLocation{};
    front_ = d;
    last_ = d ? d->server : nullptr;
    ++diags_gen_;
    flush_opens();
}

void Lsp::unfollow() {
    if (!front_) return;
    Server* keep = front_->server;
    follow(0);
    last_ = keep;
}

u64 Lsp::front() const { return front_ ? front_->key : 0; }

void Lsp::stop_server(Server* s, bool kill_now) {
    if (!s) return;
    std::vector<Document*> on;
    for (const std::unique_ptr<Document>& d : docs_)
        if (d->server == s) on.push_back(d.get());
    for (Document* d : on) drop_document(d, !kill_now);
    if (last_ == s) last_ = nullptr;
    if (asked_ == s) asked_ = nullptr;
    std::erase_if(applies_, [&](const Apply& a) { return a.server == s; });
    if (applying_.server == s) applying_.server = nullptr;
#if !defined(_WIN32)
    if (s->conn) {
        const int pid = s->conn->pid.load();
        {
            std::lock_guard lock(s->conn->mu);
            s->conn->stop = true;
        }
        s->conn->wake();
        if (kill_now && pid > 0) kill(pid, SIGTERM);
    }
#endif
    if (!s->closed.empty()) ++problems_gen_;
    std::erase_if(servers_, [&](const std::unique_ptr<Server>& x) { return x.get() == s; });
}

void Lsp::shutdown() {
    while (!servers_.empty()) stop_server(servers_.back().get(), false);
    docs_.clear();
    front_ = nullptr;
    last_ = nullptr;
    ++diags_gen_;
    ++problems_gen_;
}

void Lsp::tick() {
    const u64 now = now_ns();
    for (size_t i = 0; i < servers_.size();) {
        Server* s = servers_[i].get();
        if (s->docs == 0 && s->idle_since && now - s->idle_since >= idle_stop_ms_ * 1'000'000 && s != last_) {
            stop_server(s, false);
            continue;
        }
        ++i;
    }
}

u64 Lsp::next_tick_ns() const {
    u64 due = 0;
    for (const std::unique_ptr<Server>& s : servers_)
        if (s->docs == 0 && s->idle_since && s.get() != last_) {
            const u64 at = s->idle_since + idle_stop_ms_ * 1'000'000;
            due = due ? std::min(due, at) : at;
        }
    return due;
}

std::string Lsp::servers_dump() const {
    std::vector<std::string> rows;
    for (const std::unique_ptr<Server>& s : servers_) {
        const size_t slash = s->root.rfind('/');
        rows.push_back(s->shown + " " + (slash == std::string::npos ? s->root : s->root.substr(slash + 1)) + " " + std::to_string(s->docs) +
                       (s->ready ? "" : " starting"));
    }
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (const std::string& r : rows) out += (out.empty() ? "" : "; ") + r;
    return out.empty() ? "no servers" : out;
}

size_t Lsp::server_count() const { return servers_.size(); }

// ---------------------------------------------------------------- attach (tests and benchmarks)

namespace {
constexpr u64 kAttached = ~0ull;  // the key of attach()'s document
}

bool Lsp::attach(const Buffer& buf, const std::string& path, const std::string& language, const std::vector<LspServer>& servers) {
    if (Document* d = doc_of(kAttached)) drop_document(d, true);
#if defined(_WIN32)
    (void)buf, (void)path, (void)language, (void)servers;
    return false;
#else
    // (A server let go of by release() is used again if it is the one wanted.)
    std::string program;
    const LspServer* server = pick(servers, language, &program);
    char real[PATH_MAX];
    if (!server || path.empty() || !realpath(path.c_str(), real)) {
        detach();
        while (!servers_.empty()) stop_server(servers_.back().get(), false);
        return false;
    }
    const std::string full = real;
    const std::string root = find_root(full.substr(0, full.rfind('/')), server->roots);
    Server* s = nullptr;
    for (const std::unique_ptr<Server>& have : servers_)
        if (have->name == server->name && have->program == program && have->root == root) s = have.get();
    if (!s) s = start_server(*server, program, root, {}, {});
    if (!s) return false;
    Document* d = add_document(s, kAttached, nullptr, &buf, path, full, language);
    follow(kAttached);
    (void)d;
    return true;
#endif
}

void Lsp::release() {
    Document* d = doc_of(kAttached);
    if (!d) return;
    Server* s = d->server;
    if (!s->ready) return detach();  // still starting: not worth keeping
    drop_document(d, true);
    last_ = s;
}

void Lsp::detach() {
    Document* d = front_ ? front_ : doc_of(kAttached);
    Server* s = d ? d->server : last_;
    if (d) drop_document(d, true);
    if (s && s->docs == 0) stop_server(s, false);
    last_ = nullptr;
}

void Lsp::before_edit(u64 off, u64 remove_len) {
    if (front_ && !front_->ed) front_->before_edit(off, remove_len);
}

void Lsp::on_edit(const TextEdit& e) {
    if (front_ && !front_->ed) front_->on_edit(e);
}

// ---------------------------------------------------------------- the document in front

Lsp::Lsp() : inbox_(std::make_shared<Inbox>()) {}

Lsp::~Lsp() {
    // The program is closing and will not wait for a polite goodbye: the
    // servers are told to go now.
    while (!servers_.empty()) stop_server(servers_.back().get(), true);
    docs_.clear();
}

void Lsp::set_notify(std::function<void()> fn) {
    std::lock_guard lock(inbox_->mu);
    inbox_->notify = std::move(fn);
}

std::string Lsp::find_program(const std::string& name) {
#if defined(_WIN32)
    (void)name;
    return {};
#else
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos) {
        const std::string full = name.starts_with("~/") ? home_dir() + name.substr(1) : name;
        return runnable(full) ? full : std::string();
    }
    const std::string path = search_path();
    size_t at = 0;
    while (at <= path.size()) {
        const size_t colon = path.find(':', at);
        const std::string dir = path.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (!dir.empty() && runnable(dir + "/" + name)) return dir + "/" + name;
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    return {};
#endif
}

const LspServer* Lsp::pick(const std::vector<LspServer>& servers, std::string_view language, std::string* program) {
    if (language.empty()) return nullptr;
    for (const LspServer& s : servers) {
        if (!s.enabled || s.command.empty() || std::find(s.languages.begin(), s.languages.end(), language) == s.languages.end()) continue;
        std::string found = find_program(s.command[0]);
        if (found.empty()) continue;
        if (program) *program = std::move(found);
        return &s;
    }
    return nullptr;
}

bool Lsp::attached() const { return last_ != nullptr; }
bool Lsp::idle() const { return last_ && !front_; }
bool Lsp::ready() const { return last_ && last_->ready; }
bool Lsp::front_ready() const { return front_ && front_->opened && front_->server->ready; }

const std::string& Lsp::server_name() const {
    static const std::string none;
    return last_ ? last_->shown : none;
}
const std::string& Lsp::root() const {
    static const std::string none;
    return last_ ? last_->root : none;
}
const std::string& Lsp::path() const {
    static const std::string none;
    return front_ ? front_->path : none;
}
bool Lsp::counts_utf16() const { return last_ ? last_->caps.utf16 : true; }

const std::vector<Diagnostic>& Lsp::diagnostics() const {
    static const std::vector<Diagnostic> none;
    return front_ ? front_->diags : none;
}

const std::vector<Diagnostic>* Lsp::diagnostics_of(u64 key) const {
    const Document* d = doc_of(key);
    return d ? &d->diags : nullptr;
}

bool Lsp::can_complete() const { return front_ready() && front_->server->caps.completion; }
bool Lsp::can_hover() const { return front_ready() && front_->server->caps.hover; }
bool Lsp::can_signature() const { return front_ready() && front_->server->caps.signature; }
bool Lsp::can_define() const { return front_ready() && front_->server->caps.definition; }
bool Lsp::can_references() const { return front_ready() && front_->server->caps.references; }
bool Lsp::can_place(PlaceKind kind) const {
    if (!front_ready()) return false;
    const Caps& c = front_->server->caps;
    return kind == kImplementation ? c.implementation : kind == kTypeDefinition ? c.type_definition : c.declaration;
}
bool Lsp::can_highlight() const { return front_ready() && front_->server->caps.highlight; }
bool Lsp::can_outline() const { return front_ready() && front_->server->caps.outline; }
bool Lsp::can_find_symbols() const { return front_ready() && front_->server->caps.symbols; }
bool Lsp::can_switch() const { return front_ready() && front_->server->shown.find("clangd") != std::string::npos; }
bool Lsp::can_rename() const { return front_ready() && front_->server->caps.rename; }
bool Lsp::can_format(bool range) const { return front_ready() && (range ? front_->server->caps.range_formatting || front_->server->caps.formatting : front_->server->caps.formatting); }
bool Lsp::can_act() const { return front_ready() && front_->server->caps.actions; }
bool Lsp::completion_trigger(char c) const { return front_ready() && front_->server->caps.completion_triggers.find(c) != std::string::npos; }
bool Lsp::signature_trigger(char c) const { return front_ready() && front_->server->caps.signature_triggers.find(c) != std::string::npos; }

void Lsp::did_save() {
    Document* d = front_;
    if (!d || !d->opened || !d->server->caps.save) {
        if (d && d->server->caps.workspace_pull) pull_workspace(d->server);
        return;
    }
    std::string m = R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":)";
    json::quote(m, d->uri);
    m += "}}}";
    send(*d->server->conn, std::move(m));
    ++sent_;
    if (d->server->caps.workspace_pull) pull_workspace(d->server);
}

void Lsp::cancel(Server* s, u32* latest) {
    if (*latest && s && s->conn) {  // the answer still under way is no longer wanted
        send(*s->conn, R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":)" + std::to_string(*latest) + "}}");
        ++sent_;
    }
    *latest = 0;
}

u32 Lsp::ask(Document* d, const char* method, u64 offset, u8 kind, std::string_view extra, u32* latest) {
    cancel(asked_ ? asked_ : d->server, latest);
    asked_ = d->server;
    const u32 id = next_id_++;
    std::string m = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + method + R"(","params":{"textDocument":{"uri":)";
    json::quote(m, d->uri);
    m += R"(},"position":)";
    d->position_json(m, offset);
    m += extra;
    m += "}}";
    send(*d->server->conn, std::move(m), id, kind);
    ++sent_;
    *latest = id;
    return id;
}

void Lsp::complete(u64 offset, char trigger) {
    if (!can_complete()) return;
    std::string context;
    if (trigger) {
        context = R"(,"context":{"triggerKind":2,"triggerCharacter":)";
        json::quote(context, std::string_view(&trigger, 1));
        context += '}';
    } else {
        context = R"(,"context":{"triggerKind":1})";
    }
    ask(front_, "textDocument/completion", offset, kInCompletion, context, &complete_id_);
    complete_at_ = offset;
    complete_first_edit_ = ~0ull;
}

void Lsp::hover(u64 offset) {
    if (!can_hover()) return;
    ask(front_, "textDocument/hover", offset, kInHover, {}, &hover_id_);
    hover_at_ = offset;
}

void Lsp::signature(u64 offset) {
    if (!can_signature()) return;
    ask(front_, "textDocument/signatureHelp", offset, kInSignature, {}, &sig_id_);
}

void Lsp::definition(u64 offset) {
    if (!can_define()) return;
    ask(front_, "textDocument/definition", offset, kInDefinition, {}, &def_id_);
}

void Lsp::references(u64 offset) {
    if (!can_references()) return;
    ask(front_, "textDocument/references", offset, kInReferences, R"(,"context":{"includeDeclaration":true})", &refs_id_);
}

void Lsp::places(u64 offset, PlaceKind kind) {
    if (!can_place(kind)) return;
    static const char* const kMethods[] = {"textDocument/implementation", "textDocument/typeDefinition", "textDocument/declaration"};
    place_kind_ = kind;
    ask(front_, kMethods[kind], offset, kInPlaces, {}, &place_id_);
}

void Lsp::highlight(u64 offset) {
    if (!can_highlight()) return;
    highlight_at_ = offset;
    highlight_doc_ = front_;
    ask(front_, "textDocument/documentHighlight", offset, kInHighlight, {}, &highlight_id_);
}

// A question whose parameters are not a document and a place.
u32 Lsp::ask_with(Server* s, const char* method, const std::string& params, u8 kind, u32* latest) {
    cancel(asked_ ? asked_ : s, latest);
    asked_ = s;
    const u32 id = next_id_++;
    std::string m = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + method + R"(","params":)" + params + "}";
    send(*s->conn, std::move(m), id, kind);
    ++sent_;
    *latest = id;
    return id;
}

void Lsp::outline() {
    if (!can_outline()) return;
    outline_doc_ = front_;
    std::string p = R"({"textDocument":{"uri":)";
    json::quote(p, front_->uri);
    p += "}}";
    ask_with(front_->server, "textDocument/documentSymbol", p, kInOutline, &outline_id_);
}

void Lsp::find_symbols(const std::string& query) {
    if (!can_find_symbols()) return;
    found_asked_ = query;
    std::string p = R"({"query":)";
    json::quote(p, query);
    p += "}";
    ask_with(front_->server, "workspace/symbol", p, kInSymbols, &found_id_);
}

void Lsp::switch_header() {
    if (!can_switch()) return;
    switch_.clear();
    std::string p = R"({"uri":)";
    json::quote(p, front_->uri);
    p += "}";
    ask_with(front_->server, "textDocument/switchSourceHeader", p, kInSwitch, &switch_id_);
}

const char* symbol_kind_name(u8 kind) {
    static const char* const kNames[] = {"",          "file",     "module",   "namespace", "package",  "class",       "method",
                                         "property",  "field",    "constructor", "enum",   "interface", "function",   "variable",
                                         "constant",  "string",   "number",   "boolean",   "array",    "object",      "key",
                                         "null",      "enum member", "struct", "event",    "operator", "type parameter"};
    return kind < std::size(kNames) ? kNames[kind] : "";
}

// The word at a place, as a rename starts from when the server does not
// say which bytes the name is.
static void word_around(const Buffer& b, u64 off, u64* lo, u64* hi) {
    auto word = [](u8 c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
    char ch;
    u64 a = off, z = off;
    while (a > 0 && off - a < 400 && b.read(a - 1, &ch, 1) == 1 && word(static_cast<u8>(ch))) --a;
    while (z < b.size() && z - off < 400 && b.read(z, &ch, 1) == 1 && word(static_cast<u8>(ch))) ++z;
    *lo = a;
    *hi = z;
}

void Lsp::prepare_rename(u64 offset) {
    if (!can_rename()) return;
    rename_at_ = offset;
    rename_error_.clear();
    if (!front_->server->caps.prepare_rename) {  // nothing to ask: the word there
        word_around(*front_->buf, offset, &rename_lo_, &rename_hi_);
        rename_text_ = front_->buf->text(rename_lo_, rename_hi_ - rename_lo_);
        In m;
        m.kind = kInPrepareRename;
        m.server = front_->server->id;
        m.id = prepare_id_ = next_id_++;
        m.version = 0;
        inbox_->push(std::move(m));
        return;
    }
    ask(front_, "textDocument/prepareRename", offset, kInPrepareRename, {}, &prepare_id_);
}

void Lsp::rename(u64 offset, const std::string& name) {
    if (!can_rename()) return;
    std::string extra = R"(,"newName":)";
    json::quote(extra, name);
    ask(front_, "textDocument/rename", offset, kInRename, extra, &rename_id_);
}

void Lsp::format(u64 lo, u64 hi, bool whole, u64 key) {
    Document* d = key ? doc_of(key) : front_;
    if (!d || !d->opened || !d->server->ready) return;
    const Caps& caps = d->server->caps;
    if (!whole && !caps.range_formatting) whole = true;
    if (whole ? !caps.formatting : !caps.range_formatting) return;
    cancel(d->server, &format_id_);
    const u32 id = next_id_++;
    json::Writer w;
    w.begin_object().member("jsonrpc", "2.0").member("id", id).member("method", whole ? "textDocument/formatting" : "textDocument/rangeFormatting");
    w.key("params").begin_object().key("textDocument").begin_object().member("uri", d->uri).end_object();
    if (!whole) {
        std::string range = R"({"start":)";
        d->position_json(range, lo);
        range += R"(,"end":)";
        d->position_json(range, hi);
        range += "}";
        w.key("range").raw(range);
    }
    // (How wide a tab is and whether to indent with tabs, as the editor has them.)
    const u32 tab = d->ed ? d->ed->tab_width : 4;
    bool tabs = false;
    if (d->ed) d->ed->guess_indent(&tabs);
    w.key("options").begin_object().member("tabSize", tab).member("insertSpaces", !tabs).member("trimTrailingWhitespace", true).end_object();
    w.end_object().end_object();
    send(*d->server->conn, std::move(w.out), id, kInFormat);
    ++sent_;
    format_id_ = id;
    format_key_ = d->key;
    format_version_ = d->version;
    format_edits_.clear();
    format_failed_ = false;
}

void Lsp::code_actions(u64 lo, u64 hi) {
    if (!can_act()) return;
    Document* d = front_;
    cancel(d->server, &actions_id_);
    asked_ = d->server;
    const u32 id = next_id_++;
    std::string m = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"textDocument/codeAction","params":{"textDocument":{"uri":)";
    json::quote(m, d->uri);
    m += R"(},"range":{"start":)";
    d->position_json(m, lo);
    m += R"(,"end":)";
    d->position_json(m, hi);
    m += R"(},"context":{"diagnostics":[)";
    bool first = true;
    for (const Diagnostic& x : d->diags) {
        if (x.raw.empty() || x.start > hi || std::max(x.end, x.start + 1) < lo) continue;
        if (!first) m += ',';
        first = false;
        m += x.raw;
    }
    m += "]}}}";
    send(*d->server->conn, std::move(m), id, kInActions);
    ++sent_;
    actions_id_ = id;
    actions_.clear();
}

void Lsp::resolve_action(const LspAction& a) {
    if (!front_ready() || a.resolve.empty() || !front_->server->caps.resolve) return;
    Server* s = front_->server;
    cancel(s, &resolve_id_);
    const u32 id = next_id_++;
    send(*s->conn, R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"codeAction/resolve","params":)" + a.resolve + "}", id, kInResolve);
    ++sent_;
    resolve_id_ = id;
}

void Lsp::execute(const LspAction& a) {
    if (!front_ready() || a.command.empty()) return;
    Server* s = front_->server;
    const u32 id = next_id_++;
    send(*s->conn, R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"workspace/executeCommand","params":)" + a.command + "}", id, kInIgnore);
    ++sent_;
}

bool Lsp::take_apply(LspWorkspaceEdit* edit, std::string* label) {
    if (applies_.empty()) return false;
    applying_ = std::move(applies_.front());
    applies_.erase(applies_.begin());
    *edit = std::move(applying_.edit);
    *label = applying_.label;
    return true;
}

void Lsp::answer_apply(bool applied) {
    Server* s = applying_.server;
    if (!s || applying_.id.empty()) return;
    send(*s->conn, R"({"jsonrpc":"2.0","id":)" + applying_.id + R"(,"result":{"applied":)" + (applied ? "true" : "false") + "}}");
    ++sent_;
    applying_ = Apply{};
}

void Lsp::pull_document(Document* d) {
    if (!d->opened || !d->server->ready || !d->server->caps.pull) return;
    if (d->pull_id) {  // one at a time: the next when this one is answered
        d->pull_again = true;
        return;
    }
    const u32 id = next_id_++;
    std::string m = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":)";
    json::quote(m, d->uri);
    m += "}}}";
    send(*d->server->conn, std::move(m), id, kInPull, d->uri);
    ++sent_;
    d->pull_id = id;
    d->pull_version = d->version;
    d->pull_again = false;
}

void Lsp::pull_workspace(Server* s) {
    if (!s->ready || !s->caps.workspace_pull || s->workspace_id) return;
    const u32 id = next_id_++;
    send(*s->conn, R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"workspace/diagnostic","params":{"previousResultIds":[]}})", id,
         kInWorkspacePull);
    ++sent_;
    s->workspace_id = id;
}

bool Lsp::can_hint(u64 key) const {
    const Document* d = doc_of(key);
    return d && d->opened && d->server->ready && d->server->caps.hints;
}

void Lsp::ask_hints(u64 key, u64 first_line, u64 last_line) {
    Document* d = doc_of(key);
    if (!d || !d->opened || !d->server->ready || !d->server->caps.hints) return;
    if (d->hints_id) return;  // (one at a time: once it is answered, the lines then on show are asked for if it did not cover them)
    const u32 id = next_id_++;
    const u64 lines = d->buf->line_count();
    last_line = std::min(last_line, lines - 1);
    std::string m = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"textDocument/inlayHint","params":{"textDocument":{"uri":)";
    json::quote(m, d->uri);
    m += R"(},"range":{"start":{"line":)" + std::to_string(first_line) + R"(,"character":0},"end":)";
    const u64 end = last_line + 1 < lines ? d->buf->line_start(last_line + 1) : d->buf->size();
    d->position_json(m, end);
    m += "}}}";
    send(*d->server->conn, std::move(m), id, kInHints);
    ++sent_;
    d->hints_id = id;
    d->hints_version = d->version;
    d->hints_first = first_line;
    d->hints_last = last_line;
    d->hints_stale = false;
}

const std::vector<InlayHint>* Lsp::hints(u64 key) const {
    const Document* d = doc_of(key);
    return d ? &d->hints : nullptr;
}

u64 Lsp::hints_generation(u64 key) const {
    const Document* d = doc_of(key);
    return d ? d->hints_gen : 0;
}

bool Lsp::hints_cover(u64 key, u64 first_line, u64 last_line) const {
    const Document* d = doc_of(key);
    return d && !d->hints_stale && d->hints_first <= first_line && d->hints_last >= last_line;
}

bool Lsp::can_tokens(u64 key) const {
    const Document* d = doc_of(key);
    return d && d->opened && d->server->ready && d->server->caps.tokens;
}

void Lsp::ask_tokens(u64 key) {
    Document* d = doc_of(key);
    if (!d || !d->opened || !d->server->ready || !d->server->caps.tokens) return;
    if (d->tokens_id) return;  // (the answer under way says what it says; the version check asks again after)
    const u32 id = next_id_++;
    const bool delta = d->server->caps.tokens_delta && !d->tokens_result.empty();
    std::string m = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"textDocument/semanticTokens/full)" + (delta ? "/delta" : "") +
                    R"(","params":{"textDocument":{"uri":)";
    json::quote(m, d->uri);
    m += "}";
    if (delta) {
        m += R"(,"previousResultId":)";
        json::quote(m, d->tokens_result);
    }
    m += "}}";
    send(*d->server->conn, std::move(m), id, kInTokens, d->uri);
    ++sent_;
    d->tokens_id = id;
    d->tokens_version = d->version;
}

const std::vector<SemanticToken>* Lsp::tokens(u64 key) const {
    const Document* d = doc_of(key);
    return d ? &d->tokens : nullptr;
}

u64 Lsp::tokens_generation(u64 key) const {
    const Document* d = doc_of(key);
    return d ? d->tokens_gen : 0;
}

bool Lsp::tokens_current(u64 key) const {
    const Document* d = doc_of(key);
    return d && d->tokens_version == d->version;
}

const std::vector<std::string>* Lsp::token_types(u64 key) const {
    const Document* d = doc_of(key);
    return d ? &d->server->caps.token_types : nullptr;
}

const std::vector<std::string>* Lsp::token_modifiers(u64 key) const {
    const Document* d = doc_of(key);
    return d ? &d->server->caps.token_mods : nullptr;
}

// ---------------------------------------------------------------- taking in answers

void Lsp::server_gone(Server* s, const std::string& why) {
    message_ = "language server: " + why;
    failed_.push_back(s->shown);
    // (Nobody is left to say goodbye to.)
    for (const std::unique_ptr<Document>& d : docs_)
        if (d->server == s) d->opened = false;
    stop_server(s, false);
}

static void convert_diags(Lsp::Document& d, std::vector<RawDiag>& raw, bool utf16) {
    d.diags.clear();
    d.diags.reserve(raw.size());
    for (RawDiag& r : raw) {
        Diagnostic x;
        x.start = Lsp::offset_at(*d.buf, r.r.l0, r.r.c0, utf16);
        x.end = std::max(x.start, Lsp::offset_at(*d.buf, r.r.l1, r.r.c1, utf16));
        x.severity = r.severity;
        x.fixes = r.fixes;
        x.message = std::move(r.message);
        x.source = std::move(r.source);
        x.code = std::move(r.code);
        x.raw = std::move(r.raw);
        d.diags.push_back(std::move(x));
    }
    std::stable_sort(d.diags.begin(), d.diags.end(), [](const Diagnostic& a, const Diagnostic& b) { return a.start < b.start; });
}

u32 Lsp::poll() {
    if (!inbox_->any.load(std::memory_order_acquire) && applies_.empty()) {
        flush_opens();
        return 0;
    }
    std::vector<In> in;
    {
        std::lock_guard lock(inbox_->mu);
        in.swap(inbox_->in);
        inbox_->any.store(false, std::memory_order_relaxed);
    }
    u32 news = 0;
    for (In& m : in) {
        Server* s = nullptr;
        for (const std::unique_ptr<Server>& x : servers_)
            if (x->id == m.server) s = x.get();
        if (!s) continue;  // (one that has been stopped: what it says is for nobody)
        auto doc_by_path = [&](const std::string& path) -> Document* {
            for (const std::unique_ptr<Document>& d : docs_)
                if (d->server == s && d->path == path) return d.get();
            return nullptr;
        };
        const bool front_asked = front_ && front_->server == s;
        switch (m.kind) {
            case kInReady: {
                s->caps = std::move(m.caps);
                s->ready = true;
                if (!m.text.empty()) {  // another machine's: which server it turned out to be, and its project's folder there
                    const size_t a = m.text.find('\n'), b = m.text.find('\n', a + 1);
                    s->shown = m.text.substr(0, a);
                    s->name = m.text.substr(a + 1, b - a - 1);
                    s->root = m.text.substr(b + 1);
                }
                if (s->caps.workspace_pull) pull_workspace(s);
                unopened_ = true;  // (its documents go to it below)
                news |= kState;
                break;
            }
            case kInFailed:
            case kInStopped:
                server_gone(s, m.text);
                news |= kState | kDiagnostics;
                break;
            case kInDiagnostics: {
                Document* d = doc_by_path(m.text);
                if (d) {
                    if (m.version >= 0 && m.version != d->version) break;
                    convert_diags(*d, m.diags, s->caps.utf16);
                } else if (m.diags.empty()) {
                    if (!s->closed.erase(m.text)) break;
                } else {
                    std::vector<ProblemFile::Item>& items = s->closed[m.text];
                    items.clear();
                    for (RawDiag& r : m.diags) items.push_back({r.r.l0, r.r.c0, r.severity, ~0ull, std::move(r.message)});
                }
                ++diags_gen_;
                ++problems_gen_;
                news |= kDiagnostics;
                break;
            }
            case kInPull: {
                Document* d = nullptr;
                for (const std::unique_ptr<Document>& x : docs_)
                    if (x->server == s && x->pull_id == m.id) d = x.get();
                if (!d) break;
                d->pull_id = 0;
                if (m.flag && d->pull_version == d->version) {
                    convert_diags(*d, m.diags, s->caps.utf16);
                    ++diags_gen_;
                    ++problems_gen_;
                    news |= kDiagnostics;
                }
                if (d->pull_again || d->pull_version != d->version) pull_document(d);
                break;
            }
            case kInWorkspacePull:
                if (s->workspace_id == m.id) s->workspace_id = 0;
                break;
            case kInCompletion: {
                if (m.id != complete_id_ || !front_asked) break;
                complete_id_ = 0;
                // Positions in the answer are about the text as it was when
                // the question was asked. Typing has usually gone on since,
                // further along the line; everything before the first such
                // change is where it was, and only that much is trusted.
                // (A list of thousands is thousands of strings to free: that
                // is done on a thread of its own, not on the one that draws.)
                if (items_.size() > 2000) std::thread([gone = std::move(items_)] {}).detach();
                items_.clear();
                ++items_serial_;
                items_.reserve(m.items.size());
                // Nearly every item replaces the same word, so the position
                // worked out for one is usually the next one's too.
                u32 last_line = ~0u, last_col = ~0u;
                u64 last_from = 0;
                const Buffer& b = *front_->buf;
                for (RawItem& r : m.items) {
                    CompletionItem& it = r.item;
                    if (r.has_range) {
                        if (r.range.l0 != last_line || r.range.c0 != last_col) {
                            last_line = r.range.l0;
                            last_col = r.range.c0;
                            last_from = offset_at(b, last_line, last_col, s->caps.utf16);
                        }
                        if (last_from <= complete_first_edit_) it.replace_from = last_from;
                    }
                    for (RawItem::Extra& x : r.extra) {
                        const u64 a = offset_at(b, x.r.l0, x.r.c0, s->caps.utf16), z = offset_at(b, x.r.l1, x.r.c1, s->caps.utf16);
                        if (a <= z && z <= complete_first_edit_) it.extra.push_back({a, z, std::move(x.text)});
                    }
                    items_.push_back(std::move(it));
                }
                items_incomplete_ = m.incomplete;
                news |= kCompletion;
                break;
            }
            case kInHover:
                if (m.id != hover_id_ || !front_asked) break;
                hover_id_ = 0;
                hover_lines_ = std::move(m.lines);
                news |= kHover;
                break;
            case kInSignature:
                if (m.id != sig_id_ || !front_asked) break;
                sig_id_ = 0;
                sig_ = std::move(m.sig);
                news |= kSignature;
                break;
            case kInDefinition:
                if (m.id != def_id_ || !front_asked) break;
                def_id_ = 0;
                def_ = std::move(m.loc);
                news |= kDefinition;
                break;
            case kInReferences:
            case kInPlaces: {
                const bool refs = m.kind == kInReferences;
                if (m.id != (refs ? refs_id_ : place_id_) || !front_asked) break;
                (refs ? refs_id_ : place_id_) = 0;
                std::vector<LspPlace>& list = refs ? refs_ : places_;
                list = std::move(m.places);
                // Lines in files that are open come from their tabs, as they are now.
                for (size_t i = 0; i < list.size() && i < m.ranges.size(); ++i) {
                    LspPlace& p = list[i];
                    const RawRange& r = m.ranges[i];
                    const Document* d = doc_by_path(p.path);
                    if (!d || p.line >= d->buf->line_count()) continue;
                    const u64 ls = d->buf->line_start(p.line);
                    const u64 le = p.line + 1 < d->buf->line_count() ? d->buf->line_start(p.line + 1) - 1 : d->buf->size();
                    std::string line = d->buf->text(ls, std::min<u64>(le - ls, 4096));
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    p.col = static_cast<u32>(column_in(line, r.c0, s->caps.utf16));
                    p.end_col = r.l1 == r.l0 ? static_cast<u32>(column_in(line, r.c1, s->caps.utf16)) : static_cast<u32>(line.size());
                    if (line.size() > 400) line.resize(400);
                    p.text = std::move(line);
                }
                news |= refs ? kReferences : kPlaces;
                break;
            }
            case kInHighlight: {
                if (m.id != highlight_id_ || !front_asked || front_ != highlight_doc_) break;
                highlight_id_ = 0;
                highlights_.clear();
                for (const RawRange& r : m.ranges) {
                    const u64 a = offset_at(*front_->buf, r.l0, r.c0, s->caps.utf16), b = offset_at(*front_->buf, r.l1, r.c1, s->caps.utf16);
                    if (b > a) highlights_.push_back({a, b});
                }
                news |= kHighlights;
                break;
            }
            case kInOutline: {
                if (m.id != outline_id_ || !front_asked || front_ != outline_doc_) break;
                outline_id_ = 0;
                outline_ = std::move(m.symbols);
                for (LspSymbol& y : outline_) y.offset = offset_at(*front_->buf, y.line, y.character, s->caps.utf16);
                news |= kOutline;
                break;
            }
            case kInSymbols:
                if (m.id != found_id_) break;
                found_id_ = 0;
                found_ = std::move(m.symbols);
                found_query_ = found_asked_;
                news |= kSymbols;
                break;
            case kInSwitch:
                if (m.id != switch_id_ || !front_asked) break;
                switch_id_ = 0;
                switch_ = std::move(m.text);
                news |= kSwitch;
                break;
            case kInPrepareRename: {
                if (m.id != prepare_id_ || !front_asked) break;
                prepare_id_ = 0;
                if (m.flag) {
                    rename_lo_ = rename_hi_ = rename_at_;
                    rename_text_.clear();
                    rename_error_ = m.text;
                } else if (m.version == 0) {
                    if (m.range.l0 || m.range.c0 || m.range.l1 || m.range.c1 || rename_text_.empty()) {
                        word_around(*front_->buf, rename_at_, &rename_lo_, &rename_hi_);
                        rename_text_ = front_->buf->text(rename_lo_, rename_hi_ - rename_lo_);
                    }
                } else {
                    rename_lo_ = offset_at(*front_->buf, m.range.l0, m.range.c0, s->caps.utf16);
                    rename_hi_ = std::max(rename_lo_, offset_at(*front_->buf, m.range.l1, m.range.c1, s->caps.utf16));
                    rename_text_ = m.result_id.empty() ? front_->buf->text(rename_lo_, rename_hi_ - rename_lo_) : m.result_id;
                }
                news |= kRenameRange;
                break;
            }
            case kInRename:
                if (m.id != rename_id_ || !front_asked) break;
                rename_id_ = 0;
                rename_edit_ = std::move(m.wedit);
                rename_error_ = m.text;
                news |= kRename;
                break;
            case kInFormat: {
                if (m.id != format_id_) break;
                format_id_ = 0;
                format_edits_.clear();
                format_failed_ = m.flag;
                Document* d = doc_of(format_key_);
                if (!d || d->server != s) break;
                if (d->version == format_version_) {
                    for (LspTextEdit& t : m.edits) {
                        const u64 a = offset_at(*d->buf, t.l0, t.c0, s->caps.utf16), z = offset_at(*d->buf, t.l1, t.c1, s->caps.utf16);
                        if (z >= a) format_edits_.push_back({a, z, std::move(t.text)});
                    }
                } else {
                    format_failed_ = true;  // (the text changed meanwhile: the edits are about another one)
                }
                news |= kFormat;
                break;
            }
            case kInActions:
                if (m.id != actions_id_ || !front_asked) break;
                actions_id_ = 0;
                actions_ = std::move(m.actions);
                news |= kActions;
                break;
            case kInResolve:
                if (m.id != resolve_id_ || !front_asked) break;
                resolve_id_ = 0;
                actions_ = std::move(m.actions);
                for (LspAction& a : actions_) a.resolve.clear();
                news |= kActions;
                break;
            case kInHints: {
                Document* d = nullptr;
                for (const std::unique_ptr<Document>& x : docs_)
                    if (x->server == s && x->hints_id == m.id) d = x.get();
                if (!d) break;
                d->hints_id = 0;
                if (d->hints_version != d->version) {
                    d->hints_stale = true;  // (about a text that has changed since: asked again)
                    break;
                }
                d->hints.clear();
                const Buffer& b = *d->buf;
                for (RawHint& h : m.hints) {
                    InlayHint x;
                    x.off = offset_at(b, h.line, h.character, s->caps.utf16);
                    x.label = std::move(h.label);
                    x.kind = h.kind;
                    x.pad_left = h.pad_left;
                    x.pad_right = h.pad_right;
                    d->hints.push_back(std::move(x));
                }
                ++d->hints_gen;
                news |= kHints;
                break;
            }
            case kInTokens: {
                Document* d = nullptr;
                for (const std::unique_ptr<Document>& x : docs_)
                    if (x->server == s && x->tokens_id == m.id) d = x.get();
                if (!d) break;
                d->tokens_id = 0;
                if (m.flag) {
                    d->tokens_result.clear();
                    break;
                }
                d->tokens_result = std::move(m.result_id);
                if (d->tokens_version != d->version) {  // (about another text: the next frame asks again)
                    d->tokens_version = -1;
                    break;
                }
                // Each token is five numbers: lines down from the last, the
                // column (from the last one's, on the same line), length,
                // type and modifiers.
                d->tokens.clear();
                d->tokens.reserve(m.tokens.size() / 5);
                const Buffer& b = *d->buf;
                const bool utf16 = s->caps.utf16;
                u32 line = 0, col = 0;
                u64 ls = 0, le = 0;
                std::string text;  // (the line, when columns are UTF-16)
                const u64 lines = b.line_count();
                for (size_t i = 0; i + 5 <= m.tokens.size(); i += 5) {
                    const u32 dl = m.tokens[i], dc = m.tokens[i + 1], len = m.tokens[i + 2];
                    if (dl) {
                        line += dl;
                        col = dc;
                    } else {
                        col += dc;
                    }
                    if (i == 0 || dl) {
                        if (line >= lines) break;
                        ls = b.line_start(line);
                        le = line + 1 < lines ? b.line_start(line + 1) - 1 : b.size();
                        if (utf16) text = b.text(ls, std::min<u64>(le - ls, 1u << 20));
                    }
                    SemanticToken t;
                    if (utf16) {
                        t.start = ls + column_in(text, col, true);
                        t.end = ls + column_in(text, col + len, true);
                    } else {
                        t.start = std::min(ls + col, le);
                        t.end = std::min(ls + col + len, le);
                    }
                    if (t.end <= t.start) continue;
                    t.type = static_cast<u16>(std::min<u32>(m.tokens[i + 3], 0xFFFF));
                    t.mods = m.tokens[i + 4];
                    d->tokens.push_back(t);
                }
                ++d->tokens_gen;
                news |= kTokens;
                break;
            }
            case kInApplyEdit:
                applies_.push_back({s, std::move(m.text), std::move(m.result_id), std::move(m.wedit)});
                break;
            case kInRefresh:
                for (const std::unique_ptr<Document>& d : docs_) {
                    if (d->server != s) continue;
                    if (m.id == kRefreshTokens) d->tokens_version = -1, ++d->tokens_gen;
                    if (m.id == kRefreshHints) d->hints_stale = true, ++d->hints_gen;
                    if (m.id == kRefreshDiagnostics && s->caps.pull) pull_document(d.get());
                }
                if (m.id == kRefreshDiagnostics) pull_workspace(s);
                if (m.id == kRefreshTokens) news |= kTokens;
                if (m.id == kRefreshHints) news |= kHints;
                break;
            default: break;
        }
    }
    flush_opens();
    if (!applies_.empty()) news |= kApplyEdit;
    return news;
}

const std::vector<ProblemFile>& Lsp::problems() {
    if (problems_built_ == problems_gen_) return problems_;
    problems_built_ = problems_gen_;
    problems_.clear();
    std::map<std::string, size_t> at;  // path -> its entry
    auto file = [&](const std::string& path) -> ProblemFile& {
        const auto [it, fresh] = at.try_emplace(path, problems_.size());
        if (fresh) problems_.push_back({path, 0, {}});
        return problems_[it->second];
    };
    for (const std::unique_ptr<Document>& d : docs_) {
        if (d->diags.empty() || d->key == kAttached) continue;
        ProblemFile& f = file(d->path);
        f.key = d->key;
        const Buffer& b = *d->buf;
        for (const Diagnostic& x : d->diags) {
            const u64 off = std::min(x.start, b.size()), line = b.line_of(off);
            f.items.push_back({static_cast<u32>(line), static_cast<u32>(off - b.line_start(line)), x.severity, off, x.message});
        }
    }
    for (const std::unique_ptr<Server>& s : servers_)
        for (const auto& [path, items] : s->closed) {
            if (items.empty()) continue;
            ProblemFile& f = file(path);
            f.items.insert(f.items.end(), items.begin(), items.end());
        }
    for (ProblemFile& f : problems_)
        std::stable_sort(f.items.begin(), f.items.end(), [](const ProblemFile::Item& a, const ProblemFile::Item& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
    std::sort(problems_.begin(), problems_.end(), [](const ProblemFile& a, const ProblemFile& b) { return a.path < b.path; });
    return problems_;
}

std::string Lsp::take_message() {
    std::string m;
    m.swap(message_);
    return m;
}

std::vector<std::string> Lsp::take_failed() {
    std::vector<std::string> out;
    out.swap(failed_);
    return out;
}

u32 Lsp::utf16_units(std::string_view s) {
    u32 units = 0;
    for (const char ch : s) {
        const u8 c = static_cast<u8>(ch);
        units += static_cast<u32>((c & 0xC0) != 0x80) + static_cast<u32>(c >= 0xF0);
    }
    return units;
}

u64 Lsp::offset_at(const Buffer& b, u32 line, u32 character, bool utf16) {
    if (line >= b.line_count()) return b.size();
    const u64 start = b.line_start(line);
    const u64 limit = line + 1 < b.line_count() ? b.line_start(line + 1) - 1 : b.size();  // the line's newline, or the end
    if (!utf16) return std::min(start + character, limit);
    u32 units = 0;
    u64 off = start;
    for (Buffer::Reader rd = b.reader(start); off < limit;) {
        const std::string_view ch = rd.chunk();
        if (ch.empty()) break;
        const size_t n = static_cast<size_t>(std::min<u64>(limit - off, ch.size()));
        const u8* p = reinterpret_cast<const u8*>(ch.data());
        for (size_t i = 0; i < n; ++i) {
            if ((p[i] & 0xC0) == 0x80) continue;
            if (units >= character) return off + i;
            units += p[i] >= 0xF0 ? 2 : 1;
        }
        rd.advance(static_cast<u32>(n));
        off += n;
    }
    return limit;
}

u64 Lsp::offset_in(std::string_view text, u32 line, u32 character, bool utf16) {
    size_t at = 0;
    for (u32 l = 0; l < line; ++l) {
        const size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) return text.size();
        at = nl + 1;
    }
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    return at + column_in(text.substr(at, end - at), character, utf16);
}

bool Lsp::wait(int timeout_ms) {
    std::unique_lock lock(inbox_->mu);
    return inbox_->arrived.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !inbox_->in.empty(); });
}

}  // namespace blit
