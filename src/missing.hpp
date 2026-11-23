#pragma once
// A program a feature runs that is not on the machine it runs on, said the
// same way wherever that feature shows what went wrong: "cmake is not
// installed on this Mac", "gdb is not installed on lab". Where blit asks
// first, it asks Engine::which (engine.hpp), here or through the agent;
// where it finds out by running the program, the spawn's error or what the
// shell said in its place is read here.
#include <string>
#include <string_view>

namespace blit {

// The machine as the warnings name it: this one, or a host by its name.
inline std::string machine_name(std::string_view host) {
#if defined(__APPLE__)
    if (host.empty()) return "this Mac";
#else
    if (host.empty()) return "this machine";
#endif
    return std::string(host);
}

inline std::string not_installed(std::string_view program, std::string_view host) {
    return std::string(program) + " is not installed on " + machine_name(host);
}

// The program a shell (or env, or uv run) said it could not find, in what
// it printed: zsh's "zsh:1: command not found: cmake", bash's "bash: line
// 1: cmake: command not found", sh's "sh: 1: exec: cmake: not found",
// fish's "fish: Unknown command: cmake", "env: cmake: No such file or
// directory", uv's "Failed to spawn: `cmake`". "" if it said none of those.
// (Only a line that begins with the shell's own name counts: a tool's "not
// found" is about something else.)
inline std::string shell_not_found(std::string_view text) {
    std::string found;
    auto clean = [](std::string_view w) {
        while (!w.empty() && (w.front() == ' ' || w.front() == '`' || w.front() == '\'' || w.front() == '"')) w.remove_prefix(1);
        while (!w.empty() && (w.back() == ' ' || w.back() == '`' || w.back() == '\'' || w.back() == '"' || w.back() == '\r')) w.remove_suffix(1);
        if (w.starts_with("\xE2\x80\x98") && w.ends_with("\xE2\x80\x99") && w.size() >= 6) w = w.substr(3, w.size() - 6);  // (GNU's quotes)
        return w.find_first_of(" \t") == std::string_view::npos ? std::string(w) : std::string();
    };
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (const size_t u = line.find("Failed to spawn: `"); u != std::string_view::npos) {
            const std::string_view rest = line.substr(u + 18);
            if (const size_t q = rest.find('`'); q != std::string_view::npos && q > 0) found = clean(rest.substr(0, q));
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view who = line.substr(0, colon);
        if (const size_t slash = who.rfind('/'); slash != std::string_view::npos) who = who.substr(slash + 1);
        if (!(who == "env" || (who.size() >= 2 && who.ends_with("sh") && who.find(' ') == std::string_view::npos))) continue;
        std::string_view rest = line.substr(colon + 1);
        std::string name;
        if (const size_t c = rest.find("command not found: "); c != std::string_view::npos) {
            name = clean(rest.substr(c + 19));
        } else if (const size_t f = rest.find("Unknown command: "); f != std::string_view::npos) {
            name = clean(rest.substr(f + 17));
        } else {
            for (const std::string_view tail : {": command not found", ": not found", ": No such file or directory"}) {
                if (!rest.ends_with(tail) && !(rest.ends_with("\r") && rest.substr(0, rest.size() - 1).ends_with(tail))) continue;
                std::string_view before = rest.substr(0, rest.rfind(tail)), verb;
                if (const size_t cut = before.rfind(": "); cut != std::string_view::npos) {
                    verb = before.substr(0, cut);
                    before = before.substr(cut + 2);
                    if (const size_t v = verb.rfind(": "); v != std::string_view::npos) verb = verb.substr(v + 2);
                }
                if (verb == "cd" || verb == "source" || verb == ".") break;  // (a folder or a file, not a program)
                name = clean(before);
                break;
            }
        }
        if (!name.empty()) found = std::move(name);
    }
    return found;
}

}  // namespace blit
