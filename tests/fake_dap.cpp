// A debug adapter with predictable answers, for tests/debug.sh: it debugs
// nothing, it says what the adapters blit meets would say. tests/debug.sh
// puts it in front of blit as the machine's debugger (an xcrun and a gdb of
// its own first on the PATH, which start it).
//
//   blit_fakedap
//
// Every request it is sent is written, a line each ("command arguments"), to
// the file $BLIT_FAKE_DAP_LOG names, where it names one. What it does:
//   launch        a program that is not there is refused as lldb-dap refuses
//                 it: no message, the reason in body.error.format (with a
//                 {path} in it, filled in from its variables); else the
//                 program "runs": it prints a line coloured with escape
//                 sequences (the first one cut in two, between two output
//                 events), and stops at once, in its own file's first line
//   attach        stops at once, in a frame with no source
//   threads, stackTrace, scopes, variables
//                 one thread, one frame, the Locals of two variables (df, total)
//   evaluate      "<the expression>"
//   completions   the words of a fixed list that start with the word before
//                 the column, each to take that word's place; its start
//                 counted from 0 whatever the client asked for, as debugpy
//                 counts it
//   continue, next, stepIn, stepOut
//                 stops again where it was
//   disconnect    answered, and it ends
#include <strings.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "json.hpp"

using namespace blit;
using json::Value;

namespace {

long seq = 1;
std::string program;  // what was launched ("": attached)

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
    std::printf("Content-Length: %zu\r\n\r\n", body.size());
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

void respond(const Value& request, bool success, const std::string& body = "{}") {
    std::string m = "{\"seq\":" + std::to_string(seq++) + ",\"type\":\"response\",\"request_seq\":" + std::to_string(request["seq"].integer()) + ",\"command\":";
    json::quote(m, request["command"].str());
    m += std::string(",\"success\":") + (success ? "true" : "false") + ",\"body\":" + body + "}";
    send(m);
}

void event(std::string_view name, const std::string& body = "{}") {
    std::string m = "{\"seq\":" + std::to_string(seq++) + ",\"type\":\"event\",\"event\":";
    json::quote(m, name);
    send(m + ",\"body\":" + body + "}");
}

void output(std::string_view text) {
    std::string b = "{\"category\":\"stdout\",\"output\":";
    json::quote(b, text);
    event("output", b + "}");
}

void stopped(std::string_view reason) {
    std::string b = "{\"reason\":";
    json::quote(b, reason);
    event("stopped", b + ",\"threadId\":1,\"allThreadsStopped\":true}");
}

void log(std::string_view command, const Value& arguments) {
    const char* file = std::getenv("BLIT_FAKE_DAP_LOG");
    if (!file || !*file) return;
    std::string line(command);
    line += ' ';
    json::dump(line, arguments);
    if (FILE* f = std::fopen(file, "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
}

// The completions of the word before `column` (from 1) in `text`.
std::string completions(std::string_view text, i64 column) {
    const size_t end = std::min<size_t>(static_cast<size_t>(std::max<i64>(1, column) - 1), text.size());
    size_t start = end;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1])) || text[start - 1] == '_')) --start;
    const std::string_view word = text.substr(start, end - start);
    std::string out = "{\"targets\":[";
    bool first = true;
    for (const char* w : {"describe", "dtypes", "shape", "total"}) {
        if (!std::string_view(w).starts_with(word)) continue;
        out += std::string(first ? "" : ",") + "{\"label\":\"" + w + "\",\"text\":\"" + w + "\",\"start\":" + std::to_string(start) + ",\"length\":" + std::to_string(word.size()) + "}";
        first = false;
    }
    return out + "]}";
}

}  // namespace

int main() {
    std::string body;
    while (read_message(&body)) {
        json::Doc doc;
        if (!doc.parse(body)) continue;
        const Value v = doc.root();
        if (v["type"].str() != "request") continue;
        const std::string_view command = v["command"].str();
        const Value a = v["arguments"];
        log(command, a);
        if (command == "initialize") {
            respond(v, true, R"({"supportsConfigurationDoneRequest":true,"supportsCompletionsRequest":true,"supportsFunctionBreakpoints":true})");
        } else if (command == "launch") {
            const std::string path(a["program"].str());
            struct stat st;
            if (::stat(path.c_str(), &st) != 0) {
                std::string why = R"({"error":{"id":1,"format":"'{path}' does not exist","variables":{"path":)";
                json::quote(why, path);
                respond(v, false, why + "},\"showUser\":true}}");
                continue;
            }
            program = path;
            respond(v, true);
            event("initialized");  // (as lldb-dap has it: once there is something to configure)
        } else if (command == "attach") {
            respond(v, true);
            event("initialized");
        } else if (command == "setBreakpoints") {
            std::string b = "{\"breakpoints\":[";
            for (size_t i = 0; i < a["breakpoints"].size(); ++i) b += std::string(i ? "," : "") + "{\"verified\":true,\"line\":" + std::to_string(a["breakpoints"].at(i)["line"].integer()) + "}";
            respond(v, true, b + "]}");
        } else if (command == "configurationDone") {
            respond(v, true);
            event("process", "{\"name\":\"fake\",\"startMethod\":\"launch\"}");
            if (!program.empty()) {
                output("\x1b[1;3");
                output("1mred\x1b[0m plain\n");
            }
            stopped("breakpoint");
        } else if (command == "threads") {
            respond(v, true, R"({"threads":[{"id":1,"name":"main"}]})");
        } else if (command == "stackTrace") {
            std::string f = R"({"stackFrames":[{"id":1,"name":"main","line":1,"column":1)";
            if (!program.empty()) {
                f += ",\"source\":{\"path\":";
                json::quote(f, program);
                f += "}";
            }
            respond(v, true, f + "}],\"totalFrames\":1}");
        } else if (command == "scopes") {
            respond(v, true, R"({"scopes":[{"name":"Locals","presentationHint":"locals","variablesReference":1,"expensive":false}]})");
        } else if (command == "variables") {
            respond(v, true, R"({"variables":[{"name":"df","value":"DataFrame","variablesReference":0},{"name":"total","value":"1","variablesReference":0}]})");
        } else if (command == "evaluate") {
            std::string r = "{\"result\":";
            json::quote(r, "<" + std::string(a["expression"].str()) + ">");
            respond(v, true, r + ",\"variablesReference\":0}");
        } else if (command == "completions") {
            respond(v, true, completions(a["text"].str(), a["column"].integer()));
        } else if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
            respond(v, true, command == "continue" ? R"({"allThreadsContinued":true})" : "{}");
            stopped(command == "continue" ? "breakpoint" : "step");
        } else if (command == "disconnect") {
            respond(v, true);
            return 0;
        } else {
            respond(v, true);
        }
    }
    return 0;
}
