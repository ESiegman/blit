// A language server with predictable answers, for the tests and benchmarks.
//
//   blit_fakelsp [--utf8] [--full] [--slow MS] [--late MS] [--die-after N] [--items N] [--quiet]
//                [--pull] [--workspace] [--no-prepare] [--default-rename] [--changes] [--noise]
//                [--close-input MS] [--hints-once] [--index MS] [--name NAME]
//
// --utf8        agree to count columns in bytes (the default is UTF-16)
// --full        ask for the whole document on every change
// --slow MS     wait that long before every answer
// --late MS     wait that long before answering with edits (a rename, code
//               actions, an action resolved), reading nothing meanwhile
// --die-after N exit without a word after N messages
// --items N     pad completion lists to N items
// --quiet       send diagnostics when a document opens, but not on changes
// --pull        send no diagnostics: they are asked for (textDocument/diagnostic)
// --workspace   and the whole project's too (workspace/diagnostic): its folder's
//               files that are not open
// --no-prepare  rename without saying first what would be renamed
// --default-rename  say first only that the word there is what would be renamed
//               ({"defaultBehavior": true})
// --changes     workspace edits as "changes" (by file) rather than "documentChanges"
// --noise       print a line of its own on its output before every message (as
//               servers that log to stdout do)
// --close-input MS  once nothing has come for MS ms, close its input and keep
//               its output open for two seconds more, saying nothing
// --hints-once  answer inlay hints the first time, and with an error after
// --index MS    index the project as clangd does, saying so through $/progress:
//               begun ("indexing", "1/2") once initialized, ended MS ms later
// --name NAME   what diagnostics give as their source (default "fake")
//
// It keeps every document it is sent. What it does with one:
//   diagnostics  one for every ERROR, WARN, INFO and HINT in the text, plus a
//                hint at the very start whose message is "text:" and a hash
//                of the server's copy of the document, so a test can tell
//                whether the copy is right; a WARN says it has a fix
//   completion   a fixed vocabulary, replacing the word before the cursor
//   hover        a description of the word under the cursor
//   signature    one signature, for the call the cursor is in
//   definition   the first place the word appears
//   references   every place the word appears, in the open documents and in
//                the files of the project's folder that are not open
//   implementation  every place after the first, as LocationLinks
//   type definition the last place the word appears, as one Location (not a list)
//   declaration  the first place, as a list of one
//   highlight    every place the word appears in the document
//   symbols      a line starting "struct NAME" or "class NAME", with the
//                functions inside it as its children; a line "TYPE NAME(...) {"
//                is a function (the outline nested, the project's flat, those
//                whose name has what was asked in it, ignoring case, in the
//                open documents and the files of the project's folder that
//                are not open)
//   switch       clangd's other half: .cpp and .hpp swapped, src/ and
//                include/ too (null when the file is neither)
//   rename       those places, renamed (not ERROR, which cannot be)
//   formatting   blanks at the ends of lines go, and runs of blanks in them
//                become one
//   code actions WARN to NOTE (a fix, with an edit), the word in capitals
//                (resolved on asking), and a comment above the line (a
//                command, whose edit the server asks for with applyEdit)
//   inlay hints  "first:", "second:", "third:" before call's arguments, and
//                ": int" after a name that follows "auto "
//   tokens       Widget a type, a word starting p_ a parameter, call a
//                function, a word all in capitals (not the four above) a
//                macro; whole or as a change to the last list
#include <dirent.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

using namespace blit;
using json::Value;

namespace {

bool utf16 = true;
int sync_kind = 2;
int slow_ms = 0, late_ms = 0, close_input_ms = 0, index_ms = -1;
long long index_end = -1;  // when the indexing under way ends (ms of the steady clock; -1: none under way)
long die_after = -1;
long pad_items = 0;
bool quiet = false, pull = false, workspace = false, prepare = true, by_changes = false, noise = false, default_rename = false, hints_once = false;
long hints_answered = 0;
long handled = 0;
std::string source = "fake";

struct Document {
    std::string text;
    long version = 0;
    std::vector<unsigned> tokens;  // the last semantic tokens sent
    long tokens_id = 0;
};
std::map<std::string, Document> docs;
std::string root_path, config_value = "unset";
long next_result = 1;

// The document a request is about; text and uri point at it.
std::string text, uri;
void select(const std::string& u) {
    uri = u;
    text = docs[u].text;
}

bool read_message(std::string* body) {
    size_t length = 0;
    char line[256];
    for (;;) {
        if (!std::fgets(line, sizeof line, stdin)) return false;
        if (!std::strcmp(line, "\r\n")) break;
        if (!strncasecmp(line, "Content-Length:", 15)) length = std::strtoull(line + 15, nullptr, 10);
    }
    body->resize(length);
    return std::fread(body->data(), 1, length, stdin) == length;
}

// False once its input has been closed (--close-input), two seconds after.
bool still_reading() {
    if (close_input_ms <= 0) return true;
    pollfd p{0, POLLIN, 0};
    if (poll(&p, 1, close_input_ms) != 0) return true;
    close(0);
    usleep(2'000'000);
    return false;
}

void send(const std::string& body) {
    if (slow_ms > 0) usleep(static_cast<useconds_t>(slow_ms) * 1000);
    if (noise) std::printf("fake: a line that is not a message\n");
    std::printf("Content-Length: %zu\r\n\r\n", body.size());
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

long long now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1'000'000;
}

void progress(const std::string& value) {
    send(R"({"jsonrpc":"2.0","method":"$/progress","params":{"token":"backgroundIndexProgress","value":)" + value + "}}");
}

// The indexing under way (--index): ended once its time has come, a
// message being waited for no longer than that.
void index_tick() {
    while (index_end >= 0) {
        const long long left = index_end - now_ms();
        if (left <= 0) {
            progress(R"({"kind":"end"})");
            index_end = -1;
            break;
        }
        pollfd p{0, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left)) != 0) break;
    }
}

bool word_char(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || u >= 0x80;
}

// Byte offset of a (line, character) position.
size_t offset_of(const std::string& t, long line, long ch) {
    size_t at = 0;
    for (long l = 0; l < line; ++l) {
        const size_t nl = t.find('\n', at);
        if (nl == std::string::npos) return t.size();
        at = nl + 1;
    }
    long units = 0;
    while (at < t.size() && t[at] != '\n') {
        const unsigned char b = static_cast<unsigned char>(t[at]);
        if (utf16) {
            if ((b & 0xC0) != 0x80) {
                if (units >= ch) break;
                units += b >= 0xF0 ? 2 : 1;
            }
        } else {
            if (units >= ch) break;
            ++units;
        }
        ++at;
    }
    return at;
}

size_t offset_of(Value pos) { return offset_of(text, static_cast<long>(pos["line"].integer()), static_cast<long>(pos["character"].integer())); }

void position_of(const std::string& t, size_t off, long* line, long* ch) {
    *line = 0;
    *ch = 0;
    size_t start = 0;
    for (size_t i = 0; i < off && i < t.size(); ++i)
        if (t[i] == '\n') {
            ++*line;
            start = i + 1;
        }
    for (size_t i = start; i < off && i < t.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(t[i]);
        if (utf16) *ch += ((b & 0xC0) != 0x80) + (b >= 0xF0);
        else ++*ch;
    }
}

void position_json(std::string& out, size_t off, const std::string& t = text) {
    long line, ch;
    position_of(t, off, &line, &ch);
    out += "{\"line\":" + std::to_string(line) + ",\"character\":" + std::to_string(ch) + "}";
}

void range_json(std::string& out, size_t a, size_t b, const std::string& t = text) {
    out += "{\"start\":";
    position_json(out, a, t);
    out += ",\"end\":";
    position_json(out, b, t);
    out += "}";
}

std::string hash_of(const std::string& t) {
    unsigned long long hash = 1469598103934665603ull;
    for (const char c : t) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016llx", hash);
    return hex;
}

// The diagnostics of a text, as a JSON array.
std::string diagnostics_of(const std::string& t, const std::string& u) {
    std::string m = R"([{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"severity":4,"source":)";
    json::quote(m, source);
    m += R"(,"message":"text:)" + hash_of(t) + "\"}";
    static const struct {
        const char* word;
        int severity;
    } kinds[] = {{"ERROR", 1}, {"WARN", 2}, {"INFO", 3}, {"HINT", 4}};
    for (const auto& k : kinds) {
        const size_t n = std::strlen(k.word);
        for (size_t at = t.find(k.word); at != std::string::npos; at = t.find(k.word, at + n)) {
            m += ",{\"range\":";
            std::string range;
            range_json(range, at, at + n, t);
            m += range;
            m += ",\"severity\":" + std::to_string(k.severity) + R"(,"source":)";
            json::quote(m, source);
            m += R"(,"code":"F)" + std::to_string(k.severity) + R"(","message":"found )" + k.word + "\\nsecond line\"";
            if (k.severity == 2) {  // (as clangd says it has a fix, with codeActionsInline)
                m += R"(,"codeActions":[{"title":"Replace WARN with NOTE","kind":"quickfix","edit":{"changes":{)";
                json::quote(m, u);
                m += R"(:[{"range":)" + range + R"(,"newText":"NOTE"}]}}}])";
            }
            m += "}";
        }
    }
    m += "]";
    return m;
}

void publish(const std::string& u) {
    if (pull) return;
    const Document& d = docs[u];
    std::string m = R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":)";
    json::quote(m, u);
    m += ",\"version\":" + std::to_string(d.version) + ",\"diagnostics\":" + diagnostics_of(d.text, u) + "}}";
    send(m);
}

std::string word_at(size_t off, size_t* start = nullptr) {
    size_t a = off, b = off;
    while (a > 0 && word_char(text[a - 1])) --a;
    while (b < text.size() && word_char(text[b])) ++b;
    if (start) *start = a;
    return text.substr(a, b - a);
}

void reply(Value id, const std::string& result) {
    std::string m = R"({"jsonrpc":"2.0","id":)";
    json::dump(m, id);
    m += ",\"result\":" + result + "}";
    send(m);
}

void reply_error(Value id, const std::string& message) {
    std::string m = R"({"jsonrpc":"2.0","id":)";
    json::dump(m, id);
    m += R"(,"error":{"code":-32602,"message":)";
    json::quote(m, message);
    m += "}}";
    send(m);
}

std::string path_of(const std::string& u) { return u.starts_with("file://") ? u.substr(7) : u; }

// The files in the project's folder (one level) that are not open, with their text.
std::vector<std::pair<std::string, std::string>> closed_files() {
    std::vector<std::pair<std::string, std::string>> out;
    if (root_path.empty()) return out;
    DIR* dir = opendir(root_path.c_str());
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        const std::string path = root_path + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (1 << 20)) continue;
        const std::string u = "file://" + path;
        if (docs.count(u)) continue;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::string t(static_cast<size_t>(st.st_size), '\0');
        t.resize(std::fread(t.data(), 1, t.size(), f));
        std::fclose(f);
        out.push_back({u, std::move(t)});
    }
    closedir(dir);
    std::sort(out.begin(), out.end());
    return out;
}

// Every place a whole word appears in a text.
std::vector<size_t> places_of(const std::string& t, const std::string& w) {
    std::vector<size_t> out;
    if (w.empty()) return out;
    for (size_t at = t.find(w); at != std::string::npos; at = t.find(w, at + 1)) {
        const bool whole = (at == 0 || !word_char(t[at - 1])) && (at + w.size() >= t.size() || !word_char(t[at + w.size()]));
        if (whole) out.push_back(at);
    }
    return out;
}

// The word's places everywhere: the document asked about first, then the
// other open ones, then the files that are not open.
std::vector<std::pair<std::string, std::vector<std::pair<size_t, std::string>>>> everywhere(const std::string& w) {
    std::vector<std::pair<std::string, std::vector<std::pair<size_t, std::string>>>> out;
    auto add = [&](const std::string& u, const std::string& t) {
        const std::vector<size_t> at = places_of(t, w);
        if (at.empty()) return;
        out.push_back({u, {}});
        for (size_t a : at) out.back().second.push_back({a, t});
    };
    add(uri, text);
    for (const auto& [u, d] : docs)
        if (u != uri) add(u, d.text);
    for (const auto& [u, t] : closed_files()) add(u, t);
    return out;
}

void completion(Value id, Value params) {
    const size_t off = offset_of(params["position"]);
    size_t start = off;
    while (start > 0 && word_char(text[start - 1])) --start;
    static const struct {
        const char* label;
        int kind;
        const char* detail;
    } words[] = {{"alpha", 6, "int"},          {"alphabet", 6, "std::string"}, {"alpine", 21, "const int"},
                 {"beta", 3, "void beta()"},   {"betaMax", 3, "int betaMax(int)"}, {"delta", 5, "double"},
                 {"gamma_ray", 7, "class"},    {"getValue", 2, "int getValue() const"}, {"get_width", 2, "int get_width()"},
                 {"setValue", 2, "void setValue(int)"}, {"vector", 7, "std::vector"}, {"for_loop", 15, "snippet"},
                 {"r\xC3\xA9sum\xC3\xA9", 6, "text"}};
    std::string r = "{\"isIncomplete\":false,\"items\":[";
    std::string range;  // the same for every item: the word before the cursor
    range_json(range, start, off);
    bool first = true;
    auto item = [&](const std::string& label, int kind, const std::string& detail, int order) {
        if (!first) r += ',';
        first = false;
        r += "{\"label\":";
        json::quote(r, label);
        r += ",\"kind\":" + std::to_string(kind) + ",\"detail\":";
        json::quote(r, detail);
        char sort[16];
        std::snprintf(sort, sizeof sort, "%05d", order);
        r += ",\"sortText\":\"" + std::string(sort) + "\"";
        r += ",\"documentation\":{\"kind\":\"markdown\",\"value\":\"About **" + label + "**.\"}";
        if (label == "for_loop") {
            r += R"(,"insertTextFormat":2,"insertText":"for (${1:i} = 0; ${1:i} < ${2:n}; ++${1:i}) {$0}")";
        } else {
            r += ",\"textEdit\":{\"range\":" + range + ",\"newText\":";
            json::quote(r, label);
            r += "}";
        }
        if (label == "vector")
            r += R"(,"additionalTextEdits":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"newText":"#include <vector>\n"}])";
        r += "}";
    };
    int order = 0;
    // Sent backwards: the client has to put them in sortText order.
    for (size_t i = std::size(words); i-- > 0;) item(words[i].label, words[i].kind, words[i].detail, static_cast<int>(i));
    order = static_cast<int>(std::size(words));
    for (long i = 0; i + static_cast<long>(std::size(words)) < pad_items; ++i)
        item("item_" + std::to_string(i) + (i % 3 ? "_value" : "Name"), 6, "int", order++);
    r += "]}";
    reply(id, r);
}

void hover(Value id, Value params) {
    const size_t off = offset_of(params["position"]);
    const std::string w = word_at(off);
    if (w.empty()) return reply(id, "null");
    std::string md = "```cpp\nint " + w + "\n```\n---\nAbout **" + w + "**: see [the docs](http://example.com).";
    if (w == "config") md += "\n\nconfig=" + config_value;
    std::string r = "{\"contents\":{\"kind\":\"markdown\",\"value\":";
    json::quote(r, md);
    r += "}}";
    reply(id, r);
}

void signature(Value id, Value params) {
    const size_t off = offset_of(params["position"]);
    size_t open = off;
    while (open > 0 && text[open - 1] != '(' && text[open - 1] != '\n' && text[open - 1] != ')') --open;
    if (open == 0 || text[open - 1] != '(') return reply(id, "null");
    long commas = 0;
    for (size_t i = open; i < off; ++i) commas += text[i] == ',';
    // The first two parameters are given by position, the third by its text.
    reply(id, R"x({"signatures":[{"label":"void call(int first, const char* second, double third)",)x"
              R"x("documentation":"Calls it.",)x"
              R"x("parameters":[{"label":[10,19]},{"label":[21,39]},{"label":"double third"}]}],)x"
              R"x("activeSignature":0,"activeParameter":)x" +
                  std::to_string(commas) + "}");
}

void definition(Value id, Value params) {
    const size_t off = offset_of(params["position"]);
    const std::vector<size_t> at = places_of(text, word_at(off));
    if (at.empty()) return reply(id, "null");
    std::string r = "[{\"uri\":";
    json::quote(r, uri);
    r += ",\"range\":";
    range_json(r, at[0], at[0] + word_at(off).size());
    r += "}]";
    reply(id, r);
}

void references(Value id, Value params) {
    const std::string w = word_at(offset_of(params["position"]));
    std::string r = "[";
    bool first = true;
    for (const auto& [u, places] : everywhere(w))
        for (const auto& [at, t] : places) {
            if (!first) r += ',';
            first = false;
            r += "{\"uri\":";
            json::quote(r, u);
            r += ",\"range\":";
            range_json(r, at, at + w.size(), t);
            r += "}";
        }
    r += "]";
    reply(id, r);
}

// Implementations, type definitions and declarations, each in another of
// the shapes a location answer can have.
void places(Value id, Value params, std::string_view method) {
    const std::string w = word_at(offset_of(params["position"]));
    const std::vector<size_t> at = places_of(text, w);
    if (at.empty()) return reply(id, "null");
    std::string r;
    auto location = [&](size_t a, bool link) {
        r += link ? "{\"targetUri\":" : "{\"uri\":";
        json::quote(r, uri);
        if (link) {
            r += ",\"targetRange\":";
            range_json(r, a, a + w.size());
            r += ",\"targetSelectionRange\":";
        } else {
            r += ",\"range\":";
        }
        range_json(r, a, a + w.size());
        r += "}";
    };
    if (method == "textDocument/typeDefinition") {
        location(at.back(), false);
    } else if (method == "textDocument/declaration") {
        r = "[";
        location(at.front(), false);
        r += "]";
    } else {
        r = "[";
        for (size_t i = 1; i < at.size(); ++i) {
            if (i > 1) r += ',';
            location(at[i], true);
        }
        r += "]";
    }
    reply(id, r);
}

void highlight(Value id, Value params) {
    const std::string w = word_at(offset_of(params["position"]));
    std::string r = "[";
    bool first = true;
    for (const size_t a : places_of(text, w)) {
        if (!first) r += ',';
        first = false;
        r += "{\"range\":";
        range_json(r, a, a + w.size());
        r += ",\"kind\":1}";
    }
    r += "]";
    reply(id, r);
}

// A document's symbols: structs and classes (5, 23) with the functions
// (6 inside them, 12 outside) whose lines are indented under them.
struct FakeSymbol {
    std::string name, container;
    int kind = 12;
    size_t at = 0;
    std::vector<FakeSymbol> kids;
};
std::vector<FakeSymbol> symbols_of(const std::string& t) {
    std::vector<FakeSymbol> out;
    size_t ls = 0;
    while (ls < t.size()) {
        size_t le = t.find('\n', ls);
        if (le == std::string::npos) le = t.size();
        const std::string line = t.substr(ls, le - ls);
        size_t ind = 0;
        while (ind < line.size() && (line[ind] == ' ' || line[ind] == '\t')) ++ind;
        const std::string body = line.substr(ind);
        auto word_after = [&](size_t p) {
            size_t e = p;
            while (e < body.size() && (std::isalnum(static_cast<unsigned char>(body[e])) || body[e] == '_')) ++e;
            return body.substr(p, e - p);
        };
        if (body.rfind("struct ", 0) == 0 || body.rfind("class ", 0) == 0) {
            const size_t p = body.find(' ') + 1;
            FakeSymbol s;
            s.name = word_after(p);
            s.kind = body[0] == 's' ? 23 : 5;
            s.at = ls + ind + p;
            if (!s.name.empty()) out.push_back(s);
        } else if (const size_t open = body.find('('); open != std::string::npos && open > 0 && !body.empty() && body.back() == '{') {
            size_t b = open;
            while (b > 0 && (std::isalnum(static_cast<unsigned char>(body[b - 1])) || body[b - 1] == '_')) --b;
            const std::string name = body.substr(b, open - b);
            if (!name.empty() && b > 0 && body.find(' ') < b) {  // (a type before the name: not a call)
                FakeSymbol f;
                f.name = name;
                f.at = ls + ind + b;
                if (ind > 0 && !out.empty() && out.back().kind != 12) {
                    f.kind = 6;
                    f.container = out.back().name;
                    out.back().kids.push_back(f);
                } else {
                    out.push_back(f);
                }
            }
        }
        ls = le + 1;
    }
    return out;
}

void outline(Value id) {
    std::string r = "[";
    auto add = [&](auto&& self, const FakeSymbol& s, bool first) -> void {
        if (!first) r += ',';
        r += "{\"name\":";
        json::quote(r, s.name);
        r += ",\"detail\":\"" + std::string(s.kind == 12 || s.kind == 6 ? "()" : "") + "\",\"kind\":" + std::to_string(s.kind) + ",\"range\":";
        range_json(r, s.at, s.at + s.name.size());
        r += ",\"selectionRange\":";
        range_json(r, s.at, s.at + s.name.size());
        r += ",\"children\":[";
        for (size_t i = 0; i < s.kids.size(); ++i) self(self, s.kids[i], i == 0);
        r += "]}";
    };
    const std::vector<FakeSymbol> all = symbols_of(text);
    for (size_t i = 0; i < all.size(); ++i) add(add, all[i], i == 0);
    r += "]";
    reply(id, r);
}

void workspace_symbols(Value id, Value params) {
    std::string q(params["query"].str());
    for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string r = "[";
    bool first = true;
    auto file = [&](const std::string& u, const std::string& t) {
        auto add = [&](const FakeSymbol& s) {
            std::string low = s.name;
            for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low.find(q) == std::string::npos) return;
            if (!first) r += ',';
            first = false;
            r += "{\"name\":";
            json::quote(r, s.name);
            r += ",\"kind\":" + std::to_string(s.kind);
            if (!s.container.empty()) {
                r += ",\"containerName\":";
                json::quote(r, s.container);
            }
            r += ",\"location\":{\"uri\":";
            json::quote(r, u);
            r += ",\"range\":";
            range_json(r, s.at, s.at + s.name.size(), t);
            r += "}}";
        };
        for (const FakeSymbol& s : symbols_of(t)) {
            add(s);
            for (const FakeSymbol& k : s.kids) add(k);
        }
    };
    for (const auto& [u, d] : docs) file(u, d.text);
    for (const auto& [u, t] : closed_files()) file(u, t);
    r += "]";
    reply(id, r);
}

void switch_header(Value id, Value params) {
    std::string u(params["uri"].str());
    auto swap = [&](std::string_view from, std::string_view to) {
        const size_t at = u.rfind(from);
        if (at == std::string::npos) return false;
        u.replace(at, from.size(), to);
        return true;
    };
    if (u.ends_with(".cpp")) {
        swap("/src/", "/include/");
        u.replace(u.size() - 4, 4, ".hpp");
    } else if (u.ends_with(".hpp")) {
        swap("/include/", "/src/");
        u.replace(u.size() - 4, 4, ".cpp");
    } else {
        return reply(id, "null");
    }
    std::string r;
    json::quote(r, u);
    reply(id, r);
}

void prepare_rename(Value id, Value params) {
    size_t start = 0;
    const std::string w = word_at(offset_of(params["position"]), &start);
    if (w.empty()) return reply(id, "null");
    if (w == "ERROR") return reply_error(id, "ERROR cannot be renamed");
    if (default_rename) return reply(id, R"({"defaultBehavior":true})");
    std::string r = "{\"range\":";
    range_json(r, start, start + w.size());
    r += ",\"placeholder\":";
    json::quote(r, w);
    r += "}";
    reply(id, r);
}

void rename(Value id, Value params) {
    const std::string w = word_at(offset_of(params["position"]));
    if (w.empty() || w == "ERROR") return reply_error(id, "nothing to rename here");
    const std::string to(params["newName"].str());
    std::string r = by_changes ? "{\"changes\":{" : "{\"documentChanges\":[";
    bool first = true;
    for (const auto& [u, places] : everywhere(w)) {
        if (!first) r += ',';
        first = false;
        if (by_changes) {
            json::quote(r, u);
            r += ":[";
        } else {
            r += "{\"textDocument\":{\"uri\":";
            json::quote(r, u);
            r += ",\"version\":null},\"edits\":[";
        }
        bool first_edit = true;
        for (const auto& [at, t] : places) {
            if (!first_edit) r += ',';
            first_edit = false;
            r += "{\"range\":";
            range_json(r, at, at + w.size(), t);
            r += ",\"newText\":";
            json::quote(r, to);
            r += "}";
        }
        r += by_changes ? "]" : "]}";
    }
    r += by_changes ? "}}" : "]}";
    reply(id, r);
}

// Lines whose ends lose their blanks and whose runs of blanks become one
// (all of them, or those from line l0 to l1).
void formatting(Value id, Value params, bool range) {
    long l0 = 0, l1 = 1L << 30;
    if (range) {
        l0 = static_cast<long>(params["range"]["start"]["line"].integer());
        l1 = static_cast<long>(params["range"]["end"]["line"].integer());
        if (params["range"]["end"]["character"].integer() == 0 && l1 > l0) --l1;
    }
    std::string r = "[";
    bool first = true;
    size_t at = 0;
    for (long line = 0; at <= text.size(); ++line) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (line >= l0 && line <= l1) {
            const std::string was = text.substr(at, end - at);
            std::string now;
            size_t lead = 0;
            while (lead < was.size() && (was[lead] == ' ' || was[lead] == '\t')) ++lead;
            now = was.substr(0, lead);
            for (size_t i = lead; i < was.size(); ++i) {
                const bool blank = was[i] == ' ' || was[i] == '\t';
                if (blank && (now.size() > lead && (now.back() == ' '))) continue;
                now += blank ? ' ' : was[i];
            }
            while (now.size() > lead && now.back() == ' ') now.pop_back();
            if (lead == was.size()) now.clear();  // (a line of blanks alone: they are all at its end)
            if (now != was) {
                if (!first) r += ',';
                first = false;
                r += "{\"range\":";
                range_json(r, at, end);
                r += ",\"newText\":";
                json::quote(r, now);
                r += "}";
            }
        }
        if (end == text.size()) break;
        at = end + 1;
    }
    r += "]";
    reply(id, r);
}

void code_actions(Value id, Value params) {
    const size_t a = offset_of(params["range"]["start"]);
    size_t start = 0;
    const std::string w = word_at(a, &start);
    std::string r = "[";
    bool first = true;
    for (Value d : params["context"]["diagnostics"]) {
        if (d["message"].str() != "found WARN\nsecond line") continue;
        if (!first) r += ',';
        first = false;
        r += R"({"title":"Replace WARN with NOTE","kind":"quickfix","isPreferred":true,"diagnostics":[)";
        json::dump(r, d);
        r += R"(],"edit":{"changes":{)";
        json::quote(r, uri);
        r += R"(:[{"range":)";
        json::dump(r, d["range"]);
        r += R"(,"newText":"NOTE"}]}}})";
    }
    if (!w.empty()) {
        if (!first) r += ',';
        first = false;
        r += R"({"title":"Make it capitals","kind":"refactor.rewrite","data":{"uri":)";
        json::quote(r, uri);
        r += R"(,"start":)" + std::to_string(start) + R"(,"word":)";
        json::quote(r, w);
        r += "}}";
    }
    if (!first) r += ',';
    long line, ch;
    position_of(text, a, &line, &ch);
    r += R"({"title":"Add a comment above","kind":"refactor","command":{"title":"Add a comment above","command":"fake.comment","arguments":[)";
    json::quote(r, uri);
    r += "," + std::to_string(line) + "]}}";
    r += R"(,{"title":"Not now","kind":"refactor","disabled":{"reason":"it never is"}})";
    r += "]";
    reply(id, r);
}

void resolve(Value id, Value action) {
    const Value data = action["data"];
    select(std::string(data["uri"].str()));
    const size_t start = static_cast<size_t>(data["start"].integer());
    std::string word(data["word"].str()), upper = word;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string r = R"({"title":"Make it capitals","kind":"refactor.rewrite","edit":{"changes":{)";
    json::quote(r, uri);
    r += R"(:[{"range":)";
    range_json(r, start, start + word.size());
    r += ",\"newText\":";
    json::quote(r, upper);
    r += "}]}}}";
    reply(id, r);
}

// The command asks the editor for its edit, and answers once the editor has.
void execute(Value id, Value params) {
    if (params["command"].str() != "fake.comment") return reply_error(id, "no such command");
    const std::string u(params["arguments"].at(0).str());
    const long line = static_cast<long>(params["arguments"].at(1).integer());
    std::string m = R"({"jsonrpc":"2.0","id":9100,"method":"workspace/applyEdit","params":{"label":"Add a comment above","edit":{"changes":{)";
    json::quote(m, u);
    m += R"(:[{"range":{"start":{"line":)" + std::to_string(line) + R"(,"character":0},"end":{"line":)" + std::to_string(line) +
         R"(,"character":0}},"newText":"// fake\n"}]}}}})";
    send(m);
    reply(id, "null");
}

void inlay_hints(Value id, Value params) {
    const size_t a = offset_of(params["range"]["start"]), b = offset_of(params["range"]["end"]);
    std::string r = "[";
    bool first = true;
    auto hint = [&](size_t at, const std::string& label, int kind, bool left, bool right) {
        if (!first) r += ',';
        first = false;
        r += "{\"position\":";
        position_json(r, at);
        r += ",\"label\":";
        json::quote(r, label);
        r += ",\"kind\":" + std::to_string(kind) + (left ? ",\"paddingLeft\":true" : "") + (right ? ",\"paddingRight\":true" : "") + "}";
    };
    for (size_t at = text.find("call(", a); at != std::string::npos && at < b; at = text.find("call(", at + 1)) {
        static const char* const names[] = {"first:", "second:", "third:"};
        size_t i = at + 5;
        for (int arg = 0; arg < 3 && i < text.size() && text[i] != ')' && text[i] != '\n'; ++arg) {
            while (i < text.size() && text[i] == ' ') ++i;
            hint(i, names[arg], 2, false, true);
            while (i < text.size() && text[i] != ',' && text[i] != ')' && text[i] != '\n') ++i;
            if (i < text.size() && text[i] == ',') ++i;
        }
    }
    for (size_t at = text.find("auto ", a); at != std::string::npos && at < b; at = text.find("auto ", at + 1)) {
        size_t e = at + 5;
        while (e < text.size() && word_char(text[e])) ++e;
        if (e > at + 5) hint(e, ": int", 1, false, false);
    }
    r += "]";
    reply(id, r);
}

// Semantic tokens: the legend is type, parameter, function, macro; the one
// modifier, readonly, is on the macros.
std::vector<unsigned> tokens_of(const std::string& t) {
    std::vector<unsigned> out;
    long last_line = 0, last_ch = 0;
    // (Where the walk is, as a line and column, so that a big text is gone
    // through once rather than from its start for every token.)
    size_t at = 0;
    long at_line = 0, at_ch = 0;
    auto position = [&](size_t off, long* line, long* ch) {
        for (; at < off && at < t.size(); ++at) {
            const unsigned char b = static_cast<unsigned char>(t[at]);
            if (b == '\n') ++at_line, at_ch = 0;
            else if (utf16) at_ch += ((b & 0xC0) != 0x80) + (b >= 0xF0);
            else ++at_ch;
        }
        *line = at_line;
        *ch = at_ch;
    };
    for (size_t i = 0; i < t.size();) {
        if (!word_char(t[i]) || (i > 0 && word_char(t[i - 1]))) {
            ++i;
            continue;
        }
        size_t e = i;
        while (e < t.size() && word_char(t[e])) ++e;
        const std::string w = t.substr(i, e - i);
        int type = -1;
        bool caps = w.size() >= 2;
        for (const char c : w) caps = caps && ((c >= 'A' && c <= 'Z') || c == '_');
        if (w == "Widget") type = 0;
        else if (w.starts_with("p_")) type = 1;
        else if (w == "call") type = 2;
        else if (caps && w != "ERROR" && w != "WARN" && w != "INFO" && w != "HINT") type = 3;
        if (type >= 0) {
            long line, ch, line_e, ch_e;
            position(i, &line, &ch);
            position(e, &line_e, &ch_e);
            out.push_back(static_cast<unsigned>(line - last_line));
            out.push_back(static_cast<unsigned>(line == last_line ? ch - last_ch : ch));
            out.push_back(static_cast<unsigned>(ch_e - ch));
            out.push_back(static_cast<unsigned>(type));
            out.push_back(type == 3 ? 1u : 0u);
            last_line = line;
            last_ch = ch;
        }
        i = e;
    }
    return out;
}

void tokens(Value id, Value params, bool delta) {
    Document& d = docs[uri];
    const std::vector<unsigned> now = tokens_of(text);
    const long result = next_result++;
    std::string r = "{\"resultId\":\"" + std::to_string(result) + "\",";
    auto list = [&](const std::vector<unsigned>& v, size_t from, size_t to) {
        std::string s = "[";
        for (size_t i = from; i < to; ++i) s += (i > from ? "," : "") + std::to_string(v[i]);
        return s + "]";
    };
    if (delta && params["previousResultId"].str() == std::to_string(d.tokens_id)) {
        // The change: what differs between the first and the last numbers that differ.
        size_t lead = 0;
        while (lead < now.size() && lead < d.tokens.size() && now[lead] == d.tokens[lead]) ++lead;
        size_t tail = 0;
        while (tail < now.size() - lead && tail < d.tokens.size() - lead && now[now.size() - 1 - tail] == d.tokens[d.tokens.size() - 1 - tail]) ++tail;
        r += "\"edits\":[{\"start\":" + std::to_string(lead) + ",\"deleteCount\":" + std::to_string(d.tokens.size() - lead - tail) +
             ",\"data\":" + list(now, lead, now.size() - tail) + "}]}";
    } else {
        r += "\"data\":" + list(now, 0, now.size()) + "}";
    }
    d.tokens = now;
    d.tokens_id = result;
    reply(id, r);
}

void pull_document(Value id, Value params) {
    const std::string u(params["textDocument"]["uri"].str());
    reply(id, R"({"kind":"full","items":)" + diagnostics_of(docs[u].text, u) + "}");
}

void pull_workspace(Value id) {
    std::string r = "{\"items\":[";
    bool first = true;
    for (const auto& [u, t] : closed_files()) {
        if (t.find("ERROR") == std::string::npos && t.find("WARN") == std::string::npos) continue;
        if (!first) r += ',';
        first = false;
        r += "{\"uri\":";
        json::quote(r, u);
        r += R"(,"version":null,"kind":"full","items":)" + diagnostics_of(t, u) + "}";
    }
    r += "]}";
    reply(id, r);
}

}  // namespace

// Call and type hierarchies, in the document: a function's calls are the
// names of functions followed by "(" in its lines (up to the next one); a
// type's supertype is what follows ":" in "struct B : A {". Each item
// says its name again in "data", read back from what blit sends (which
// is the item as it was given).
struct FakeFn {
    std::string name;
    size_t at = 0, end = 0;  // its name, and where its lines end
};
std::vector<FakeFn> functions_of(const std::string& t) {
    std::vector<FakeFn> out;
    auto add = [&](const FakeSymbol& f) { out.push_back({f.name, f.at, t.size()}); };
    for (const FakeSymbol& s : symbols_of(t)) {
        if (s.kind == 12) add(s);
        for (const FakeSymbol& k : s.kids) add(k);
    }
    std::sort(out.begin(), out.end(), [](const FakeFn& a, const FakeFn& b) { return a.at < b.at; });
    for (size_t i = 0; i + 1 < out.size(); ++i) out[i].end = out[i + 1].at;
    return out;
}

void hier_item(std::string& r, const std::string& name, int kind, size_t at, const char* key) {
    r += "{\"name\":";
    json::quote(r, name);
    r += ",\"kind\":" + std::to_string(kind) + ",\"detail\":\"" + (kind == 12 ? "()" : "") + "\",\"uri\":";
    json::quote(r, uri);
    r += ",\"range\":";
    range_json(r, at, at + name.size());
    r += ",\"selectionRange\":";
    range_json(r, at, at + name.size());
    r += ",\"data\":{\"" + std::string(key) + "\":";
    json::quote(r, name);
    r += "}}";
}

// The calls in a function's lines (after its name): each callee and where.
std::vector<std::pair<std::string, size_t>> calls_in(const FakeFn& f, const std::vector<FakeFn>& all) {
    std::vector<std::pair<std::string, size_t>> out;
    const size_t from = text.find('\n', f.at);
    for (size_t i = from == std::string::npos ? f.end : from; i < f.end && i < text.size(); ++i) {
        if (!word_char(text[i]) || (i > 0 && word_char(text[i - 1]))) continue;
        size_t e = i;
        while (e < text.size() && word_char(text[e])) ++e;
        if (e < text.size() && text[e] == '(') {
            const std::string w = text.substr(i, e - i);
            for (const FakeFn& g : all)
                if (g.name == w) out.emplace_back(w, i);
        }
        i = e;
    }
    return out;
}

void prepare_calls(Value id, Value params) {
    const std::string w = word_at(offset_of(params["position"]));
    for (const FakeFn& f : functions_of(text))
        if (f.name == w) {
            std::string r = "[";
            hier_item(r, f.name, 12, f.at, "fn");
            return reply(id, r + "]");
        }
    reply(id, "null");
}

void calls(Value id, Value params, bool incoming) {
    const std::string name(params["item"]["data"]["fn"].str());
    const std::vector<FakeFn> all = functions_of(text);
    std::string r = "[";
    bool first = true;
    auto item_of = [&](const std::string& n) {
        for (const FakeFn& g : all)
            if (g.name == n) return g;
        return FakeFn{};
    };
    for (const FakeFn& f : all) {
        if (incoming ? false : f.name != name) continue;
        // Coming in: each function that calls it, with where; going out: each it calls.
        std::vector<std::pair<std::string, size_t>> found = calls_in(f, all);
        if (!incoming) {
            for (const auto& [callee, at] : found) {
                r += first ? "{\"to\":" : ",{\"to\":";
                first = false;
                hier_item(r, callee, 12, item_of(callee).at, "fn");
                r += ",\"fromRanges\":[";
                range_json(r, at, at + callee.size());
                r += "]}";
            }
        }
    }
    if (incoming)
        for (const FakeFn& f : all) {
            std::string ranges;
            for (const auto& [callee, at] : calls_in(f, all))
                if (callee == name) {
                    if (!ranges.empty()) ranges += ',';
                    range_json(ranges, at, at + callee.size());
                }
            if (ranges.empty()) continue;
            r += first ? "{\"from\":" : ",{\"from\":";
            first = false;
            hier_item(r, f.name, 12, f.at, "fn");
            r += ",\"fromRanges\":[" + ranges + "]}";
        }
    reply(id, r + "]");
}

// Each struct or class, where its name is, and its supertype ("" none).
std::vector<std::tuple<std::string, size_t, std::string>> types_of(const std::string& t) {
    std::vector<std::tuple<std::string, size_t, std::string>> out;
    for (size_t ls = 0; ls < t.size();) {
        size_t le = t.find('\n', ls);
        if (le == std::string::npos) le = t.size();
        const std::string line = t.substr(ls, le - ls);
        for (const char* kw : {"struct ", "class "})
            if (line.rfind(kw, 0) == 0) {
                const size_t p = std::strlen(kw);
                size_t e = p;
                while (e < line.size() && word_char(line[e])) ++e;
                std::string base;
                if (const size_t colon = line.find(':', e); colon != std::string::npos) {
                    size_t b = colon + 1;
                    while (b < line.size() && line[b] == ' ') ++b;
                    if (line.compare(b, 7, "public ") == 0) b += 7;
                    size_t be = b;
                    while (be < line.size() && word_char(line[be])) ++be;
                    base = line.substr(b, be - b);
                }
                out.emplace_back(line.substr(p, e - p), ls + p, base);
            }
        ls = le + 1;
    }
    return out;
}

void prepare_types(Value id, Value params) {
    const std::string w = word_at(offset_of(params["position"]));
    for (const auto& [name, at, base] : types_of(text))
        if (name == w) {
            std::string r = "[";
            hier_item(r, name, 23, at, "type");
            return reply(id, r + "]");
        }
    reply(id, "null");
}

void types(Value id, Value params, bool super) {
    const std::string name(params["item"]["data"]["type"].str());
    const auto all = types_of(text);
    std::string r = "[";
    bool first = true;
    for (const auto& [n, at, base] : all) {
        const bool wanted = super ? false : base == name;
        bool is_base = false;
        if (super)
            for (const auto& [m, mat, mbase] : all) is_base = is_base || (m == name && mbase == n);
        if (!wanted && !is_base) continue;
        if (!first) r += ',';
        first = false;
        hier_item(r, n, 23, at, "type");
    }
    reply(id, r + "]");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--utf8") utf16 = false;
        else if (a == "--full") sync_kind = 1;
        else if (a == "--slow" && i + 1 < argc) slow_ms = std::atoi(argv[++i]);
        else if (a == "--late" && i + 1 < argc) late_ms = std::atoi(argv[++i]);
        else if (a == "--die-after" && i + 1 < argc) die_after = std::atol(argv[++i]);
        else if (a == "--items" && i + 1 < argc) pad_items = std::atol(argv[++i]);
        else if (a == "--quiet") quiet = true;
        else if (a == "--pull") pull = true;
        else if (a == "--workspace") pull = workspace = true;
        else if (a == "--no-prepare") prepare = false;
        else if (a == "--changes") by_changes = true;
        else if (a == "--noise") noise = true;
        else if (a == "--default-rename") default_rename = true;
        else if (a == "--close-input" && i + 1 < argc) close_input_ms = std::atoi(argv[++i]);
        else if (a == "--hints-once") hints_once = true;
        else if (a == "--index" && i + 1 < argc) index_ms = std::atoi(argv[++i]);
        else if (a == "--name" && i + 1 < argc) source = argv[++i];
    }
    if (close_input_ms > 0 || index_ms >= 0) setvbuf(stdin, nullptr, _IONBF, 0);  // (what has come is all read: poll() says when more has)
    std::string body;
    while ((index_tick(), still_reading()) && read_message(&body)) {
        if (die_after >= 0 && handled++ >= die_after) return 3;
        json::Doc doc;
        if (!doc.parse(body)) continue;
        const Value v = doc.root();
        const std::string_view method = v["method"].str();
        const Value id = v["id"], params = v["params"];
        if (params["textDocument"]["uri"].is_string() && method != "textDocument/didOpen") select(std::string(params["textDocument"]["uri"].str()));
        if (method == "initialize") {
            // Honour the encoding only if the client offered it.
            bool offered_utf8 = false;
            for (Value e : params["capabilities"]["general"]["positionEncodings"]) offered_utf8 |= e.str() == "utf-8";
            if (!offered_utf8) utf16 = true;
            root_path = path_of(std::string(params["rootUri"].str()));
            std::string caps = std::string(R"({"capabilities":{"positionEncoding":")") + (utf16 ? "utf-16" : "utf-8") +
                               R"(","textDocumentSync":{"openClose":true,"change":)" + std::to_string(sync_kind) +
                               R"(,"save":true},"completionProvider":{"triggerCharacters":[".",">",":"]},"hoverProvider":true,)"
                               R"("signatureHelpProvider":{"triggerCharacters":["(",","]},"definitionProvider":true,)"
                               R"("referencesProvider":true,"renameProvider":)" +
                               (prepare ? R"({"prepareProvider":true})" : "true") +
                               R"(,"documentFormattingProvider":true,"documentRangeFormattingProvider":true,)"
                               R"("codeActionProvider":{"resolveProvider":true},"executeCommandProvider":{"commands":["fake.comment"]},)"
                               R"("inlayHintProvider":true,"implementationProvider":true,"typeDefinitionProvider":true,)"
                               R"("declarationProvider":true,"documentHighlightProvider":true,"documentSymbolProvider":true,)"
                               R"("workspaceSymbolProvider":true,"callHierarchyProvider":true,"typeHierarchyProvider":true,)"
                               R"("semanticTokensProvider":{"legend":{"tokenTypes":["type","parameter","function","macro"],)"
                               R"("tokenModifiers":["readonly"]},"full":{"delta":true}})";
            if (pull) caps += std::string(R"(,"diagnosticProvider":{"interFileDependencies":false,"workspaceDiagnostics":)") + (workspace ? "true" : "false") + "}";
            caps += "}}";
            reply(id, caps);
        } else if (method == "initialized") {
            send(R"({"jsonrpc":"2.0","id":9000,"method":"workspace/configuration","params":{"items":[{"section":"fake.level"}]}})");
            if (index_ms >= 0) {  // (as clangd does: a progress to be shown asked for, begun, and how far it is)
                send(R"({"jsonrpc":"2.0","id":9001,"method":"window/workDoneProgress/create","params":{"token":"backgroundIndexProgress"}})");
                progress(R"({"kind":"begin","title":"indexing","percentage":0})");
                progress(R"({"kind":"report","message":"1/2","percentage":50})");
                index_end = now_ms() + index_ms;
            }
        } else if (method.empty() && id.integer() == 9000) {
            std::string value;
            json::dump(value, v["result"].at(0));
            config_value = value;
        } else if (method.empty()) {
            // (The answer to workspace/applyEdit: nothing to do with it.)
        } else if (method == "textDocument/didOpen") {
            const std::string u(params["textDocument"]["uri"].str());
            Document& d = docs[u];
            d.text = params["textDocument"]["text"].str();
            d.version = static_cast<long>(params["textDocument"]["version"].integer());
            publish(u);
        } else if (method == "textDocument/didChange") {
            Document& d = docs[uri];
            d.version = static_cast<long>(params["textDocument"]["version"].integer());
            for (Value c : params["contentChanges"]) {
                if (c["range"].exists()) {
                    const size_t a = offset_of(d.text, static_cast<long>(c["range"]["start"]["line"].integer()), static_cast<long>(c["range"]["start"]["character"].integer()));
                    const size_t b = offset_of(d.text, static_cast<long>(c["range"]["end"]["line"].integer()), static_cast<long>(c["range"]["end"]["character"].integer()));
                    d.text.replace(a, b >= a ? b - a : 0, c["text"].str());
                } else {
                    d.text = c["text"].str();
                }
            }
            text = d.text;
            if (!quiet) publish(uri);
        } else if (method == "textDocument/didClose") {
            docs.erase(uri);
            if (!pull) {  // (what it said about the file is withdrawn)
                std::string m = R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":)";
                json::quote(m, uri);
                m += R"(,"diagnostics":[]}})";
                send(m);
            }
        } else if (method == "textDocument/didSave") {
        } else if (method == "textDocument/completion") {
            completion(id, params);
        } else if (method == "textDocument/hover") {
            hover(id, params);
        } else if (method == "textDocument/signatureHelp") {
            signature(id, params);
        } else if (method == "textDocument/definition") {
            definition(id, params);
        } else if (method == "textDocument/references") {
            references(id, params);
        } else if (method == "textDocument/implementation" || method == "textDocument/typeDefinition" || method == "textDocument/declaration") {
            places(id, params, method);
        } else if (method == "textDocument/documentHighlight") {
            highlight(id, params);
        } else if (method == "textDocument/documentSymbol") {
            outline(id);
        } else if (method == "workspace/symbol") {
            workspace_symbols(id, params);
        } else if (method == "textDocument/prepareCallHierarchy") {
            prepare_calls(id, params);
        } else if (method == "callHierarchy/incomingCalls" || method == "callHierarchy/outgoingCalls") {
            calls(id, params, method == "callHierarchy/incomingCalls");
        } else if (method == "textDocument/prepareTypeHierarchy") {
            prepare_types(id, params);
        } else if (method == "typeHierarchy/supertypes" || method == "typeHierarchy/subtypes") {
            types(id, params, method == "typeHierarchy/supertypes");
        } else if (method == "textDocument/switchSourceHeader") {
            switch_header(id, params);
        } else if (method == "textDocument/prepareRename") {
            prepare_rename(id, params);
        } else if (method == "textDocument/rename") {
            if (late_ms > 0) usleep(static_cast<useconds_t>(late_ms) * 1000);
            rename(id, params);
        } else if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting") {
            formatting(id, params, method == "textDocument/rangeFormatting");
        } else if (method == "textDocument/codeAction") {
            if (late_ms > 0) usleep(static_cast<useconds_t>(late_ms) * 1000);
            code_actions(id, params);
        } else if (method == "codeAction/resolve") {
            if (late_ms > 0) usleep(static_cast<useconds_t>(late_ms) * 1000);
            resolve(id, params);
        } else if (method == "workspace/executeCommand") {
            execute(id, params);
        } else if (method == "textDocument/inlayHint") {
            if (hints_once && hints_answered++) reply_error(id, "no hints now");
            else inlay_hints(id, params);
        } else if (method == "textDocument/semanticTokens/full" || method == "textDocument/semanticTokens/full/delta") {
            tokens(id, params, method.ends_with("delta"));
        } else if (method == "textDocument/diagnostic") {
            pull_document(id, params);
        } else if (method == "workspace/diagnostic") {
            pull_workspace(id);
        } else if (method == "shutdown") {
            reply(id, "null");
        } else if (method == "exit") {
            return 0;
        } else if (id.exists() && !method.empty()) {
            std::string m = R"({"jsonrpc":"2.0","id":)";
            json::dump(m, id);
            m += R"(,"error":{"code":-32601,"message":"no such method"}})";
            send(m);
        }
    }
    return 0;
}
