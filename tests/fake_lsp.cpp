// A language server with predictable answers, for the tests and benchmarks.
//
//   blit_fakelsp [--utf8] [--full] [--slow MS] [--die-after N] [--items N] [--quiet]
//
// --utf8        agree to count columns in bytes (the default is UTF-16)
// --full        ask for the whole document on every change
// --slow MS     wait that long before every answer
// --die-after N exit without a word after N messages
// --items N     pad completion lists to N items
// --quiet       send diagnostics when a document opens, but not on changes
//
// What it does with a document:
//   diagnostics  one for every ERROR, WARN, INFO and HINT in the text, plus a
//                hint at the very start whose message is "text:" and a hash
//                of the server's copy of the document, so a test can tell
//                whether the copy is right
//   completion   a fixed vocabulary, replacing the word before the cursor
//   hover        a description of the word under the cursor
//   signature    one signature, for the call the cursor is in
//   definition   the first place the word appears
#include <strings.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
bool quiet = false;
long handled = 0;

std::string text, uri, config_value = "unset";
long version = 0;

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
size_t offset_of(long line, long ch) {
    size_t at = 0;
    for (long l = 0; l < line; ++l) {
        const size_t nl = text.find('\n', at);
        if (nl == std::string::npos) return text.size();
        at = nl + 1;
    }
    long units = 0;
    while (at < text.size() && text[at] != '\n') {
        const unsigned char b = static_cast<unsigned char>(text[at]);
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

size_t offset_of(Value pos) { return offset_of(static_cast<long>(pos["line"].integer()), static_cast<long>(pos["character"].integer())); }

void position_json(std::string& out, size_t off) {
    long line = 0, ch = 0;
    size_t start = 0;
    for (size_t i = 0; i < off && i < text.size(); ++i)
        if (text[i] == '\n') {
            ++line;
            start = i + 1;
        }
    for (size_t i = start; i < off && i < text.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        if (utf16) ch += ((b & 0xC0) != 0x80) + (b >= 0xF0);
        else ++ch;
    }
    out += "{\"line\":" + std::to_string(line) + ",\"character\":" + std::to_string(ch) + "}";
}

void range_json(std::string& out, size_t a, size_t b) {
    out += "{\"start\":";
    position_json(out, a);
    out += ",\"end\":";
    position_json(out, b);
    out += "}";
}

void publish() {
    unsigned long long hash = 1469598103934665603ull;
    for (const char c : text) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016llx", hash);

    std::string m = R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":)";
    json::quote(m, uri);
    m += ",\"version\":" + std::to_string(version) + ",\"diagnostics\":[";
    m += R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"severity":4,"source":"fake","message":"text:)";
    m += hex;
    m += "\"}";
    static const struct {
        const char* word;
        int severity;
    } kinds[] = {{"ERROR", 1}, {"WARN", 2}, {"INFO", 3}, {"HINT", 4}};
    for (const auto& k : kinds) {
        const size_t n = std::strlen(k.word);
        for (size_t at = text.find(k.word); at != std::string::npos; at = text.find(k.word, at + n)) {
            m += ",{\"range\":";
            range_json(m, at, at + n);
            m += ",\"severity\":" + std::to_string(k.severity) + R"(,"source":"fake","code":"F)" + std::to_string(k.severity) +
                 R"(","message":"found )" + k.word + "\\nsecond line\"}";
        }
    }
    m += "]}}";
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
    const std::string w = word_at(off);
    if (w.empty()) return reply(id, "null");
    size_t at = 0;
    for (;;) {
        at = text.find(w, at);
        if (at == std::string::npos) return reply(id, "null");
        const bool whole = (at == 0 || !word_char(text[at - 1])) && (at + w.size() >= text.size() || !word_char(text[at + w.size()]));
        if (whole) break;
        ++at;
    }
    std::string r = "[{\"uri\":";
    json::quote(r, uri);
    r += ",\"range\":";
    range_json(r, at, at + w.size());
    r += "}]";
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
    }
    std::string body;
    while (read_message(&body)) {
        if (die_after >= 0 && handled++ >= die_after) return 3;
        json::Doc doc;
        if (!doc.parse(body)) continue;
        const Value v = doc.root();
        const std::string_view method = v["method"].str();
        const Value id = v["id"], params = v["params"];
        if (method == "initialize") {
            // Honour the encoding only if the client offered it.
            bool offered_utf8 = false;
            for (Value e : params["capabilities"]["general"]["positionEncodings"]) offered_utf8 |= e.str() == "utf-8";
            if (!offered_utf8) utf16 = true;
            reply(id, std::string(R"({"capabilities":{"positionEncoding":")") + (utf16 ? "utf-16" : "utf-8") +
                          R"(","textDocumentSync":{"openClose":true,"change":)" + std::to_string(sync_kind) +
                          R"(,"save":true},"completionProvider":{"triggerCharacters":[".",">",":"]},"hoverProvider":true,)"
                          R"("signatureHelpProvider":{"triggerCharacters":["(",","]},"definitionProvider":true}})");
        } else if (method == "initialized") {
            send(R"({"jsonrpc":"2.0","id":9000,"method":"workspace/configuration","params":{"items":[{"section":"fake.level"}]}})");
        } else if (method.empty() && id.integer() == 9000) {
            std::string value;
            json::dump(value, v["result"].at(0));
            config_value = value;
        } else if (method == "textDocument/didOpen") {
            uri = params["textDocument"]["uri"].str();
            text = params["textDocument"]["text"].str();
            version = static_cast<long>(params["textDocument"]["version"].integer());
            publish();
        } else if (method == "textDocument/didChange") {
            version = static_cast<long>(params["textDocument"]["version"].integer());
            for (Value c : params["contentChanges"]) {
                if (c["range"].exists()) {
                    const size_t a = offset_of(c["range"]["start"]), b = offset_of(c["range"]["end"]);
                    text.replace(a, b >= a ? b - a : 0, c["text"].str());
                } else {
                    text = c["text"].str();
                }
            }
            if (!quiet) publish();
        } else if (method == "textDocument/completion") {
            completion(id, params);
        } else if (method == "textDocument/hover") {
            hover(id, params);
        } else if (method == "textDocument/signatureHelp") {
            signature(id, params);
        } else if (method == "textDocument/definition") {
            definition(id, params);
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
