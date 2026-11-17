// A language server with predictable answers, for the tests and benchmarks.
//
//   blit_fakelsp [--utf8] [--full] [--slow MS] [--die-after N] [--items N] [--quiet]
//                [--pull] [--workspace] [--no-prepare] [--changes] [--name NAME]
//
// --utf8        agree to count columns in bytes (the default is UTF-16)
// --full        ask for the whole document on every change
// --slow MS     wait that long before every answer
// --die-after N exit without a word after N messages
// --items N     pad completion lists to N items
// --quiet       send diagnostics when a document opens, but not on changes
// --pull        send no diagnostics: they are asked for (textDocument/diagnostic)
// --workspace   and the whole project's too (workspace/diagnostic): its folder's
//               files that are not open
// --no-prepare  rename without saying first what would be renamed
// --changes     workspace edits as "changes" (by file) rather than "documentChanges"
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
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

using namespace blit;
using json::Value;

namespace {

bool utf16 = true;
int sync_kind = 2;
int slow_ms = 0;
long die_after = -1;
long pad_items = 0;
bool quiet = false, pull = false, workspace = false, prepare = true, by_changes = false;
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

void send(const std::string& body) {
    if (slow_ms > 0) usleep(static_cast<useconds_t>(slow_ms) * 1000);
    std::printf("Content-Length: %zu\r\n\r\n", body.size());
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
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

void prepare_rename(Value id, Value params) {
    size_t start = 0;
    const std::string w = word_at(offset_of(params["position"]), &start);
    if (w.empty()) return reply(id, "null");
    if (w == "ERROR") return reply_error(id, "ERROR cannot be renamed");
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

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--utf8") utf16 = false;
        else if (a == "--full") sync_kind = 1;
        else if (a == "--slow" && i + 1 < argc) slow_ms = std::atoi(argv[++i]);
        else if (a == "--die-after" && i + 1 < argc) die_after = std::atol(argv[++i]);
        else if (a == "--items" && i + 1 < argc) pad_items = std::atol(argv[++i]);
        else if (a == "--quiet") quiet = true;
        else if (a == "--pull") pull = true;
        else if (a == "--workspace") pull = workspace = true;
        else if (a == "--no-prepare") prepare = false;
        else if (a == "--changes") by_changes = true;
        else if (a == "--name" && i + 1 < argc) source = argv[++i];
    }
    std::string body;
    while (read_message(&body)) {
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
                               R"("inlayHintProvider":true,)"
                               R"("semanticTokensProvider":{"legend":{"tokenTypes":["type","parameter","function","macro"],)"
                               R"("tokenModifiers":["readonly"]},"full":{"delta":true}})";
            if (pull) caps += std::string(R"(,"diagnosticProvider":{"interFileDependencies":false,"workspaceDiagnostics":)") + (workspace ? "true" : "false") + "}";
            caps += "}}";
            reply(id, caps);
        } else if (method == "initialized") {
            send(R"({"jsonrpc":"2.0","id":9000,"method":"workspace/configuration","params":{"items":[{"section":"fake.level"}]}})");
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
        } else if (method == "textDocument/prepareRename") {
            prepare_rename(id, params);
        } else if (method == "textDocument/rename") {
            rename(id, params);
        } else if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting") {
            formatting(id, params, method == "textDocument/rangeFormatting");
        } else if (method == "textDocument/codeAction") {
            code_actions(id, params);
        } else if (method == "codeAction/resolve") {
            resolve(id, params);
        } else if (method == "workspace/executeCommand") {
            execute(id, params);
        } else if (method == "textDocument/inlayHint") {
            inlay_hints(id, params);
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
