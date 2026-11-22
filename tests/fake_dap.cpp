// A debug adapter with predictable answers, for tests/debug.sh: it debugs
// nothing, it says what the adapters blit meets would say. tests/debug.sh
// puts it in front of blit as the machine's debugger (an xcrun and a gdb of
// its own first on the PATH, which start it).
//
//   blit_fakedap
//
// Every request it is sent is written, a line each ("command arguments"), to
// the file $BLIT_FAKE_DAP_LOG names, where it names one; and every answer to
// a request of its own ("answer:command success body"). What it does:
//   initialize    says it can restart a session where $BLIT_FAKE_DAP_RESTART
//                 is set (as lldb-dap does not)
//   launch        a program that is not there is refused as lldb-dap refuses
//                 it: no message, the reason in body.error.format (with a
//                 {path} in it, filled in from its variables); else the
//                 program "runs": it prints a line coloured with escape
//                 sequences (the first one cut in two, between two output
//                 events), and stops at once, in its own file's first line
//                 (a program with "raise" in it raises an exception at once:
//                 where the exceptions uncaught or raised are stopped at, it
//                 stops there, reason "exception"; else what it raised is
//                 printed to stderr, and it ends with code 1)
//                 Asked to run it in a terminal (lldb-dap's runInTerminal,
//                 or console "integratedTerminal"), it asks blit to
//                 (runInTerminal: a shell that says where it is and its
//                 $FAKE_TERM, then echoes a line read), as lldb-dap and
//                 debugpy ask
//                 (a module launched, as debugpy launches pytest: the file
//                 its first argument names, as pytest names a test)
//   attach        stops at once, in a frame with no source
//   setBreakpoints
//                 each with an id of its own (the same for the same line
//                 asked for again); one on a blank line is put on the next
//                 line that is not, as lldb-dap puts it (on the next with
//                 code); one in a file other than the program's is not
//                 verified, no message (lldb-dap's answer for a library not
//                 loaded yet), until the program goes on (continue): then a
//                 breakpoint event says it is, as lldb-dap's does once the
//                 library loads. As gdb would (from gdb's source, its DAP
//                 breakpoint.py: there is no gdb here): one in a file that
//                 is not there is "pending", and one whose condition names
//                 `nope` "failed", with gdb's message for a name it does not
//                 know
//   threads, stackTrace, scopes, variables
//                 one thread, one frame, the Locals of two variables (df, total)
//   evaluate      "<the expression>"; "break N" makes a breakpoint on line N of
//                 the program and "delete ID" takes one away, each said in a
//                 breakpoint event (new, removed), as gdb's console commands
//                 are
//   initialize    says it can (among the rest) stop at debugpy's exceptions:
//                 raised, uncaught (by default) and userUnhandled, each with a
//                 condition; with no default where BLIT_FAKE_DAP_NODEFAULTS is set
//   threads, stackTrace, scopes, variables
//                 one thread, one frame, the Locals of two variables (df, total);
//                 df evaluated has members (shape, and columns, which has its own)
//   evaluate      "<the expression>"; in the "repl" context, df is a value with
//                 members and nope is refused (Python's traceback for a name it has not)
//   exceptionInfo debugpy's account of a ValueError raised, with its stack
//                 trace and the KeyError it was raised while handling
//   restart       says "restarted", and stops again
// (With $BLIT_FAKE_DAP_PROMPT set, it writes that once configured, with no
// newline after it: a program's prompt, waiting for what is typed.)
//   evaluate      "<the expression>" (one that starts with "fail" fails, as
//                 lldb-dap 21 answers an expression before the program has stopped)
//   completions   the words of a fixed list that start with the word before
//                 the column, each to take that word's place; its start
//                 counted from 0 whatever the client asked for, as debugpy
//                 counts it
//   continue, next, stepIn, stepOut
//                 stops again where it was
//   disconnect    answered, and it ends
// With $BLIT_FAKE_DAP_WAVES set to a number, it is rocgdb debugging a HIP
// program: it says so on its error stream as blit's start of rocgdb does;
// three threads of the program's own and that many of the GPU's waves (with
// no names: their names are `info threads`'s, which evaluate answers as
// rocgdb's manual shows it, there being no rocgdb here); and it stops in the
// wave halfway along, in the kernel's frame.
// Where BLIT_FAKE_DAP_GDB is set it says first, on its error stream, what
// blit's script for the machine's debugger says when it starts gdb (blit
// then takes it for gdb: its "monitor" lines, its hit counts).
//
// (debugpy's filters, its exception stop, its exceptionInfo and its refusal
// are as debugpy 1.8.22 answers, but for the exception inside the one raised
// (innerException: the protocol's, which debugpy did not give for this) and
// a condition on each filter (lldb-dap's way: supportsCondition, and
// supportsExceptionFilterOptions, which debugpy does not say). The rest is
// as lldb-dap and gdb answer.)
#include <strings.h>
#include <sys/stat.h>

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

long seq = 1;
std::string program;  // what was launched ("": attached)
long waves = 0;       // the GPU's waves, as rocgdb has them ($BLIT_FAKE_DAP_WAVES)
long stop_thread = 1;  // the thread that stops
constexpr long kOwnThreads = 3;  // (the waves' ids are after these)
// Breakpoints: the id of each (file, line asked for), and those not verified
// until the program goes on (a library's, loaded then): id and line.
std::map<std::pair<std::string, long>, long> break_ids;
long next_break_id = 1;
std::map<std::string, std::vector<std::pair<long, long>>> loads_later;  // (by file)
bool raises = false;    // (and it raises an exception)
bool catching = false;  // the exceptions uncaught or raised are stopped at

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

void output(std::string_view text, std::string_view category = "stdout") {
    std::string b = "{\"category\":";
    json::quote(b, category);
    b += ",\"output\":";
    json::quote(b, text);
    event("output", b + "}");
}

void stopped(std::string_view reason) {
    std::string b = "{\"reason\":";
    json::quote(b, reason);
    event("stopped", b + ",\"threadId\":" + std::to_string(stop_thread) + ",\"allThreadsStopped\":true}");
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

// The threads, and what rocgdb's `info threads` says of them: wave n is
// the (n % 4)th of work-group (n / 4, 0, 0).
std::string threads() {
    if (!waves) return R"({"threads":[{"id":1,"name":"main"}]})";
    std::string out = "{\"threads\":[";
    for (long id = 1; id <= kOwnThreads; ++id) out += std::string(id > 1 ? "," : "") + "{\"id\":" + std::to_string(id) + ",\"name\":\"vadd\"}";
    for (long n = 0; n < waves; ++n) out += ",{\"id\":" + std::to_string(kOwnThreads + 1 + n) + "}";
    return out + "]}";
}

std::string info_threads() {
    std::string out = "  Id   Target Id                                    Frame \n";
    char line[256];
    for (long id = 1; id <= kOwnThreads; ++id) {
        std::snprintf(line, sizeof line, "%c %-4ld Thread 0x7ffff%04lx (LWP %ld) \"vadd\" 0x00007ffff7108d2b in ioctl () from /lib/x86_64-linux-gnu/libc.so.6\n", id == stop_thread ? '*' : ' ', id, id * 4096, 3262 + id);
        out += line;
    }
    for (long n = 0; n < waves; ++n) {
        const long id = kOwnThreads + 1 + n;
        std::snprintf(line, sizeof line, "%c %-4ld AMDGPU Wave 1:1:1:%ld (%ld,0,0)/%ld \"vadd\" vadd (a=0x7ffff5000000, n=1024) at vadd.hip:9\n", id == stop_thread ? '*' : ' ', id, n + 1, n / 4, n % 4);
        out += line;
    }
    return out;
}

// A file's lines (none if it cannot be read).
std::vector<std::string> lines_of(const std::string& path) {
    std::vector<std::string> out;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return out;
    std::string line;
    for (int c; (c = std::fgetc(f)) != EOF;) {
        if (c != '\n') line += static_cast<char>(c);
        else out.push_back(line), line.clear();
    }
    if (!line.empty()) out.push_back(line);
    std::fclose(f);
    return out;
}

// The answer to a file's breakpoints.
std::string set_breakpoints(const Value& a) {
    const std::string path(a["source"]["path"].str());
    const std::vector<std::string> lines = lines_of(path);
    loads_later.erase(path);
    std::string b = "{\"breakpoints\":[";
    for (size_t i = 0; i < a["breakpoints"].size(); ++i) {
        long line = a["breakpoints"].at(i)["line"].integer();
        auto [it, fresh] = break_ids.try_emplace({path, line}, next_break_id);
        if (fresh) ++next_break_id;
        const std::string id = "{\"id\":" + std::to_string(it->second);
        b += i ? "," : "";
        if (a["breakpoints"].at(i)["condition"].str().find("nope") != std::string_view::npos) {
            b += id + R"(,"verified":false,"reason":"failed","message":"No symbol \"nope\" in current context."})";
        } else if (lines.empty()) {
            b += id + R"(,"verified":false,"reason":"pending"})";
        } else if (!program.empty() && path != program) {
            b += id + ",\"verified\":false,\"line\":" + std::to_string(line) + "}";
            loads_later[path].emplace_back(it->second, line);
        } else {
            while (line < static_cast<long>(lines.size()) && lines[static_cast<size_t>(line - 1)].find_first_not_of(" \t") == std::string::npos) ++line;
            b += id + ",\"verified\":true,\"line\":" + std::to_string(line) + "}";
        }
    }
    return b + "]}";
}

}  // namespace

// What debugpy says of the ValueError the program raised (its stack trace, and the KeyError it was handling).
std::string exception_info() {
    std::string where = "  File ";
    json::quote(where, program);
    where += ", line 1, in <module>\n";
    std::string b = R"({"exceptionId":"ValueError","description":"bad value","breakMode":"unhandled","details":{"message":"bad value","typeName":"ValueError","fullTypeName":"builtins.ValueError","stackTrace":)";
    json::quote(b, where + "    raise ValueError(\"bad value\")\nValueError: bad value\n");
    b += R"(,"innerException":[{"message":"'k'","typeName":"KeyError","fullTypeName":"builtins.KeyError","stackTrace":)";
    json::quote(b, where + "    d[\"k\"]\n");
    return b + "}]}}";
}

int main() {
    if (const char* w = std::getenv("BLIT_FAKE_DAP_WAVES"); w && *w) {
        waves = std::atol(w);
        stop_thread = kOwnThreads + 1 + waves / 2;
        std::fprintf(stderr, "blit-debugger: gdb (rocgdb)\n");
        std::fflush(stderr);
    }
    if (const char* g = std::getenv("BLIT_FAKE_DAP_GDB"); g && *g) std::fprintf(stderr, "blit-debugger: gdb\n");
    const bool defaults = !std::getenv("BLIT_FAKE_DAP_NODEFAULTS");
    std::string body;
    while (read_message(&body)) {
        json::Doc doc;
        if (!doc.parse(body)) continue;
        const Value v = doc.root();
        if (v["type"].str() == "response") {  // (to a request of its own)
            log("answer:" + std::string(v["command"].str()) + (v["success"].boolean() ? " true" : " false"), v["body"]);
            continue;
        }
        if (v["type"].str() != "request") continue;
        const std::string_view command = v["command"].str();
        const Value a = v["arguments"];
        log(command, a);
        if (command == "initialize") {
            std::string filters;
            for (const char* f : {R"("filter":"raised","label":"Raised Exceptions","description":"Break whenever any exception is raised.")",
                                  R"("filter":"uncaught","label":"Uncaught Exceptions","description":"Break when the process is exiting due to unhandled exception.")",
                                  R"("filter":"userUnhandled","label":"User Uncaught Exceptions","description":"Break when exception escapes into library code.")"}) {
                filters += std::string(filters.empty() ? "{" : ",{") + f + ",\"supportsCondition\":true";
                if (defaults) filters += std::string(",\"default\":") + (std::strstr(f, "\"uncaught\"") ? "true" : "false");
                filters += "}";
            }
            const char* restart = std::getenv("BLIT_FAKE_DAP_RESTART");
            respond(v, true, R"({"supportsConfigurationDoneRequest":true,"supportsCompletionsRequest":true,"supportsFunctionBreakpoints":true,)"
                             R"("supportsExceptionFilterOptions":true,"supportsExceptionInfoRequest":true,"exceptionBreakpointFilters":[)" + filters + "]" +
                                 (restart && *restart ? std::string(",\"supportsRestartRequest\":true}") : std::string("}")));
        } else if (command == "restart") {
            respond(v, true);
            output("restarted\n");
            stopped("breakpoint");
        } else if (command == "launch" && (a["runInTerminal"].boolean() || a["console"].str() == "integratedTerminal")) {  // (asked of blit, then launched)
            std::string r = R"({"kind":"integrated","title":"fake","cwd":)";
            json::quote(r, a["cwd"].str());
            r += R"(,"args":["/bin/sh","-c","echo \"in the terminal: $FAKE_TERM in ${PWD##*/}\"; read line; echo \"read: $line\""],"env":{"FAKE_TERM":"yes"}})";
            send("{\"seq\":" + std::to_string(seq++) + ",\"type\":\"request\",\"command\":\"runInTerminal\",\"arguments\":" + r + "}");
            program = std::string(a["program"].str());
            respond(v, true);
            event("initialized");
        } else if (command == "launch") {
            // (A module run, as debugpy runs pytest: "its file" is that of the
            // first argument, as pytest names a test, in the folder it runs in.)
            std::string path(a["program"].str());
            if (a["module"].is_string() && path.empty()) {
                const std::string first(a["args"].at(0).str());
                path = std::string(a["cwd"].str()) + "/" + first.substr(0, first.find("::"));
            }
            struct stat st;
            if (::stat(path.c_str(), &st) != 0) {
                std::string why = R"({"error":{"id":1,"format":"'{path}' does not exist","variables":{"path":)";
                json::quote(why, path);
                respond(v, false, why + "},\"showUser\":true}}");
                continue;
            }
            program = path;
            if (FILE* f = std::fopen(path.c_str(), "r")) {
                char text[4096];
                const size_t n = std::fread(text, 1, sizeof text - 1, f);
                text[n] = 0;
                raises = std::strstr(text, "raise") != nullptr;
                std::fclose(f);
            }
            respond(v, true);
            event("initialized");  // (as lldb-dap has it: once there is something to configure)
        } else if (command == "attach") {
            respond(v, true);
            event("initialized");
        } else if (command == "setBreakpoints") {
            respond(v, true, set_breakpoints(a));
        } else if (command == "setExceptionBreakpoints") {
            catching = false;
            for (const Value f : a["filters"]) catching |= f.str() == "uncaught" || f.str() == "raised";
            for (const Value f : a["filterOptions"]) catching |= f["filterId"].str() == "uncaught" || f["filterId"].str() == "raised";
            respond(v, true);
        } else if (command == "configurationDone") {
            respond(v, true);
            event("process", "{\"name\":\"fake\",\"startMethod\":\"launch\"}");
            if (raises && catching) {
                event("stopped", R"({"reason":"exception","description":"bad value","text":"ValueError","threadId":1,"allThreadsStopped":true})");
                continue;
            }
            if (raises) {  // (not stopped at: it ends, as Python does)
                output("Traceback (most recent call last):\n", "stderr");
                output("ValueError: bad value\n", "stderr");
                event("exited", R"({"exitCode":1})");
                event("terminated");
                continue;
            }
            if (!program.empty()) {
                output("\x1b[1;3");
                output("1mred\x1b[0m plain\n");
            }
            if (const char* ask = std::getenv("BLIT_FAKE_DAP_PROMPT"); ask && *ask) output(ask);  // (a line not ended: a program's prompt, waiting)
            stopped("breakpoint");
        } else if (command == "threads") {
            respond(v, true, threads());
        } else if (command == "stackTrace") {
            std::string f = a["threadId"].integer() > kOwnThreads ? R"({"stackFrames":[{"id":1,"name":"vadd","line":1,"column":1)" : R"({"stackFrames":[{"id":1,"name":"main","line":1,"column":1)";
            if (!program.empty()) {
                f += ",\"source\":{\"path\":";
                json::quote(f, program);
                f += "}";
            }
            respond(v, true, f + "}],\"totalFrames\":1}");
        } else if (command == "scopes") {
            respond(v, true, R"({"scopes":[{"name":"Locals","presentationHint":"locals","variablesReference":1,"expensive":false}]})");
        } else if (command == "variables") {
            const i64 ref = a["variablesReference"].integer();
            if (ref == 2)  // (df's members)
                respond(v, true, R"j({"variables":[{"name":"shape","value":"(3, 2)","type":"tuple","variablesReference":0},{"name":"columns","value":"Index(['a', 'b'])","type":"Index","variablesReference":3}]})j");
            else if (ref == 3)
                respond(v, true, R"({"variables":[{"name":"0","value":"'a'","type":"str","variablesReference":0},{"name":"1","value":"'b'","type":"str","variablesReference":0}]})");
            else
                respond(v, true, R"({"variables":[{"name":"df","value":"DataFrame","variablesReference":0},{"name":"total","value":"1","variablesReference":0}]})");
        } else if (command == "evaluate" && a["expression"].str().starts_with("fail")) {
            respond(v, false, R"({"error":{"id":2,"format":"can't evaluate expressions when the process is running."}})");
        } else if (command == "evaluate" && waves && a["expression"].str() == "info threads") {
            std::string r = "{\"result\":";
            json::quote(r, info_threads());
            respond(v, true, r + ",\"variablesReference\":0}");
        } else if (command == "evaluate" && a["expression"].str().starts_with("break ")) {
            const long id = next_break_id++, line = std::atol(std::string(a["expression"].str().substr(6)).c_str());
            std::string e = "{\"reason\":\"new\",\"breakpoint\":{\"id\":" + std::to_string(id) + ",\"verified\":true,\"line\":" + std::to_string(line) + ",\"source\":{\"path\":";
            json::quote(e, program);
            event("breakpoint", e + "}}}");
            std::string r = "{\"result\":";
            json::quote(r, "Breakpoint " + std::to_string(id) + " at 0x401136: file " + program.substr(program.find_last_of('/') + 1) + ", line " + std::to_string(line) + ".");
            respond(v, true, r + ",\"variablesReference\":0}");
        } else if (command == "evaluate" && a["expression"].str().starts_with("delete ")) {
            event("breakpoint", "{\"reason\":\"removed\",\"breakpoint\":{\"id\":" + std::string(a["expression"].str().substr(7)) + "}}");
            respond(v, true, R"({"result":"","variablesReference":0})");
        } else if (command == "evaluate") {
            const std::string_view e = a["expression"].str();
            const bool repl = a["context"].str() == "repl";
            if (repl && e == "nope") {
                send("{\"seq\":" + std::to_string(seq++) + ",\"type\":\"response\",\"request_seq\":" + std::to_string(v["seq"].integer()) +
                     R"(,"command":"evaluate","success":false,"message":"Traceback (most recent call last):\n  File \"<string>\", line 1, in <module>\nNameError: name 'nope' is not defined\n"})");
                continue;
            }
            std::string r = "{\"result\":";
            json::quote(r, repl && e == "df" ? std::string("DataFrame") : "<" + std::string(e) + ">");
            respond(v, true, r + ",\"variablesReference\":" + (repl && e == "df" ? "2" : "0") + "}");
        } else if (command == "exceptionInfo") {
            respond(v, true, exception_info());
        } else if (command == "completions") {
            respond(v, true, completions(a["text"].str(), a["column"].integer()));
        } else if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
            respond(v, true, command == "continue" ? R"({"allThreadsContinued":true})" : "{}");
            for (const auto& [path, those] : loads_later)  // (its library loaded as it goes on)
                for (const auto& [id, line] : those) event("breakpoint", "{\"reason\":\"changed\",\"breakpoint\":{\"id\":" + std::to_string(id) + ",\"verified\":true,\"line\":" + std::to_string(line) + "}}");
            loads_later.clear();
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
