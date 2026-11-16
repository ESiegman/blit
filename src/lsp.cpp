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
    std::string message, source, code;
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

enum : u8 { kInReady = 1, kInFailed, kInStopped, kInDiagnostics, kInCompletion, kInHover, kInSignature, kInDefinition };

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

}  // namespace

struct Lsp::Conn {
    struct Out {
        std::string head, tail;                          // the message, around the document text if there is one
        std::shared_ptr<const std::vector<Piece>> text;  // the document, when the message carries it
        u32 id = 0;                                      // of a request; 0 for a notification
        u8 kind = 0;                                     // what its answer will be
        bool whole = false;                              // a whole-document change, superseding an unsent older one
        bool dead = false;                               // superseded while queued: not to be sent
        u64 epoch = 0;                                   // the Conn's epoch when the text was taken
    };
    struct In {
        u8 kind = 0;
        u32 id = 0;
        Caps caps;
        std::string text;  // why it failed, or which document
        i64 version = -1;
        std::vector<RawDiag> diags;
        std::vector<RawItem> items;
        bool incomplete = false;
        std::vector<InfoLine> lines;
        SignatureInfo sig;
        LspLocation loc;
    };

    std::mutex mu;
    std::condition_variable arrived;
    std::vector<Out> out;  // main thread -> worker
    // The pieces of a document point into its buffer, which is gone (or
    // read again) when the document is closed. Closing raises the epoch
    // while holding text_mu, which the worker holds while it copies pieces:
    // text queued before is never read after, and one being read is waited for.
    std::mutex text_mu;
    std::atomic<u64> epoch{0};
    std::vector<In> in;    // worker -> main thread
    bool stop = false;
    int wake_r = -1, wake_w = -1;  // a byte written here wakes the worker
    std::atomic<int> pid{0};
    std::function<void()> notify;

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
    std::mutex probe_mu;
    std::string probed_root;  // the project's folder as found there (empty until it is)

    void push(In&& m) {
        {
            std::lock_guard lock(mu);
            in.push_back(std::move(m));
        }
        arrived.notify_all();
        if (notify) notify();
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

#if !defined(_WIN32)

// ---------------------------------------------------------------- the worker thread

struct Lsp::Conn::Worker {
    explicit Worker(Conn& conn) : c(conn) {}

    Conn& c;
    int to_fd = -1, from_fd = -1;  // the server's stdin and stdout
    std::string inbuf, outbuf;
    size_t in_at = 0, out_at = 0;
    std::unordered_map<u32, u8> waiting;  // request id -> what kind of answer it gets
    json::Doc settings;
    bool utf16 = true;

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
        std::lock_guard lock(c.probe_mu);
        c.probed_root = c.root;
        return true;
    }

    static u64 now_ms() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
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
        w.key("capabilities")
            .raw(R"({"general":{"positionEncodings":["utf-8","utf-16"]},)"
                 R"("offsetEncoding":["utf-8","utf-16"],)"
                 R"("textDocument":{)"
                 R"("synchronization":{"didSave":true},)"
                 R"("publishDiagnostics":{"versionSupport":true,"relatedInformation":false},)"
                 R"("completion":{"contextSupport":true,"completionItem":{"snippetSupport":false,"insertReplaceSupport":true,)"
                 R"("labelDetailsSupport":true,"documentationFormat":["plaintext","markdown"]}},)"
                 R"("hover":{"contentFormat":["markdown","plaintext"]},)"
                 R"("signatureHelp":{"signatureInformation":{"documentationFormat":["plaintext","markdown"],)"
                 R"("parameterInformation":{"labelOffsetSupport":true},"activeParameterSupport":true}},)"
                 R"("definition":{"linkSupport":true}},)"
                 R"("workspace":{"configuration":true,"workspaceFolders":true},)"
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

        frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        if (!c.server.settings.empty() && settings.parse(c.server.settings)) {
            frame(R"({"jsonrpc":"2.0","method":"workspace/didChangeConfiguration","params":{"settings":)" + c.server.settings + "}}");
        }
        In m;
        m.kind = kInReady;
        m.caps = caps;
        if (!c.remote.empty()) m.text = c.server.name + "@" + c.remote.substr(6);  // (the one chosen there)
        c.push(std::move(m));
    }

    // A request from the server. All of them get an answer, because a
    // server may wait for it.
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
            reply += R"(,"result":{"applied":false}})";
        } else if (method == "client/registerCapability" || method == "client/unregisterCapability" ||
                   method == "window/workDoneProgress/create" || method == "window/showMessageRequest" ||
                   method == "workspace/semanticTokens/refresh" || method == "workspace/inlayHint/refresh" ||
                   method == "workspace/diagnostic/refresh" || method == "workspace/codeLens/refresh") {
            reply += R"(,"result":null})";
        } else {
            reply += R"(,"error":{"code":-32601,"message":"not supported"}})";
        }
        frame(reply);
    }

    void on_diagnostics(Value params) {
        In m;
        m.kind = kInDiagnostics;
        m.text = from_uri(params["uri"].str());
        m.version = params["version"].is_number() ? params["version"].integer() : -1;
        for (Value d : params["diagnostics"]) {
            RawDiag r;
            r.r = range_of(d["range"]);
            r.severity = static_cast<u8>(std::clamp<i64>(d["severity"].integer(1), 1, 4));
            r.message = d["message"].str();
            while (!r.message.empty() && (r.message.back() == '\n' || r.message.back() == ' ')) r.message.pop_back();
            r.source = d["source"].str();
            const Value code = d["code"];
            if (code.is_string()) r.code = code.str();
            else if (code.is_number()) r.code = std::to_string(code.integer());
            m.diags.push_back(std::move(r));
        }
        c.push(std::move(m));
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
            if (it["insertTextFormat"].integer(1) == 2) o.insert = strip_snippet(o.insert);
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

    // Byte offset in s of a column counted the way the server counts.
    size_t column_in(std::string_view s, u32 col) const {
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
                    a = column_in(s.label, static_cast<u32>(label.at(0).integer()));
                    b = column_in(s.label, static_cast<u32>(label.at(1).integer()));
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
        const u8 kind = it->second;
        waiting.erase(it);
        switch (kind) {
            case kInCompletion: on_completion(n, result); break;
            case kInHover: on_hover(n, result); break;
            case kInSignature: on_signature(n, result); break;
            case kInDefinition: on_definition(n, result); break;
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
        std::vector<Out> out;
        {
            std::lock_guard lock(c.mu);
            out.swap(c.out);
        }
        // Of several whole-document changes with nothing between that
        // needs the older one, only the newest is worth sending.
        bool later_whole = false;
        for (size_t i = out.size(); i-- > 0;) {
            Out& o = out[i];
            if (o.whole) {
                if (later_whole) o.dead = true;
                later_whole = true;
            } else if (o.id || o.text || o.head.find("didClose") != std::string::npos) {
                later_whole = false;
            }
        }
        std::string body;
        for (Out& o : out) {
            if (o.dead) continue;
            if (o.id) waiting[o.id] = o.kind;
            if (!o.text) {
                frame(o.head);
                continue;
            }
            // The document's text, as one JSON string made piece by piece.
            std::string text;
            {
                std::lock_guard lock(c.text_mu);
                if (o.epoch != c.epoch.load(std::memory_order_relaxed)) continue;  // its document has been closed: the pieces may be gone
                size_t total = 0;
                for (const Piece& p : *o.text) total += p.len;
                text.reserve(total);
                for (const Piece& p : *o.text) text.append(p.p, p.len);
            }
            body = std::move(o.head);
            json::quote(body, text);
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
                    deadline = now_ms() + 300;
                }
            }
            if (stopping && (out_at >= outbuf.size() || now_ms() >= deadline)) break;

            pollfd fds[3] = {{from_fd, POLLIN, 0}, {c.wake_r, POLLIN, 0}, {to_fd, static_cast<short>(out_at < outbuf.size() ? POLLOUT : 0), 0}};
            const int timeout = stopping ? static_cast<int>(deadline - std::min(deadline, now_ms())) + 1 : -1;
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
            const u64 until = now_ms() + static_cast<u64>(ms);
            for (;;) {
                int status;
                const pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid || (r < 0 && errno != EINTR)) return true;
                if (now_ms() >= until) return false;
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

// ---------------------------------------------------------------- main thread

Lsp::Lsp() = default;

Lsp::~Lsp() {
    if (!conn_) return;
#if !defined(_WIN32)
    // The program is closing and will not wait for a polite goodbye: the
    // server is told to go now.
    const int pid = conn_->pid.load();
    detach();
    if (pid > 0) kill(pid, SIGTERM);
#endif
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

bool Lsp::attach(const Buffer& buf, const std::string& path, const std::string& language,
                 const std::vector<LspServer>& servers) {
#if !defined(_WIN32)
    // The server that is running may be the one this file wants as well,
    // which is the usual thing when moving between tabs: then the document
    // it had is closed, this one is opened, and nothing is started.
    if (conn_ && ready_ && is_remote(path) && path.starts_with(conn_->remote + "/")) {  // (another machine's: the same server, if the file is in its project)
        std::string probed;
        {
            std::lock_guard lock(conn_->probe_mu);
            probed = conn_->probed_root;
        }
        bool wanted = false;
        for (const LspServer& s : servers)
            wanted |= s.enabled && s.name == conn_->server.name && std::ranges::find(s.languages, language) != s.languages.end();
        if (wanted && !probed.empty() && path.starts_with(probed + "/")) {
            close_document();
            buf_ = &buf;
            name_document(path, language);
            open_document();
            return true;
        }
    }
    if (conn_ && ready_ && !path.empty() && !is_remote(path)) {
        std::string program;
        const LspServer* server = pick(servers, language, &program);
        char real[PATH_MAX];
        if (server && server->name == server_name_ && program == conn_->program && realpath(path.c_str(), real)) {
            const std::string full = real;
            if (find_root(full.substr(0, full.rfind('/')), server->roots) == root_) {
                close_document();
                buf_ = &buf;
                name_document(full, language);
                open_document();
                return true;
            }
        }
    }
#endif
    detach();
#if defined(_WIN32)
    (void)buf;
    (void)path;
    (void)language;
    (void)servers;
    return false;
#else
    std::string program, host, dir;
    const LspServer* server = nullptr;
    std::vector<LspServer> candidates;
    std::string full;
    if (split_remote(path, &host, &dir)) {  // another machine's: which server is installed there is found out there
        if (language.empty()) return false;
        for (const LspServer& s : servers)
            if (s.enabled && !s.command.empty() && std::ranges::find(s.languages, language) != s.languages.end()) candidates.push_back(s);
        if (candidates.empty()) return false;
        server = &candidates[0];
        program = server->command[0];
        full = path;
    } else {
        server = pick(servers, language, &program);
        if (!server || path.empty()) return false;
        char real[PATH_MAX];
        if (!realpath(path.c_str(), real)) return false;  // not on disk yet: nothing a server could use
        full = real;
    }

    auto conn = std::make_shared<Conn>();
    int wake[2];
    if (pipe(wake) != 0) return false;
    for (const int fd : wake) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    conn->wake_r = wake[0];
    conn->wake_w = wake[1];
    conn->server = *server;
    conn->program = program;
    if (!host.empty()) {
        conn->remote = "ssh://" + host;
        conn->candidates = candidates;
        conn->root = full.substr(0, full.rfind('/'));  // (until the worker has found the project's folder there)
    } else {
        conn->root = find_root(full.substr(0, full.rfind('/')), server->roots);
    }
    conn->path_env = search_path();
    conn->notify = notify_;
    // Writing to a server that has died must be an error, not the end of
    // this program.
    signal(SIGPIPE, SIG_IGN);

    buf_ = &buf;
    server_name_ = server->name;
    if (!host.empty()) server_name_ += "@" + host;  // (one that fails there may yet do here, and the other way round)
    root_ = conn->root;
    name_document(full, language);
    caps_ = Caps{};
    ready_ = opened_ = false;
    version_ = 0;
    conn_ = conn;
    std::thread(Conn::run, conn).detach();
    return true;
#endif
}

// The document's names, as the server is told them.
void Lsp::name_document(const std::string& full, const std::string& language) {
#if !defined(_WIN32)
    path_ = full;
    std::string host, there;
    uri_ = path_to_uri(split_remote(full, &host, &there) ? there : full);  // (another machine's: the path there)
    language_id_ = language_id(language);
    change_head_ = R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":)";
    json::quote(change_head_, uri_);
    change_head_ += R"(,"version":)";
    version_ = 0;
#else
    (void)full;
    (void)language;
#endif
}

// Tells the server the document is closed and forgets what it said about
// it. Answers still on their way are for questions nobody is asking any more.
void Lsp::close_document() {
#if !defined(_WIN32)
    if (conn_) {  // (the text still queued is of a buffer about to go: the worker is to let go of it, and is waited for if it is reading it)
        std::lock_guard lock(conn_->text_mu);
        conn_->epoch.fetch_add(1, std::memory_order_relaxed);
    }
    if (conn_ && opened_) {
        std::string m = R"({"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":)";
        json::quote(m, uri_);
        m += "}}}";
        send(std::move(m));
    }
#endif
    opened_ = false;
    buf_ = nullptr;
    path_.clear();  // what the server still says about that file is for nobody
    complete_id_ = hover_id_ = sig_id_ = def_id_ = 0;
    complete_first_edit_ = ~0ull;
    items_.clear();
    ++items_serial_;
    hover_lines_.clear();
    sig_ = SignatureInfo{};
    def_ = LspLocation{};
    if (!diags_.empty()) {
        diags_.clear();
        ++diags_gen_;
    }
}

void Lsp::release() {
    if (!conn_) return;
    if (!ready_) return detach();  // still starting: not worth keeping
    close_document();
}

void Lsp::detach() {
    if (!conn_) return;
    close_document();
#if !defined(_WIN32)
    {
        std::lock_guard lock(conn_->mu);
        conn_->stop = true;
    }
    conn_->wake();
#endif
    conn_.reset();
    ready_ = false;
}

void Lsp::send(std::string json, u32 id, u8 kind) {
#if !defined(_WIN32)
    bool was_empty;
    {
        std::lock_guard lock(conn_->mu);
        was_empty = conn_->out.empty();
        Conn::Out o;
        o.head = std::move(json);
        o.id = id;
        o.kind = kind;
        conn_->out.push_back(std::move(o));
    }
    if (was_empty) conn_->wake();  // otherwise the worker is already on its way
    ++sent_;
#else
    (void)json;
    (void)id;
    (void)kind;
#endif
}

// A message that carries the whole document. The text is handed over as a
// list of pieces and turned into JSON on the worker thread, so the size of
// the file costs the main thread nothing.
void Lsp::send_document(std::string head, std::string tail, bool replaces_unsent) {
#if !defined(_WIN32)
    auto pieces = std::make_shared<std::vector<Piece>>();
    buf_->snapshot(pieces.get());
    bool was_empty;
    {
        std::lock_guard lock(conn_->mu);
        was_empty = conn_->out.empty();
        Conn::Out o;
        o.head = std::move(head);
        o.tail = std::move(tail);
        o.text = std::move(pieces);
        o.whole = replaces_unsent;
        o.epoch = conn_->epoch.load(std::memory_order_relaxed);
        if (replaces_unsent && !conn_->out.empty() && conn_->out.back().whole) conn_->out.back() = std::move(o);
        else conn_->out.push_back(std::move(o));
    }
    if (was_empty) conn_->wake();
    ++sent_;
#else
    (void)head;
    (void)tail;
    (void)replaces_unsent;
#endif
}

void Lsp::open_document() {
    std::string head = R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":)";
    json::quote(head, uri_);
    head += R"(,"languageId":)";
    json::quote(head, language_id_);
    head += R"(,"version":)" + std::to_string(version_) + R"(,"text":)";
    send_document(std::move(head), "}}}", false);
    opened_ = true;
}

// Column of `off` within its line, the way the server counts.
u32 Lsp::character_of(u64 line_start, u64 off) const {
    if (!caps_.utf16) return static_cast<u32>(off - line_start);
    // One unit per character, two for those outside the first plane: every
    // byte that starts a character counts one, a four-byte starter one more.
    u32 units = 0;
    u64 left = off - line_start;
    for (Buffer::Reader rd = buf_->reader(line_start); left > 0;) {
        const std::string_view ch = rd.chunk();
        if (ch.empty()) break;
        const size_t n = static_cast<size_t>(std::min<u64>(left, ch.size()));
        const u8* p = reinterpret_cast<const u8*>(ch.data());
        for (size_t i = 0; i < n; ++i) units += static_cast<u32>((p[i] & 0xC0) != 0x80) + static_cast<u32>(p[i] >= 0xF0);
        rd.advance(static_cast<u32>(n));
        left -= n;
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

void Lsp::position_json(std::string& out, u64 off) const {
    const u64 line = buf_->line_of(off);
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, R"({"line":%llu,"character":%u})", static_cast<unsigned long long>(line),
                                character_of(buf_->line_start(line), off));
    out.append(buf, static_cast<size_t>(std::min<int>(n, sizeof buf - 1)));
}

void Lsp::before_edit(u64 off, u64 remove_len) {
    // In UTF-16 the column where the removed text ends has to be worked out
    // while that text is still there.
    if (!opened_ || !caps_.utf16 || caps_.sync != 2) return;
    const u64 end = off + remove_len;
    old_end_char_ = character_of(buf_->line_start(buf_->line_of(end)), end);
}

void Lsp::on_edit(const TextEdit& e) {
    if (!conn_) return;
    ++version_;
    complete_first_edit_ = std::min(complete_first_edit_, e.start);

    // Diagnostics stay on the text they were about until the server sends
    // new ones.
    if (!diags_.empty()) {
        const i64 delta = static_cast<i64>(e.new_end) - static_cast<i64>(e.old_end);
        auto moved = [&](u64 x, u64 inside) {
            if (x <= e.start) return x;
            if (x >= e.old_end) return static_cast<u64>(static_cast<i64>(x) + delta);
            return inside;
        };
        for (Diagnostic& d : diags_) {
            d.start = moved(d.start, e.start);
            d.end = std::max(d.start, moved(d.end, e.new_end));
        }
        ++diags_gen_;
    }

    if (!opened_ || caps_.sync == 0) return;
    char num[192];
    std::string m = change_head_;
    m += std::to_string(version_);
    if (caps_.sync == 1) {
        m += R"(},"contentChanges":[{"text":)";
        send_document(std::move(m), "}]}}", true);
        return;
    }
    const u32 c0 = caps_.utf16 ? character_of(buf_->line_start(e.start_row), e.start) : e.start_col;
    const u32 c1 = caps_.utf16 ? old_end_char_ : e.old_end_col;
    const int n = std::snprintf(num, sizeof num, R"(},"contentChanges":[{"range":{"start":{"line":%u,"character":%u},"end":{"line":%u,"character":%u}},"text":)",
                                e.start_row, c0, e.old_end_row, c1);
    m.append(num, static_cast<size_t>(std::min<int>(n, sizeof num - 1)));
    const u64 len = e.new_end - e.start;
    if (len > 256 * 1024) {
        // A big paste: cheaper to hand over the document than to copy the
        // pasted text here. (A change without a range means "all of it".)
        std::string whole = change_head_;
        whole += std::to_string(version_);
        whole += R"(},"contentChanges":[{"text":)";
        send_document(std::move(whole), "}]}}", false);
        return;
    }
    json::quote(m, buf_->text(e.start, len));
    m += "}]}}";
    send(std::move(m));
}

void Lsp::did_save() {
    if (!opened_ || !caps_.save) return;
    std::string m = R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":)";
    json::quote(m, uri_);
    m += "}}}";
    send(std::move(m));
}

u32 Lsp::ask(const char* method, u64 offset, u8 kind, std::string_view extra, u32* latest) {
    if (*latest) {  // the answer still under way is no longer wanted
        send(R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":)" + std::to_string(*latest) + "}}");
    }
    const u32 id = next_id_++;
    std::string m = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + method + R"(","params":{"textDocument":{"uri":)";
    json::quote(m, uri_);
    m += R"(},"position":)";
    position_json(m, offset);
    m += extra;
    m += "}}";
    send(std::move(m), id, kind);
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
    ask("textDocument/completion", offset, kInCompletion, context, &complete_id_);
    complete_at_ = offset;
    complete_first_edit_ = ~0ull;
}

void Lsp::hover(u64 offset) {
    if (!can_hover()) return;
    ask("textDocument/hover", offset, kInHover, {}, &hover_id_);
    hover_at_ = offset;
}

void Lsp::signature(u64 offset) {
    if (!can_signature()) return;
    ask("textDocument/signatureHelp", offset, kInSignature, {}, &sig_id_);
}

void Lsp::definition(u64 offset) {
    if (!can_define()) return;
    ask("textDocument/definition", offset, kInDefinition, {}, &def_id_);
}

u32 Lsp::poll() {
    if (!conn_) return 0;
    std::vector<Conn::In> in;
    {
        std::lock_guard lock(conn_->mu);
        in.swap(conn_->in);
    }
    u32 news = 0;
    for (Conn::In& m : in) {
        switch (m.kind) {
            case kInReady:
                caps_ = m.caps;
                ready_ = true;
                if (!m.text.empty()) server_name_ = m.text;
                open_document();
                news |= kState;
                break;
            case kInFailed:
            case kInStopped:
                message_ = "language server: " + m.text;
                opened_ = false;  // nobody left to say goodbye to
                detach();
                return news | kState | kDiagnostics;
            case kInDiagnostics: {
                if (!buf_ || path_.empty() || m.text != path_ || (m.version >= 0 && m.version != version_)) break;
                diags_.clear();
                diags_.reserve(m.diags.size());
                for (RawDiag& r : m.diags) {
                    Diagnostic d;
                    d.start = offset_at(*buf_, r.r.l0, r.r.c0);
                    d.end = std::max(d.start, offset_at(*buf_, r.r.l1, r.r.c1));
                    d.severity = r.severity;
                    d.message = std::move(r.message);
                    d.source = std::move(r.source);
                    d.code = std::move(r.code);
                    diags_.push_back(std::move(d));
                }
                std::stable_sort(diags_.begin(), diags_.end(), [](const Diagnostic& a, const Diagnostic& b) { return a.start < b.start; });
                ++diags_gen_;
                news |= kDiagnostics;
                break;
            }
            case kInCompletion: {
                if (m.id != complete_id_) break;
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
                for (RawItem& r : m.items) {
                    CompletionItem& it = r.item;
                    if (r.has_range) {
                        if (r.range.l0 != last_line || r.range.c0 != last_col) {
                            last_line = r.range.l0;
                            last_col = r.range.c0;
                            last_from = offset_at(*buf_, last_line, last_col);
                        }
                        if (last_from <= complete_first_edit_) it.replace_from = last_from;
                    }
                    for (RawItem::Extra& x : r.extra) {
                        const u64 a = offset_at(*buf_, x.r.l0, x.r.c0), b = offset_at(*buf_, x.r.l1, x.r.c1);
                        if (a <= b && b <= complete_first_edit_) it.extra.push_back({a, b, std::move(x.text)});
                    }
                    items_.push_back(std::move(it));
                }
                items_incomplete_ = m.incomplete;
                news |= kCompletion;
                break;
            }
            case kInHover:
                if (m.id != hover_id_) break;
                hover_id_ = 0;
                hover_lines_ = std::move(m.lines);
                news |= kHover;
                break;
            case kInSignature:
                if (m.id != sig_id_) break;
                sig_id_ = 0;
                sig_ = std::move(m.sig);
                news |= kSignature;
                break;
            case kInDefinition:
                if (m.id != def_id_) break;
                def_id_ = 0;
                def_ = std::move(m.loc);
                news |= kDefinition;
                break;
            default: break;
        }
    }
    return news;
}

std::string Lsp::take_message() {
    std::string m;
    m.swap(message_);
    return m;
}

bool Lsp::wait(int timeout_ms) {
    if (!conn_) return false;
    std::unique_lock lock(conn_->mu);
    return conn_->arrived.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !conn_->in.empty(); });
}

}  // namespace blit
