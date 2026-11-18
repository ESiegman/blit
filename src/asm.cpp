#include "asm.hpp"

#include <algorithm>
#include <charconv>
#include <map>

#include "json.hpp"

namespace blit {

namespace {

int number(std::string_view s) {
    int n = 0;
    std::from_chars(s.data(), s.data() + s.size(), n);
    return n;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// The quoted words of a directive ("dir" "name", or "path"), joined as a path.
std::string quoted_path(std::string_view s) {
    std::vector<std::string> parts;
    for (size_t at = 0; (at = s.find('"', at)) != std::string_view::npos;) {
        const size_t end = s.find('"', at + 1);
        if (end == std::string_view::npos) break;
        parts.emplace_back(s.substr(at + 1, end - at - 1));
        at = end + 1;
    }
    if (parts.empty()) return {};
    if (parts.size() >= 2 && !parts[1].empty() && parts[1][0] != '/') return parts[0] + "/" + parts[1];
    return parts.size() >= 2 && !parts[1].empty() ? parts[1] : parts[0];
}

// Whether a path the compiler names is the file compiled: the same, or (a
// relative one) its end.
bool same_file(std::string_view named, std::string_view source) {
    if (named.empty()) return false;
    if (named == source) return true;
    if (named.starts_with("./")) named.remove_prefix(2);
    return source.ends_with(named) && (source.size() == named.size() || source[source.size() - named.size() - 1] == '/');
}

// A section whose lines are code (and so shown): .text and its kinds, Mach-O's __TEXT.
bool code_section(std::string_view d) {
    if (d.starts_with(".text")) return true;
    if (!d.starts_with(".section")) return false;
    const std::string_view name = trim(d.substr(8));
    return name.starts_with(".text") || name.starts_with("__TEXT,__text") || name.starts_with("__TEXT,__textcoal");
}

}  // namespace

// The assembler's comment mark, from the first line that is all comment:
// '#' (x86), ';' (Apple's arm64) or "//" (arm64 elsewhere).
std::string_view comment_mark(std::string_view text) {
    for (size_t at = 0; at < text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view t = trim(text.substr(at, nl - at));
        at = nl + 1;
        if (t.starts_with("//")) return "//";
        if (t.starts_with(";")) return ";";
        if (t.starts_with("#")) return "#";
    }
    return "#";
}

AsmView read_asm(std::string_view text, std::string_view source) {
    AsmView v;
    const std::string_view mark_of = comment_mark(text);
    const char quote_or_mark[] = {'"', mark_of[0], 0};
    std::map<int, std::string> files;
    int file = -1;
    u32 line = 0;
    bool code = true;  // (until a section says otherwise: the start is .text)
    auto mark = [&](u8 st) {
        const u32 at = static_cast<u32>(v.text.size());
        if (!v.style.empty() && v.style.back() == st) return;
        if (!v.off.empty() && v.off.back() == at) v.style.back() = st;
        else v.off.push_back(at), v.style.push_back(st);
    };
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view raw = text.substr(at, nl - at);
        at = nl + 1;
        // (Outside the code, only what says where the code is: most of it is the debug information.)
        if (!code) {
            size_t k = 0;
            while (k < raw.size() && (raw[k] == ' ' || raw[k] == '\t')) ++k;
            if (k >= raw.size() || raw[k] != '.' || (raw.compare(k, 4, ".sec") != 0 && raw.compare(k, 4, ".tex") != 0 && raw.compare(k, 5, ".file") != 0)) continue;
        }
        // The line without its comment (outside quotes), the comment kept to show.
        std::string_view t = trim(raw), note;
        bool quoted = false;
        for (size_t k = 0; (k = t.find_first_of(quote_or_mark, k)) != std::string_view::npos; ++k) {
            if (t[k] == '"' && (k == 0 || t[k - 1] != '\\')) quoted = !quoted;
            else if (!quoted && t.substr(k).starts_with(mark_of)) {
                note = trim(t.substr(k));
                t = trim(t.substr(0, k));
                break;
            }
        }
        if (t.empty()) continue;
        if (t[0] == '.' && t.back() != ':') {  // a directive
            if (t.starts_with(".file")) {
                const std::string_view rest = trim(t.substr(5));
                const int n = number(rest);
                files[n] = quoted_path(rest);
            } else if (t.starts_with(".loc ") || t.starts_with(".loc\t")) {
                const std::string_view rest = trim(t.substr(4));
                file = number(rest);
                const size_t sp = rest.find_first_of(" \t");
                line = sp == std::string_view::npos ? 0 : static_cast<u32>(number(trim(rest.substr(sp))));
            } else if (t.starts_with(".section") || t.starts_with(".text") || t.starts_with(".data") || t.starts_with(".bss") || t.starts_with(".cstring") ||
                       t.starts_with(".const")) {
                code = code_section(t);
            }
            continue;
        }
        if (!code) continue;
        const bool label = t.back() == ':' && t.find_first_of(" \t") == std::string_view::npos;
        if (label) {
            // (The compiler's own labels for the debug information and the unwinding: no place to go.)
            std::string_view name = t;
            if (name.starts_with('.')) name.remove_prefix(1);
            bool debug = (name.starts_with("LBB") || name.starts_with("LBE")) && name.find('_') == std::string_view::npos;  // (GCC's scopes; not Clang's blocks, LBB0_1)
            for (const std::string_view p : {"Ltmp", "Lfunc_begin", "Lfunc_end", "Lcfi", "Ldebug", "Ltext", "Letext", "LVL", "LVU", "LFB", "LFE", "LCFI"}) debug = debug || name.starts_with(p);
            if (debug) continue;
            mark(3);
            v.text += t;
        } else {
            // An instruction: its name, then (from the ninth column) its operands.
            mark(0);
            v.text += "    ";
            const size_t gap = t.find_first_of(" \t");
            const std::string_view name = t.substr(0, gap);
            v.text += name;
            if (gap != std::string_view::npos) {
                v.text.append(name.size() < 8 ? 8 - name.size() : 1, ' ');
                for (const char ch : trim(t.substr(gap))) v.text += ch == '\t' ? ' ' : ch;
            }
        }
        if (!note.empty()) {
            mark(4);
            v.text += "  ";
            v.text += note;
        }
        v.text += '\n';
        const auto f = files.find(file);
        v.line.push_back(!label && f != files.end() && same_file(f->second, source) ? line : 0);
    }
    if (!v.text.empty()) v.text.pop_back();
    return v;
}

bool asm_command(std::string_view compile_commands, std::string_view file, std::vector<std::string>* argv, std::string* dir) {
    json::Doc doc;
    if (!doc.parse(std::string(compile_commands))) return false;
    for (const json::Value e : doc.root()) {
        std::string path(e["file"].str());
        const std::string d(e["directory"].str());
        if (!path.empty() && path[0] != '/') path = d + "/" + path;
        if (path != file) continue;
        std::vector<std::string> words;
        if (e["arguments"].is_array()) {
            for (const json::Value a : e["arguments"]) words.emplace_back(a.str());
        } else {
            // The command as a shell splits it (quotes and backslashes).
            const std::string_view c = e["command"].str();
            std::string w;
            bool any = false;
            char quote = 0;
            for (size_t i = 0; i < c.size(); ++i) {
                const char ch = c[i];
                if (quote) {
                    if (ch == quote) quote = 0;
                    else if (ch == '\\' && quote == '"' && i + 1 < c.size()) w += c[++i];
                    else w += ch;
                } else if (ch == '"' || ch == '\'') {
                    quote = ch, any = true;
                } else if (ch == '\\' && i + 1 < c.size()) {
                    w += c[++i];
                } else if (ch == ' ' || ch == '\t') {
                    if (any || !w.empty()) words.push_back(w);
                    w.clear(), any = false;
                } else {
                    w += ch;
                }
            }
            if (any || !w.empty()) words.push_back(w);
        }
        if (words.empty()) return false;
        // Its object and its dependencies left out; the assembly, with line information, to the standard output.
        argv->clear();
        for (size_t i = 0; i < words.size(); ++i) {
            const std::string& w = words[i];
            if (w == "-o" || w == "-MF" || w == "-MT" || w == "-MQ") {
                ++i;
                continue;
            }
            if (w == "-c" || w == "-MD" || w == "-MMD" || w.starts_with("-o") || w.starts_with("-flto") || w == "-g0") continue;
            argv->push_back(w);
        }
        argv->insert(argv->begin() + 1, {"-S", "-g", "-fno-asynchronous-unwind-tables", "-o", "-"});
        *dir = d;
        return true;
    }
    return false;
}

}  // namespace blit
